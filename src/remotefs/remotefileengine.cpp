#include "remotefileengine.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>

#include <QtCore/private/qabstractfileengine_p.h>

#include <emscripten.h>
#include <emscripten/val.h>

#include <cstdlib>
#include <memory>
#include <optional>

namespace Ripes {

namespace {

// Base URL of the file API, e.g. "http://localhost:8080/api/files". Resolved
// once from the origin of the page hosting Ripes.
QString s_apiBase;

QString resolveApiBase() {
  const auto document = emscripten::val::global("document");
  const QUrl pageUrl(QString::fromStdString(document["URL"].as<std::string>()));
  const QUrl origin = pageUrl.adjusted(QUrl::RemovePath | QUrl::RemoveQuery |
                                       QUrl::RemoveFragment);
  return origin.toString() + QStringLiteral("/api/files");
}

// ---------------------------------------------------------------------------
// Synchronous HTTP through a synchronous XMLHttpRequest.
//
// QAbstractFileEngine is a blocking API, so the response must be available
// before returning. Awaiting an asynchronous request with a nested event loop
// does not work here: file operations are typically issued from within modal
// dialogs, which already hold the single Asyncify suspension, and Qt also
// calls the engine from worker threads (e.g. QFileInfoGatherer). A
// synchronous XHR blocks the calling thread without needing an event loop and
// works on the main thread as well as in workers.
// ---------------------------------------------------------------------------

EM_JS_DEPS(ripes_remotefs, "$UTF8ToString,malloc");

// Returns the HTTP status (0 on network failure). The response body is
// returned in a malloc'ed buffer (*outData, *outSize) to be freed by the
// caller. A negative bodySize means "no request body".
EM_JS(int, ripes_remotefs_xhr,
      (const char *method, const char *url, const char *body, int bodySize,
       char **outData, int *outSize),
      {
        const xhr = new XMLHttpRequest();
        try {
          xhr.open(UTF8ToString(method), UTF8ToString(url), false);
          // Receive the body as raw bytes (one char code per byte).
          xhr.overrideMimeType('text/plain; charset=x-user-defined');
          if (bodySize >= 0) {
            xhr.setRequestHeader('Content-Type', 'text/plain; charset=utf-8');
            // slice() copies out of the (shared) wasm memory.
            xhr.send(HEAPU8.slice(body, body + bodySize));
          } else {
            xhr.send(null);
          }
        } catch (e) {
          return 0;
        }

        const text = xhr.responseText || "";
        const ptr = _malloc(text.length + 1);
        for (let i = 0; i < text.length; ++i)
          HEAPU8[ptr + i] = text.charCodeAt(i) & 0xff;
        HEAP32[outData >> 2] = ptr;
        HEAP32[outSize >> 2] = text.length;
        return xhr.status;
      });

struct HttpResult {
  bool ok = false;
  int status = 0;
  QByteArray data;

  QString errorString() const {
    if (status == 0)
      return QStringLiteral("Network error");
    return QStringLiteral("HTTP error %1").arg(status);
  }
};

HttpResult httpRequest(const char *method, const QString &url,
                       const QByteArray *body = nullptr) {
  const QByteArray urlUtf8 = url.toUtf8();
  char *data = nullptr;
  int size = 0;

  HttpResult result;
  result.status = ripes_remotefs_xhr(
      method, urlUtf8.constData(), body ? body->constData() : nullptr,
      body ? int(body->size()) : -1, &data, &size);
  result.ok = result.status == 200;
  if (data) {
    if (result.ok)
      result.data = QByteArray(data, size);
    std::free(data);
  }
  return result;
}

QString fileUrl(const QString &name) {
  return s_apiBase + u'/' + QString::fromLatin1(QUrl::toPercentEncoding(name));
}

// ---------------------------------------------------------------------------
// Directory iterator for the (flat) remote root.
// ---------------------------------------------------------------------------

class RemoteDirIterator : public QAbstractFileEngineIterator {
public:
  RemoteDirIterator(const QString &path, QDirListing::IteratorFlags filters,
                    const QStringList &nameFilters, QStringList entries)
      : QAbstractFileEngineIterator(path, filters, nameFilters),
        m_entries(std::move(entries)) {}

  bool advance() override {
    if (m_index + 1 >= m_entries.size())
      return false;
    ++m_index;
    return true;
  }

  QString currentFileName() const override {
    return (m_index >= 0 && m_index < m_entries.size()) ? m_entries.at(m_index)
                                                        : QString();
  }

private:
  QStringList m_entries;
  qsizetype m_index = -1;
};

// ---------------------------------------------------------------------------
// File engine. Reads are fetched on open(), writes are buffered and sent on
// close().
// ---------------------------------------------------------------------------

class RemoteFileEngine : public QAbstractFileEngine {
public:
  explicit RemoteFileEngine(const QString &fileName) { setFileName(fileName); }

  void setFileName(const QString &file) override {
    // "remote:/program.s" -> "program.s"; "remote:/" and "remote:" -> ""
    QString name = file.mid(RemoteFileSystemPrefix.size());
    while (name.startsWith(u'/'))
      name.remove(0, 1);
    if (name == u'.')
      name.clear();
    m_name = name;
    m_metadata.reset();
  }

