# Ripes in the browser with a remote file system

In the regular WebAssembly build of Ripes, *Load Program*, *Save File* and
*Save File As...* are disabled: the browser does not give Ripes access to the
files of the user, and only the program in the editor is kept, in the storage
of the browser. With the CMake option `RIPES_WITH_REMOTE_FS`, these actions are
enabled and Ripes loads and saves source files on the server that hosts it.
This folder contains that server and a Dockerfile that builds Ripes with the
option enabled and serves it.

The option is off by default: the regular WebAssembly build of Ripes is
unchanged and needs no server.

## Build the image

The image is built from the sources of this repository, so the build context
is the root of the repository and not this folder. From the root, with the
submodules checked out (`git submodule update --init --recursive`):

```bash
docker build -f docker/remote-fs/Dockerfile -t ripes-remote-fs .
```

To build the image for another architecture, add for example
`--platform linux/amd64`.

Build arguments:

| Argument | Default | Description |
|---|---|---|
| `QT_VERSION` | `6.9.3` | Qt version used to build Ripes |
| `EMSDK_VERSION` | `3.1.70` | Emscripten version (must match the Qt version) |
| `UID`, `GID` | `1000` | User and group running the server |
| `DATA_PATH` | `/data` | Folder where the files are stored |

### What the Dockerfile does

The build has two stages.

1. `ripes-build`, which runs on the architecture of the build machine, since
   its output does not depend on it:
   - installs the build tools, and `aqtinstall` and `cmake` with pip;
   - installs Emscripten 3.1.70, cloning `emsdk` at the tag of the same
     version;
   - installs with `aqtinstall` Qt for WebAssembly (`wasm_multithread`, with
     Qt Charts) and the desktop Qt of the build machine, which provides the
     host tools (`moc`, `rcc`, `uic`);
   - copies the sources from the build context, then configures and builds
     Ripes with the CMake options of `.github/workflows/wasm-release.yml`,
     plus `-DRIPES_WITH_REMOTE_FS=ON`.
2. The final image, based on `node:22-alpine`:
   - installs the dependencies of the server (`npm ci`) and copies
     `server.js`;
   - copies from the first stage the files the browser needs: `Ripes.html`,
     `Ripes.js`, `Ripes.wasm`, `qtloader.js` and `qtlogo.svg`;
   - creates the data folder, owned by `UID`/`GID`, and runs the server as
     that user.

Compared with `wasm-release.yml`, the sources are copied instead of checked
out, ccache is not used, and the number of parallel jobs is limited to the
number of CPUs: without a limit, Make starts an unbounded number of jobs and
the build can run out of memory.

## Run

```bash
docker run --rm -p 8080:8080 -v ripes-data:/data ripes-remote-fs
```

Ripes is then available at <http://localhost:8080/>, and the files saved from
Ripes are stored in the `ripes-data` volume. A folder of the host can be
mounted instead of a volume (e.g. `-v /path/to/files:/data`); in that case it
must be writable by the user running the server (`UID`, `GID`).

The server is configured with environment variables:

| Variable | Default | Description |
|---|---|---|
| `RIPES_LISTEN_PORT` | `8080` | Port the server listens on |
| `RIPES_DATA_PATH` | `/data` in the image, `data` next to `server.js` otherwise | Folder where the files are stored |
| `RIPES_BASE_PATH` | empty | Path prefix under which Ripes is served, e.g. `ripes` for `http://host/ripes/` |

`RIPES_BASE_PATH` is needed when a reverse proxy forwards a sub-path to the
server without removing it. Ripes computes the address of the file API from
the folder of its page, so the page must be loaded from a URL ending with a
slash; the server redirects `/ripes` to `/ripes/`.

## Run without Docker

Build Ripes for WebAssembly as in `wasm-release.yml`, adding
`-DRIPES_WITH_REMOTE_FS=ON` to the CMake options. Then copy `Ripes.html`,
`Ripes.js`, `Ripes.wasm`, `qtloader.js` and `qtlogo.svg` from the build folder
to a `static` folder next to `server.js`, and start the server:

```bash
cd docker/remote-fs
npm ci
node server.js
```

## How it works

### Overview

```
Browser                                         Server (server.js)
┌──────────────────────────────────────┐        ┌──────────────────────────┐
│ Ripes (WebAssembly)                  │        │ static files of Ripes    │
│  LoadDialog / SaveDialog / MainWindow│        │                          │
│        │ QFile("remote:/prog.s")     │  HTTP  │ GET/HEAD/PUT api/files   │
│        ▼                             │ ─────► │        │                 │
│  remote file engine (src/remotefs)   │        │        ▼                 │
│        │ fetch() on a worker thread  │        │ RIPES_DATA_PATH/prog.s   │
└──────────────────────────────────────┘        └──────────────────────────┘
```

