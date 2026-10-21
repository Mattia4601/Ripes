#pragma once

#include <QString>

namespace Ripes {

/// Prefix of the paths served by the remote file system, e.g.
/// "remote:/program.s".
inline const QString RemoteFileSystemPrefix = QStringLiteral("remote:");

/// Registers a Qt file engine handler which serves "remote:/<name>" paths
/// through the HTTP file API of the server hosting Ripes:
///
///   GET <origin>/api/files          list of file names (JSON array)
///   GET <origin>/api/files/<name>   file contents
///   PUT <origin>/api/files/<name>   store file contents (text/plain)
///
/// After installation, QFile, QFileInfo and QDir operate transparently on
/// remote paths. Only available in WebAssembly builds, where the local file
/// system is not persistent.
void installRemoteFileSystem();

} // namespace Ripes
