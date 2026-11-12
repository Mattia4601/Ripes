#include "remotefileengine.h"

#include <QApplication>
#include <QDebug>
#include <QHash>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProgressDialog>
#include <QTimer>
#include <QUrl>

#include <QtCore/private/qabstractfileengine_p.h>

#include <emscripten.h>
#include <emscripten/eventloop.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>
#include <emscripten/val.h>

#include <pthread.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>

namespace Ripes {

namespace {

// Base URL of the file API, e.g. "http://localhost:8080/api/files". Resolved
// once from the folder of the page hosting Ripes, so that the API is reached
// also when the application is served under a path prefix, as on CrownLabs.
QString s_apiBase;

QString resolveApiBase() {
  const auto document = emscripten::val::global("document");
  const QUrl pageUrl(QString::fromStdString(document["URL"].as<std::string>()));
  const QUrl folder = pageUrl.adjusted(QUrl::RemoveFilename | QUrl::RemoveQuery |
                                       QUrl::RemoveFragment);
  return folder.toString() + QStringLiteral("api/files");
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

// Same contract as ripes_remotefs_xhr, through fetch(). EM_ASYNC_JS suspends
// the calling C++ code with Asyncify until the returned promise resolves.
// The request is aborted when *cancel becomes non-zero (checked every 50 ms)
// or after timeoutMs milliseconds. *outContentLength receives the
// Content-Length header (-1 if absent), which is how HEAD requests report the
// size of a file.
EM_ASYNC_JS(int, ripes_remotefs_fetch,
            (const char *method, const char *url, const char *body,
             int bodySize, char **outData, int *outSize, int *cancel,
             int timeoutMs, int *outContentLength),
            {
              const controller = new AbortController();
              const start = Date.now();
              const watchdog = setInterval(() => {
                if (Atomics.load(HEAP32, cancel >> 2) !== 0 ||
                    Date.now() - start > timeoutMs)
                  controller.abort();
              }, 50);
              try {
                const init = {
                  method : UTF8ToString(method),
                  signal : controller.signal
                };
                if (bodySize >= 0) {
                  init.headers = {'Content-Type' : 'text/plain; charset=utf-8'};
                  // slice() copies out of the (shared) wasm memory.
                  init.body = HEAPU8.slice(body, body + bodySize);
                }
                const response = await fetch(UTF8ToString(url), init);
                const length = response.headers.get('Content-Length');
                HEAP32[outContentLength >> 2] =
                    length === null ? -1 : parseInt(length, 10);
                const bytes = new Uint8Array(await response.arrayBuffer());
                const ptr = _malloc(bytes.length + 1);
                HEAPU8.set(bytes, ptr);
                HEAP32[outData >> 2] = ptr;
                HEAP32[outSize >> 2] = bytes.length;
                return response.status;
              } catch (e) {
                console.error('ripes_remotefs_fetch', e);
                return 0;
              } finally {
                clearInterval(watchdog);
              }
            });

struct HttpResult {
  bool ok = false;
  int status = 0;
  QByteArray data;
  qint64 contentLength = -1;

  QString errorString() const {
    if (status == 0)
      return QStringLiteral("Network error");
    return QStringLiteral("HTTP error %1").arg(status);
  }
};

// Thread running the HTTP requests. Browser events reach Qt only on the main
// thread, so here the Asyncify suspension of ripes_remotefs_fetch cannot be
// interleaved with other calls into the application.
pthread_t s_httpThread;

void *httpThreadMain(void *) {
  // Keep the thread alive after returning: it goes back to its event loop
  // and executes the requests proxied to it.
  emscripten_runtime_keepalive_push();
  return nullptr;
}

// Maximum duration of a request.
constexpr int RequestTimeoutMs = 10000;
// Delay before the waiting dialog is shown, so that it does not flash for
// the (usually very short) requests.
constexpr int WaitDialogDelayMs = 300;

struct HttpTask {
  QByteArray method;
  QByteArray url;
  QByteArray body;
  bool hasBody = false;
  int status = 0;
  char *data = nullptr;
  int size = 0;
  int contentLength = -1;
  // Written by the main thread, read by the fetch watchdog.
  std::atomic<int> cancel{0};
  // Written by s_httpThread when the request is complete.
  std::atomic<bool> done{false};
};

// Executed on s_httpThread.
void runHttpTask(HttpTask *task) {
  task->status = ripes_remotefs_fetch(
      task->method.constData(), task->url.constData(),
      task->hasBody ? task->body.constData() : nullptr,
      task->hasBody ? int(task->body.size()) : -1, &task->data, &task->size,
      reinterpret_cast<int *>(&task->cancel), RequestTimeoutMs,
      &task->contentLength);
  task->done = true;
}

void runHttpTaskSync(void *arg) { runHttpTask(static_cast<HttpTask *>(arg)); }

void runHttpTaskAsync(void *arg) {
  // Keeps the task alive until the request is complete.
  auto *task = static_cast<std::shared_ptr<HttpTask> *>(arg);
  runHttpTask(task->get());
  delete task;
}

// Waits on the main thread for the request to complete, without blocking
// the browser: a local event loop is run (suspended through Asyncify by Qt),
// and after WaitDialogDelayMs a modal dialog lets the user cancel.
void waitOnMainThread(HttpTask &task) {
  QTimer poll;
  poll.setInterval(20);

  // First phase: user input is deferred, so that no other operation is
  // started while waiting.
  {
    QEventLoop loop;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
      if (task.done)
        loop.quit();
    });
    QTimer::singleShot(WaitDialogDelayMs, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    poll.stop();
  }
  if (task.done)
    return;

  // Second phase: the request takes longer, show a modal dialog. Opened with
  // open() and not exec(), since the event loop below already suspends.
  auto *dialog = new QProgressDialog(QObject::tr("Waiting for the server..."),
                                     QObject::tr("Cancel"), 0, 0,
                                     QApplication::activeWindow());
  dialog->setWindowModality(Qt::ApplicationModal);
  dialog->setMinimumDuration(0);
  QEventLoop loop;
  QObject::connect(dialog, &QProgressDialog::canceled, &loop,
                   [&] { task.cancel = 1; });
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (task.done)
      loop.quit();
  });
  dialog->open();
  poll.start();
  loop.exec();
  poll.stop();

