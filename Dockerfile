# ==============================================================================
# Dockerfile — HFT Backtesting Engine
# ==============================================================================
# Multi-stage build:
#   Stage 1 (builder): Compiles the engine with full optimisations.
#   Stage 2 (runtime): Minimal image with MPI + SSH for inter-pod communication.
#
# Build:
#   docker build -t hft-engine:latest .
#
# The runtime image exposes port 22 (SSH) for MPI inter-pod transport and
# port 9090 for the Prometheus metrics endpoint.
# ==============================================================================

# ─────────────────────────────────────────────────────────────────────────────
# Stage 1 — Builder
# ─────────────────────────────────────────────────────────────────────────────
FROM ubuntu:22.04 AS builder

# Prevent interactive prompts during apt-get install
ENV DEBIAN_FRONTEND=noninteractive

# Install all build-time dependencies in a single layer to minimise image size.
# - cmake / ninja-build:  build system
# - g++-12:               C++20-capable compiler
# - libopenmpi-dev:       MPI headers + libraries
# - openmpi-bin:          mpirun / mpiexec
# - libflatbuffers-dev:   FlatBuffers serialisation
# - qt6-base-dev:         Qt6 Widgets (GUI)
# - qt6-declarative-dev:  Qt6 QML (GUI)
# - libnuma-dev:          NUMA topology headers
# - libgtest-dev:         Google Test framework
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake \
        ninja-build \
        g++-12 \
        libopenmpi-dev \
        openmpi-bin \
        libflatbuffers-dev \
        qt6-base-dev \
        qt6-declarative-dev \
        libnuma-dev \
        libgtest-dev \
    && rm -rf /var/lib/apt/lists/*

# Use g++-12 as the default C++ compiler so CMake picks it up automatically.
ENV CXX=g++-12
ENV CC=gcc-12

WORKDIR /build

# Copy the entire source tree into the build context.
COPY . .

# Configure and build with:
#   -O3             : maximum optimisation
#   -march=x86-64-v3: target AVX2 / BMI2 capable CPUs (broadwell+)
#   Ninja:           fastest parallel build generator
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_FLAGS="-O3 -march=x86-64-v3" \
    && cmake --build build --parallel

# ─────────────────────────────────────────────────────────────────────────────
# Stage 2 — Runtime
# ─────────────────────────────────────────────────────────────────────────────
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive

# Install only the runtime libraries needed to execute the binary.
# - libopenmpi3:         MPI runtime shared libraries
# - openmpi-bin:         mpirun / mpiexec for the launcher pod
# - openssh-server:      sshd for MPI inter-pod communication
# - qt6-base-dev:        Qt6 runtime (includes libs)
# - qt6-declarative-dev: Qt6 QML runtime
# - libnuma1:            NUMA runtime library
RUN apt-get update && apt-get install -y --no-install-recommends \
        libopenmpi3 \
        openmpi-bin \
        openssh-server \
        qt6-base-dev \
        qt6-declarative-dev \
        libnuma1 \
    && rm -rf /var/lib/apt/lists/*

# ── SSH setup for MPI inter-pod communication ─────────────────────────────
# MPI uses SSH to spawn processes on remote worker pods.  We configure a
# trivial root password here; in production this would be replaced by
# SSH key-based auth injected via Kubernetes secrets.
RUN mkdir -p /var/run/sshd \
    && echo 'root:mpipass' | chpasswd \
    && sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config \
    && sed -i 's/#StrictModes yes/StrictModes no/' /etc/ssh/sshd_config \
    && sed -i 's/UsePAM yes/UsePAM no/' /etc/ssh/sshd_config

# Copy the compiled binary from the builder stage.
COPY --from=builder /build/build/hft-engine /usr/local/bin/hft-engine

# Copy the Kubernetes entrypoint script.
COPY k8s/entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

# SSH port for MPI transport + Prometheus metrics port
EXPOSE 22 9090

ENTRYPOINT ["/entrypoint.sh"]
