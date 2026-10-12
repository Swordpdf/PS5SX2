# PS5SX2 build environment: the public dependencies of ps5/README.md, at the revisions it names, in PS5SX2_DEPS.
#
# Copyright (C) 2026 swordpdf
# SPDX-License-Identifier: GPL-3.0-or-later

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      bash ca-certificates cmake curl git make ninja-build python3 unzip xz-utils zlib1g-dev \
      clang-18 lld-18 llvm-18 llvm-18-dev libclang-rt-18-dev \
    && rm -rf /var/lib/apt/lists/*

ENV PS5SX2_DEPS=/opt/ps5sx2-deps
ENV PS5_PAYLOAD_SDK=/opt/ps5sx2-deps/sdk285

# ps5-payload-dev SDK v0.42 (stock: see ps5/README.md for the changes the release build adds to target/lib).
ARG SDK_URL=https://github.com/ps5-payload-dev/sdk/releases/download/v0.42/ps5-payload-sdk.zip
ARG SDK_SHA256=8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da
RUN mkdir -p "$PS5SX2_DEPS" && cd /tmp \
    && curl -fsSL -o sdk.zip "$SDK_URL" \
    && echo "$SDK_SHA256  sdk.zip" | sha256sum -c - \
    && unzip -q sdk.zip && mv ps5-payload-sdk "$PS5_PAYLOAD_SDK" && rm sdk.zip

RUN git clone -q https://github.com/biojppm/rapidyaml.git "$PS5SX2_DEPS/ryml" \
    && git -C "$PS5SX2_DEPS/ryml" checkout -q 0cd25a1 \
    && git -C "$PS5SX2_DEPS/ryml" submodule update -q --init --recursive

RUN git clone -q --depth 1 --branch v2026.2 https://github.com/google/shaderc.git "$PS5SX2_DEPS/shaderc"

RUN git clone -q --depth 1 --branch 16.3.0 https://github.com/KhronosGroup/glslang.git "$PS5SX2_DEPS/glslang" \
    && test "$(git -C "$PS5SX2_DEPS/glslang" rev-parse --short=7 HEAD)" = 275822a \
    && cmake -S "$PS5SX2_DEPS/glslang" -B "$PS5SX2_DEPS/glslang/build-ps5" -G Ninja \
         -DCMAKE_TOOLCHAIN_FILE="$PS5_PAYLOAD_SDK/toolchain/prospero.cmake" -DCMAKE_BUILD_TYPE=Release \
         -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=OFF -DGLSLANG_TESTS=OFF \
    && cmake --build "$PS5SX2_DEPS/glslang/build-ps5"

WORKDIR /src/ps5/coreorbis
