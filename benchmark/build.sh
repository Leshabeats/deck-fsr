#!/bin/sh
set -eu

project=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
revision=c64f093404125e960813f95f1996c14780f0dd69
digest=85e43a54aafd56dcca2ae7d165874bb764d27bb69c7046f9afcee71f1cc6399d
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
mkdir "$build/source"
if [ "$#" -eq 1 ]; then
    cp -- "$1" "$build/upstream.tar.gz"
elif [ "$#" -eq 0 ]; then
    curl --fail --location --silent --show-error \
        "https://codeload.github.com/vladtepes22/FSR-Vulkan/tar.gz/$revision" \
        -o "$build/upstream.tar.gz"
else
    printf 'Usage: %s [fsr-vulkan.tar.gz]\n' "$0" >&2
    exit 2
fi
printf '%s  %s\n' "$digest" "$build/upstream.tar.gz" | sha256sum -c -
tar -xzf "$build/upstream.tar.gz" -C "$build/source" --strip-components=1
cmake -S "$project" -B "$build/bin" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DFSR_SOURCE="$build/source"
cmake --build "$build/bin" --parallel "${BUILD_JOBS:-4}" --target deck-fsr-benchmark
cp -- "$build/bin/deck-fsr-benchmark" "$project/deck-fsr-benchmark"
