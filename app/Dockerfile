# Build image for the native TV app: the PS5 app, PC previews and packaging.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      bash ca-certificates curl wget git unzip tar xz-utils zstd make ninja-build ccache pkg-config \
      clang clang-18 lld-18 llvm-18 libclang-rt-18-dev clang-format-18 clang-tidy-18 \
      python3 python3-venv python3-pil \
      libegl-dev libgl-dev mesa-common-dev libegl1 libegl-mesa0 libgl1-mesa-dri libwebp-dev \
      dotnet-sdk-8.0 \
    && rm -rf /var/lib/apt/lists/*
# The console is x86-64. The OpenGL link group needs compiler-rt builtins for that
# architecture, which the compiler-rt package of an arm64 host does not contain.
RUN set -eu; builtins=/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a; \
    if [ ! -s "$builtins" ]; then \
      deb=libclang-rt-18-dev_18.1.3-1ubuntu1_amd64.deb; \
      curl -fsSL -o /tmp/rt.deb "http://archive.ubuntu.com/ubuntu/pool/universe/l/llvm-toolchain-18/$deb"; \
      echo '82d5d27d12c0abdf4cefbafa6044e7544a4cbbbec9c04784f5ce326979842b2b  /tmp/rt.deb' | sha256sum -c -; \
      mkdir /tmp/rt && cd /tmp/rt && llvm-ar-18 x /tmp/rt.deb; \
      tar -xf data.tar.* -C / ".$builtins"; \
      cd / && rm -rf /tmp/rt /tmp/rt.deb; \
    fi; test -s "$builtins"
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1
WORKDIR /work/app