Qt lets an application register a *file engine* for its own kind of paths. In
Ripes, `src/remotefs` registers one for the paths starting with `remote:/`.
Ripes keeps reading and writing files with `QFile`, `QFileInfo` and `QDir`: for
these paths, Qt uses this engine instead of its default one, and the operations
are translated into HTTP requests to the server that hosts Ripes.

### The build option

Everything is behind the CMake option `RIPES_WITH_REMOTE_FS` (off by default),
which defines the macro of the same name. If the option is enabled in a build
that is not for WebAssembly (e.g. a desktop build), CMake stops with an error,
since the feature is only meant for the browser. The engine is built on the
private Qt API `QAbstractFileEngine` (`qabstractfileengine_p.h`), so the option
also links `Qt6::CorePrivate`; this API may change between Qt versions.

Changes to the existing code, all inside `#ifdef RIPES_WITH_REMOTE_FS` except
the last one:

| File | Change |
|---|---|
| `CMakeLists.txt`, `src/CMakeLists.txt` | The option, `CorePrivate` and the `src/remotefs` library |
| `main.cpp` | `installRemoteFileSystem()` before the main window is created |
| `src/mainwindow.cpp` | *Load Program*, *Save File* and *Save File As...* enabled; load and save dialogs opened with `open()`; file actions queued (see below) |
| `src/loaddialog.cpp` | Only source files can be loaded (ELF and flat binary disabled); *Open...* shows the remote file selector |
| `src/savedialog.cpp` | Binary output disabled; default path `remote:/`; the browse button shows the remote file selector |
| `src/mainwindow.cpp` (`writeTextFile`, `writeBinaryFile`) | The result of a save is checked after `close()`, when the remote engine sends the file. This also applies to other builds, where it only reports errors that were ignored before |

New files, in `src/remotefs`:

- `remotefileengine.h`, `remotefileengine.cpp`: the file engine, the directory
  listing and the HTTP requests;
- `remotefiledialog.h`, `remotefiledialog.cpp`: the file selector.

### The file engine

`installRemoteFileSystem()` computes the address of the API from the folder of
the page (`http://host/ripes/` → `http://host/ripes/api/files`), starts the
thread that runs the requests and registers a `QAbstractFileEngineHandler`.
Qt calls its `create()` for every `QFile`, `QFileInfo` and `QDir`: it returns a
`RemoteFileEngine` for the paths starting with `remote:`, and nothing for the
others, which are left to the default engine. `create()` never accesses the
network.

The remote file system is flat: `remote:/` is the only directory and
`remote:/<name>` are the files in it. The operations are mapped as follows:

| Qt operation | Engine | Request |
|---|---|---|
| `QFile::open(ReadOnly)` | `open()` reads the whole file into a buffer; `read()` and `seek()` work on the buffer | `GET api/files/<name>` |
| `QFile::open(WriteOnly)`, `write()` | the data is collected in a buffer | none |
| `QFile::close()` after writing | `close()` sends the buffer, and fails if the server does | `PUT api/files/<name>` |
| `QFileInfo::exists()`, `size()`, `isFile()` | `fileFlags()`, `size()`; the answer is kept for the lifetime of the engine | `HEAD api/files/<name>` |
| `QDir("remote:/").entryList()` | `beginEntryList()` returns an iterator over the names | `GET api/files` |

While a listing is iterated, Qt creates a `QFileInfo`, and therefore a new
engine, for every name, and asks whether it exists. To avoid one `HEAD` request
per file, the names of the listings being iterated are kept in a table
(`s_listedNames`): an engine whose name is in the table answers that the file
exists without asking the server. The names are removed when the iterator is
destroyed.

### Requests

`QAbstractFileEngine` is a blocking API: `open()`, `close()` and the other
functions must return with the result. The browser only offers asynchronous
requests, and the main thread of the page must not block, or the page stops
responding. Requests are handled as follows (`httpRequest()` in
`remotefileengine.cpp`):

- The HTTP request is always executed on a dedicated thread, created by
  `installRemoteFileSystem()`. There, `fetch()` is called inside an
  `EM_ASYNC_JS` function, which suspends that thread with Asyncify until the
  response arrives.
