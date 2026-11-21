# Vulkan benchmark

The benchmark runs FSR 3.1.4 Super Resolution on Linux x86-64. It generates linear HDR color `(0.25, 0.5, 0.75, 1)`, constant depth `0.5`, and zero motion and jitter. Auto exposure is enabled. Sharpening and frame generation are disabled. The render extent is 854×534 and the output is 1280×800.

## Build from macOS

Run this command from the repository root with Docker Desktop running.

```sh
docker run --rm --platform linux/amd64 \
  --mount "type=bind,source=$PWD/benchmark,target=/job" \
  debian:trixie-slim sh -ec '
    apt-get update
    apt-get install -y --no-install-recommends ca-certificates curl cmake g++ libvulkan-dev ninja-build
    sh /job/build.sh
  '
```

On Linux x86-64 with the same tools installed, run `sh benchmark/build.sh`. Pass a local copy of the pinned source archive as the first argument for an offline build. The script checks SHA-256 and writes `benchmark/deck-fsr-benchmark`. The binary built with Debian Trixie requires glibc 2.38 or newer and the Vulkan loader. The C++ runtime is linked statically.

## Run on Steam Deck

Copy the binary to an executable directory on the Deck and run it in the user session.

```sh
./deck-fsr-benchmark
```

The benchmark warms up for eight frames, then records 40 GPU timestamp pairs around the FSR commands. Texture initialization and readback are outside the timed region. At reset and at the end of the temporal sequence, it checks all 1,024,000 RGBA16F output pixels for positive finite RGB with `R < G < B` and alpha equal to 1. It prints the device, queue, timestamp period, individual samples, median, and nearest-rank p95.

Exit 0 means the run and readbacks passed. Exit 77 means the program could not select a compatible Vulkan 1.3 device with compute timestamps.

Three runs on Steam Deck OLED with SteamOS 3.9.5 and RADV VANGOGH gave a combined median of 3.53126 ms and a nearest-rank p95 of 4.4314 ms over 120 samples. All six readbacks passed. This measures a synthetic FSR workload.

The Vulkan resource helpers adapt the FSR-Vulkan smoke test. The build uses pinned FSR-Vulkan sources derived from AMD FidelityFX SDK 1.1.4. [FSR-Vulkan revision](https://github.com/vladtepes22/FSR-Vulkan/tree/c64f093404125e960813f95f1996c14780f0dd69) · [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4) · [License notices](../licenses/)
