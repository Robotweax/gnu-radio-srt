# Development and qualification image for gr-robotweax-srt.
# Build with tools/dev/build-image.sh, which supplies the pinned SRT source
# as the named build context "srt" (a git archive of one exact commit).
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake ninja-build pkg-config git \
        gnuradio gnuradio-dev \
        python3 python3-dev python3-numpy python3-pybind11 pybind11-dev \
        python3-packaging python3-pytest \
        libssl-dev libspdlog-dev libfmt-dev libgmp-dev libboost-all-dev \
        clang llvm libclang-rt-18-dev iproute2 iputils-ping procps \
    && rm -rf /var/lib/apt/lists/*

# Robotweax SRT, installed into its own prefix with the default namespaced
# layout. Shared library: the OOT module is itself a shared object loaded by
# Python, and exactly one SRT implementation may exist per process.
COPY --from=srt . /opt/src/robotweax-srt
ARG SRT_COMMIT=unknown
RUN cmake -S /opt/src/robotweax-srt -B /opt/build/robotweax-srt -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DROBOTWEAX_SRT_BUILD_TESTS=OFF \
        -DROBOTWEAX_SRT_BUILD_BENCHMARKS=OFF \
        -DROBOTWEAX_SRT_BUILD_TOOLS=OFF \
        -DCMAKE_INSTALL_PREFIX=/opt/robotweax-srt \
    && cmake --build /opt/build/robotweax-srt --parallel \
    && cmake --install /opt/build/robotweax-srt \
    && echo "${SRT_COMMIT}" > /opt/robotweax-srt/SOURCE_COMMIT \
    && rm -rf /opt/build/robotweax-srt

ENV CMAKE_PREFIX_PATH=/opt/robotweax-srt \
    LD_LIBRARY_PATH=/opt/robotweax-srt/lib \
    RWSRT_SRT_PREFIX=/opt/robotweax-srt
WORKDIR /work