- When the engine is used from the main thread, which is the usual case
  (dialogs, load and save), the main thread hands the request to the dedicated
  thread and waits in a local `QEventLoop` until it is complete. Qt suspends
  the main thread in its own way, so the page keeps responding. During the
  first 300 ms, clicks and key presses are held back and delivered after the
  request; if the request takes longer, a modal dialog appears, from which the
  user can only cancel it. Requests also time out after 10 seconds.
- When the engine is used from another thread, that thread can block: it
  waits for the dedicated thread to complete the request.

Calling the `EM_ASYNC_JS` function directly on the main thread would be
simpler, but that suspension is not known to Qt: Qt would consider itself idle
and keep handling events while the caller is suspended halfway. In tests this
caused two problems:

- objects scheduled with `deleteLater()` were deleted while still in use: the
  load dialog (`WA_DeleteOnClose`) was destroyed while the code that had
  loaded the file was still reading its parameters;
- a click during a slow request started a second suspension, and Asyncify,
  which supports one at a time, resumed the wrong code and crashed.

The dedicated thread receives no browser events, so nothing can run in the
middle of its suspension; the wait on the main thread is a suspension Qt knows
about, which postpones deferred deletions.

The comments in `src/remotefs/remotefileengine.cpp` explain these choices in
more detail.

### Dialogs and file actions

The wait described above suspends the application, which affects how dialogs
and file actions are started:

- the load and save dialogs of `MainWindow`, the remote file selector and its
  messages are shown with `open()` instead of `exec()`. The local event loop of
  `exec()` would be a second suspension, while Asyncify supports one at a
  time;
- *Load Program*, *Save File* and *Save File As...* are connected with
  `Qt::QueuedConnection`. With this connection, triggering an action does not
  call its function right away: Qt posts an event and calls the function
  later from the event loop, after the code that triggered the action has
  returned.

  This is needed for keyboard shortcuts. The browser runs the default action
  of a key (for example "Save page" for Ctrl+S) unless the page calls
  `preventDefault()` on the key event before its event handler returns. Qt
  calls it only after handling the key, which includes running the function
  of the shortcut. A request started inside that function suspends the
  application and returns control to the browser before `preventDefault()` is
  called, so the browser runs its default action. With the queued connection,
  the request starts only after the key event has been handled and
  `preventDefault()` has been called.

`QFileDialog` is not used on `remote:/`. Its `QFileSystemModel` queries the
engine from a worker thread for every file and attribute, which results in a
large number of requests. `RemoteFileDialog` is a minimal selector instead: it
lists the folder once with `GET api/files`, filters the names by extension and
lets the user pick a file or, when saving, type a new name.

### Errors

- Listing: if the request fails, the selector does not open and a message
  shows the reason (server not reachable, HTTP error, timeout); if the user
  cancels it, the selector does not open and no message is shown.
- Save: Ripes shows "Error when saving file" with the reason, including
  "Request cancelled".
- Load: Ripes shows its usual "Could not open file" message.

### The server

`server.js` is a small Express application. It stores the files in a single
folder and provides this API, relative to the folder of the page:

| Request | Response |
|---|---|
| `GET api/files` | JSON array with the names of the `.s` and `.c` files |
| `GET api/files/<name>` | Content of the file (404 if it does not exist) |
| `HEAD api/files/<name>` | As `GET`, without the content: used for existence and size |
| `PUT api/files/<name>` | Creates or overwrites the file with the body of the request |

Invalid names (see the limitations) are rejected with 400. The server also
serves the files of Ripes, with the headers
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`: the multithreaded WebAssembly
build needs `SharedArrayBuffer`, which browsers enable only for cross-origin
isolated pages.

## Limitations

- Only C and assembly source files (`.s`, `.c`) can be stored, in a single
  folder without subfolders. File names may contain letters, digits, `-` and
  `_`. ELF and flat binary files cannot be loaded from or saved to the server.
- The WebAssembly build of Ripes has no C compiler, so C input is disabled:
  programs are always saved as assembly (`.s`), and a `.c` file on the server
  is not loaded: Ripes shows its "No C compiler set" warning, as in a desktop
  build without a compiler. `.c` files can only be added to the folder from
  outside Ripes. The server still lists and stores them, so that programs made
  of C and assembly sources can be supported if C compilation becomes
  available in the WebAssembly build, for example on the server.
- The content of a file is read when it is opened. Changes made to the same
  file from another tab or by another user are not shown until the file is
  loaded again, and the last save overwrites the previous ones.
- The server has no authentication: anyone who can reach it can read and write
  the files. It is meant to run behind a platform that controls the access, or
  locally.
- Ripes sends one request at a time.