  bool open(QIODevice::OpenMode openMode,
            std::optional<QFile::Permissions> permissions) override {
    Q_UNUSED(permissions);

    if (isRoot()) {
      setError(QFile::OpenError,
               QStringLiteral("The remote root is not a file"));
      return false;
    }

    const bool read = openMode & QIODevice::ReadOnly;
    const bool write = openMode & QIODevice::WriteOnly;
    if (read && !write) {
      const Metadata &meta = metadata();
      if (!meta.exists) {
        setError(QFile::OpenError, meta.error);
        return false;
      }
      m_buffer = meta.content;
    } else if (write && !read && !(openMode & QIODevice::Append)) {
      m_buffer.clear();
    } else {
      setError(QFile::OpenError,
               QStringLiteral("Unsupported open mode for remote files"));
      return false;
    }

    m_openMode = openMode;
    m_pos = 0;
    return true;
  }

  bool close() override {
    const bool wasWriting = m_openMode & QIODevice::WriteOnly;
    m_openMode = QIODevice::NotOpen;
    if (!wasWriting)
      return true;

    m_metadata.reset();
    const HttpResult result = httpRequest("PUT", fileUrl(m_name), &m_buffer);
    if (!result.ok) {
      setError(QFile::WriteError, result.errorString());
      return false;
    }
    return true;
  }

  bool flush() override { return true; }

  qint64 read(char *data, qint64 maxlen) override {
    const qint64 available = m_buffer.size() - m_pos;
    const qint64 count = qMin(maxlen, qMax<qint64>(0, available));
    if (count > 0) {
      memcpy(data, m_buffer.constData() + m_pos, size_t(count));
      m_pos += count;
    }
    return count;
  }

  qint64 write(const char *data, qint64 len) override {
    if (!(m_openMode & QIODevice::WriteOnly))
      return -1;
    m_buffer.replace(m_pos, len, data, len);
    m_pos += len;
    return len;
  }

  qint64 pos() const override { return m_pos; }

  bool seek(qint64 pos) override {
    if (pos < 0 || pos > m_buffer.size())
      return false;
    m_pos = pos;
    return true;
  }

  bool isSequential() const override { return false; }

  qint64 size() const override {
    if (m_openMode != QIODevice::NotOpen)
      return m_buffer.size();
    if (isRoot())
      return 0;
    return metadata().content.size();
  }

  FileFlags fileFlags(FileFlags type) const override {
    Q_UNUSED(type);
    const FileFlags perms =
        ReadOwnerPerm | WriteOwnerPerm | ReadUserPerm | WriteUserPerm;

    if (isRoot())
      return perms | ExeOwnerPerm | ExeUserPerm | DirectoryType | ExistsFlag |
             RootFlag;

    if (!metadata().exists)
      return {};
    return perms | FileType | ExistsFlag;
  }

  QString fileName(FileName file) const override {
    const QString root = RemoteFileSystemPrefix + u'/';
    switch (file) {
    case BaseName:
      return m_name;
    case PathName:
    case AbsolutePathName:
    case CanonicalPathName:
      return root;
    case DefaultName:
    case AbsoluteName:
    case CanonicalName:
      return root + m_name;
    default:
      return QString();
    }
  }

  bool isRelativePath() const override { return false; }
  bool caseSensitive() const override { return true; }

  IteratorUniquePtr beginEntryList(const QString &path,
                                   QDirListing::IteratorFlags filters,
                                   const QStringList &filterNames) override {
    if (!isRoot())
      return {};

    QStringList entries;
    const HttpResult result = httpRequest("GET", s_apiBase);
    if (result.ok) {
      const QJsonArray array = QJsonDocument::fromJson(result.data).array();
      for (const QJsonValue &value : array)
        entries.append(value.toString());
    } else {
      qWarning() << "RemoteFileEngine: cannot list remote files:"
                 << result.errorString();
    }

    // Type and name filters are applied by QDirListing.
    return std::make_unique<RemoteDirIterator>(path, filters, filterNames,
                                               entries);
  }

private:
  struct Metadata {
    bool exists = false;
    QByteArray content;
    QString error;
  };

  bool isRoot() const { return m_name.isEmpty(); }

  // There is no metadata endpoint yet: existence and size are derived from a
  // GET of the file, cached for the lifetime of this engine instance (Qt
  // creates one per QFile/QFileInfo).
  const Metadata &metadata() const {
    if (!m_metadata) {
      const HttpResult result = httpRequest("GET", fileUrl(m_name));
      Metadata meta;
      meta.exists = result.ok;
      meta.content = result.data;
      if (!result.ok)
        meta.error = result.errorString();
      m_metadata = meta;
    }
    return *m_metadata;
  }

  QString m_name;
  QByteArray m_buffer;
  qint64 m_pos = 0;
  QIODevice::OpenMode m_openMode = QIODevice::NotOpen;
  mutable std::optional<Metadata> m_metadata;
};

class RemoteFileEngineHandler : public QAbstractFileEngineHandler {
public:
  std::unique_ptr<QAbstractFileEngine>
  create(const QString &fileName) const override {
    // Called for every QFile/QFileInfo/QDir: must not do any network access.
    if (!fileName.startsWith(RemoteFileSystemPrefix))
      return {};
    return std::make_unique<RemoteFileEngine>(fileName);
  }
};

} // namespace

void installRemoteFileSystem() {
  // The handler registers itself on construction and must outlive every file
  // operation, hence it is never destroyed.
  static RemoteFileEngineHandler *handler = nullptr;
  if (handler)
    return;
  s_apiBase = resolveApiBase();
  handler = new RemoteFileEngineHandler;
}

} // namespace Ripes