  // Close the dialog while it is still intact, so that the focus moves back
  // to the window below, and delete it later from the event loop: destroying
  // it right away, while open and focused, crashes in the focus handling.
  QObject::disconnect(dialog, nullptr, &loop, nullptr);
  dialog->close();
  dialog->deleteLater();
}

HttpResult httpRequest(const char *method, const QString &url,
                       const QByteArray *body = nullptr) {
  HttpResult result;

  // Requests are executed one at a time. A request started on the main
  // thread while waiting for another one (re-entrancy through the local
  // event loop) is rejected.
  static std::mutex mutex;
  std::unique_lock<std::mutex> lock(mutex, std::try_to_lock);
  const bool onMainThread = emscripten_is_main_runtime_thread();
  if (!lock.owns_lock()) {
    if (onMainThread)
      return result;
    lock.lock();
  }

  auto task = std::make_shared<HttpTask>();
  task->method = method;
  task->url = url.toUtf8();
  if (body) {
    task->body = *body;
    task->hasBody = true;
  }

  if (onMainThread) {
    // Start the request on s_httpThread without waiting for it.
    auto *arg = new std::shared_ptr<HttpTask>(task);
    if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                                s_httpThread, runHttpTaskAsync, arg)) {
      delete arg;
      return result;
    }
    waitOnMainThread(*task);
  } else {
    // Secondary threads may block: run the request and wait for it.
    if (!emscripten_proxy_sync(emscripten_proxy_get_system_queue(),
                               s_httpThread, runHttpTaskSync, task.get()))
      return result;
  }

  result.status = task->status;
  result.ok = result.status == 200;
  result.contentLength = task->contentLength;
  if (task->data) {
    if (result.ok)
      result.data = QByteArray(task->data, task->size);
    std::free(task->data);
  }
  return result;
}

QString fileUrl(const QString &name) {
  return s_apiBase + u'/' + QString::fromLatin1(QUrl::toPercentEncoding(name));
}

// ---------------------------------------------------------------------------
// Directory iterator for the (flat) remote root.
// ---------------------------------------------------------------------------

// Names returned by the listings currently being iterated. While a listing
// is iterated, Qt creates an engine for each entry and asks whether it exists:
// the answer is taken from here instead of asking the server again. The names
// are removed when the iteration ends, so no information outlives the listing.
std::mutex s_listedNamesMutex;
QHash<QString, int> s_listedNames;

bool isListed(const QString &name) {
  const std::lock_guard<std::mutex> lock(s_listedNamesMutex);
  return s_listedNames.contains(name);
}

class RemoteDirIterator : public QAbstractFileEngineIterator {
public:
  RemoteDirIterator(const QString &path, QDirListing::IteratorFlags filters,
                    const QStringList &nameFilters, QStringList entries)
      : QAbstractFileEngineIterator(path, filters, nameFilters),
        m_entries(std::move(entries)) {
    const std::lock_guard<std::mutex> lock(s_listedNamesMutex);
    for (const QString &name : std::as_const(m_entries))
      ++s_listedNames[name];
  }

  ~RemoteDirIterator() override {
    const std::lock_guard<std::mutex> lock(s_listedNamesMutex);
    for (const QString &name : std::as_const(m_entries)) {
      auto it = s_listedNames.find(name);
      if (it != s_listedNames.end() && --it.value() == 0)
        s_listedNames.erase(it);
    }
  }

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
    m_info.reset();
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
      // The content is always fetched when the file is opened.
      const HttpResult result = httpRequest("GET", fileUrl(m_name));
      if (!result.ok) {
        setError(QFile::OpenError, result.errorString());
        return false;
      }
      m_buffer = result.data;
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

    m_info.reset();
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
    return info().size;
  }

  FileFlags fileFlags(FileFlags type) const override {
    Q_UNUSED(type);
    const FileFlags perms =
        ReadOwnerPerm | WriteOwnerPerm | ReadUserPerm | WriteUserPerm;

    if (isRoot())
      return perms | ExeOwnerPerm | ExeUserPerm | DirectoryType | ExistsFlag |
             RootFlag;

    // The remote file system is flat: every other existing name is a file.
    if (!isListed(m_name) && !info().exists)
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
  struct Info {
    bool exists = false;
    qint64 size = 0;
  };

  bool isRoot() const { return m_name.isEmpty(); }

  // Existence and size of the file, from a HEAD request (no content is
  // transferred). Kept for the lifetime of this engine instance only (Qt
  // creates one per QFile/QFileInfo), so that e.g. exists() followed by
  // size() does not repeat the request.
  const Info &info() const {
    if (!m_info) {
      const HttpResult result = httpRequest("HEAD", fileUrl(m_name));
      Info info;
      info.exists = result.ok;
      info.size = qMax<qint64>(0, result.contentLength);
      m_info = info;
    }
    return *m_info;
  }

  QString m_name;
  QByteArray m_buffer;
  qint64 m_pos = 0;
  QIODevice::OpenMode m_openMode = QIODevice::NotOpen;
  mutable std::optional<Info> m_info;
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
  pthread_create(&s_httpThread, nullptr, httpThreadMain, nullptr);
  handler = new RemoteFileEngineHandler;
}

} // namespace Ripes
