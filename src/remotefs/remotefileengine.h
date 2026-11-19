#pragma once

#include <QString>
#include <QStringList>

namespace Ripes {

/// Prefix of the paths served by the remote file system, e.g.
/// "remote:/program.s".
inline const QString RemoteFileSystemPrefix = QStringLiteral("remote:");

/// Registers a Qt file engine handler which serves "remote:/<name>" paths
/// through the HTTP file API of the server hosting Ripes:
///
///   GET  <base>/api/files          list of file names (JSON array)
///   GET  <base>/api/files/<name>   file contents
///   HEAD <base>/api/files/<name>   existence and size of a file
///   PUT  <base>/api/files/<name>   store file contents (text/plain)
///
/// After installation, QFile, QFileInfo and QDir operate transparently on
/// remote paths. Only available in WebAssembly builds, where the local file
/// system is not persistent.
void installRemoteFileSystem();

/// Outcome of a request to the server.
enum class RemoteRequestStatus { Ok, Cancelled, Failed };

/// Lists the files on the server. Unlike QDir::entryList(), which returns an
/// empty list on failure, reports whether the request failed (\p errorString
/// describes the error) or was cancelled by the user.
RemoteRequestStatus listRemoteFiles(QStringList &names, QString &errorString);

} // namespace Ripes
