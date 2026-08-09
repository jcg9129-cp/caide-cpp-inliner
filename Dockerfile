# Multi-stage build for the caide C++ inliner `cmd` tool, using the LOCAL source
# tree (the repo this Dockerfile lives in) rather than cloning from GitHub.
#
#   docker build -t caide-inliner .
#   docker run --rm -v "$PWD":/data caide-inliner -std=c++17 -I /data -- -o /data/out.cpp /data/main.cpp
#
# LLVM is pinned (ARG LLVM_VERSION) for reproducibility and because the inliner
# currently supports Clang <= 22. Bump it only in step with the code's support.

# ---------------------------------------------------------------------------
# Stage 1: build — full dev toolchain, produces the `cmd` binary.
# ---------------------------------------------------------------------------
FROM arm64v8/ubuntu:26.04 AS build

ARG LLVM_VERSION=22
ENV TZ=Europe/Berlin
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /src

RUN apt-get update && apt-get upgrade -y \
    && apt-get install -y cmake make g++ wget gnupg lsb-release ca-certificates software-properties-common \
    && wget -O /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh ${LLVM_VERSION} \
    && apt-get install -y \
         "clang-${LLVM_VERSION}" \
         "libclang-${LLVM_VERSION}-dev" \
         "llvm-${LLVM_VERSION}-dev" \
    && rm -rf /var/lib/apt/lists/*

# Bring in the local checkout (see .dockerignore for what is excluded).
COPY . /src

# CAIDE_USE_SYSTEM_CLANG=ON links the installed clang/llvm; -DClang_ROOT points
# find_package(Clang) at the pinned toolchain. (No submodule/LLVM build needed.)
RUN mkdir build && cd build \
    && cmake -G "Unix Makefiles" \
         -DCMAKE_BUILD_TYPE=Release \
         -DCAIDE_USE_SYSTEM_CLANG=ON \
         -DClang_ROOT="/usr/lib/llvm-${LLVM_VERSION}" \
         ../src \
    && make -j"$(nproc)"

# ---------------------------------------------------------------------------
# Stage 2: runtime — only what `cmd` needs to run.
#
# `cmd` links libLLVM / libclang-cpp *dynamically*, so those shared libs must be
# present, but the ~700 MB of -dev static libs and the build tools are dropped.
# It parses real C++ with libTooling, so it also needs clang's builtin resource
# headers (libclang-common-*-dev) and the C++ standard headers. And `-a`
# (auto-detect) shells out to `g++` to discover the include search paths, so a
# compiler with the STL must be present too.
#
# GXX_VERSION selects that g++ (and therefore the libstdc++ headers `-a` feeds to
# clang). Match it to the compiler you build the inlined output with. Newer C++23
# library headers need a recent enough libstdc++ — e.g. <generator> requires
# libstdc++ >= 14. It must still be a libstdc++ that clang (LLVM_VERSION) can
# parse; libstdc++ should not run far ahead of clang.
# ---------------------------------------------------------------------------
FROM arm64v8/ubuntu:26.04 AS runtime

ARG LLVM_VERSION=22
ARG GXX_VERSION=16
ENV TZ=Europe/Berlin
ENV DEBIAN_FRONTEND=noninteractive

WORKDIR /app

RUN apt-get update \
    && apt-get install -y "g++-${GXX_VERSION}" wget gnupg ca-certificates \
    # Make the unversioned `g++`/`gcc` resolve to the chosen version, so `-a`
    # auto-detect (which runs plain `g++`) uses its libstdc++ headers.
    && update-alternatives --install /usr/bin/g++ g++ "/usr/bin/g++-${GXX_VERSION}" 100 \
    && update-alternatives --install /usr/bin/gcc gcc "/usr/bin/gcc-${GXX_VERSION}" 100 \
    # Add the pinned apt.llvm.org repo (codename taken from the base image).
    && wget -qO /etc/apt/trusted.gpg.d/apt.llvm.org.asc https://apt.llvm.org/llvm-snapshot.gpg.key \
    && . /etc/os-release \
    && echo "deb http://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM_VERSION} main" \
         > /etc/apt/sources.list.d/llvm.list \
    && apt-get update \
    # Runtime LLVM/Clang pieces only: the shared libs cmd links + builtin headers.
    && apt-get install -y \
         "libllvm${LLVM_VERSION}" \
         "libclang-cpp${LLVM_VERSION}" \
         "libclang-common-${LLVM_VERSION}-dev" \
    # Drop the transient tools used only to add the repo.
    && apt-get purge -y wget gnupg && apt-get autoremove -y \
    && rm -rf /var/lib/apt/lists/*

# Just the built binary from the build stage.
COPY --from=build /src/build/cmd/cmd /app/cmd

# Default temporary directory used by cmd (matches its default -d ./caide-tmp).
RUN mkdir -p /app/caide-tmp

ENTRYPOINT ["/app/cmd"]
