# syntax=docker/dockerfile:1

# Image with the WebAssembly build of Ripes and the server for its remote file
# system. The build context is the root of the repository, see README.md:
#   docker build -f docker/remote-fs/Dockerfile -t ripes-remote-fs .

# ---------------------------------------------------------------------------
# Stage 1: build Ripes for WebAssembly, as in .github/workflows/wasm-release.yml.
# The output does not depend on the platform, so this stage runs on the
# architecture of the build machine.
# ---------------------------------------------------------------------------
FROM --platform=$BUILDPLATFORM python:3.12-slim-trixie AS ripes-build

ARG BUILDARCH
ARG QT_VERSION=6.10.3
ARG EMSDK_VERSION=4.0.7

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        git make xz-utils bzip2 ca-certificates libglib2.0-0t64 \
    && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir aqtinstall cmake

# Emscripten, in the version required by Qt for WebAssembly
RUN git clone --depth 1 --branch ${EMSDK_VERSION} \
        https://github.com/emscripten-core/emsdk.git /opt/emsdk \
    && /opt/emsdk/emsdk install ${EMSDK_VERSION} \
    && /opt/emsdk/emsdk activate ${EMSDK_VERSION}

# Qt for WebAssembly (multithread) and the desktop Qt of the build machine,
# which provides the host tools (moc, rcc, uic)
RUN if [ "$BUILDARCH" = "arm64" ]; then HOST=linux_arm64; ARCH=linux_gcc_arm64; \
    else HOST=linux; ARCH=linux_gcc_64; fi \
    && aqt install-qt ${HOST} desktop ${QT_VERSION} ${ARCH} -m qtcharts -O /opt/Qt \
    && aqt install-qt all_os wasm ${QT_VERSION} wasm_multithread -m qtcharts -O /opt/Qt

COPY . /src/Ripes

# emsdk_env.sh must be sourced from bash to find its own location
SHELL ["/bin/bash", "-c"]
WORKDIR /build
RUN source /opt/emsdk/emsdk_env.sh \
    && export QT_HOST_PATH=$(ls -d /opt/Qt/${QT_VERSION}/gcc_*) \
    && cmake                                            \
        -DCMAKE_BUILD_TYPE=Release                      \
        -DRIPES_WITH_QPROCESS=OFF                       \
        -DRIPES_WITH_REMOTE_FS=ON                       \
        -DQT_HOST_PATH="${QT_HOST_PATH}"                \
        -DCMAKE_TOOLCHAIN_FILE=/opt/Qt/${QT_VERSION}/wasm_multithread/lib/cmake/Qt6/qt.toolchain.cmake \
        -DCMAKE_PREFIX_PATH=/opt/Qt/${QT_VERSION}/wasm_multithread/ \
        /src/Ripes \
    && cmake --build . --target Ripes --parallel "$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: the server, with the files of Ripes built in the previous stage.
# ---------------------------------------------------------------------------
FROM node:22-alpine

# User running the server and folder of the files (usually a mounted volume).
ARG UID=1000
ARG GID=1000
ARG DATA_PATH=/data

WORKDIR /app

COPY docker/remote-fs/package.json docker/remote-fs/package-lock.json ./
RUN npm ci --omit=dev

COPY docker/remote-fs/server.js ./
COPY --from=ripes-build /build/Ripes.html /build/Ripes.js /build/Ripes.wasm \
    /build/qtloader.js /build/qtlogo.svg ./static/

RUN mkdir -p ${DATA_PATH} && chown ${UID}:${GID} ${DATA_PATH}
USER ${UID}:${GID}

ENV RIPES_LISTEN_PORT=8080
ENV RIPES_DATA_PATH=${DATA_PATH}
EXPOSE 8080

# Arguments of "docker run" are passed to the server (see README.md).
ENTRYPOINT ["node", "server.js"]
