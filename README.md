# Deck FSR

Deck FSR is an open-source project for adding temporal upscaling to games on Steam Deck. The first game profile targets the original **Tom Clancy’s The Division**. The current release scope is an FSR 3.1.4 Vulkan benchmark and offline tools for examining the game's TAA dispatch.

| Component | Status |
| --- | --- |
| FSR 3.1.4 Super Resolution | Runs on Steam Deck OLED with RADV |
| The Division, Steam build `21161679` | Experimental offline profile. The two TAA shaders are identified, and the descriptor resolver has synthetic tests. |
| In-game upscaling and Decky integration | Pending |

The headless benchmark upscales **854×534 to 1280×800**. Across 120 measured dispatches on SteamOS 3.9.5, it took **3.53 ms median** and **4.43 ms p95**. The timing covers FSR on a synthetic scene with constant color and depth and zero motion and jitter. Generation is disabled. Game FPS and battery life remain unmeasured. [Benchmark setup and build instructions](benchmark/README.md)

## Try the tools

- Build and run the native Linux x86-64 Vulkan benchmark on the Deck. A container command for building on macOS is included. [Benchmark](benchmark/README.md)
- Build the Vulkan control app to test RenderDoc replay against known descriptor and mip selections. [Control app](control/README.md)
- Resolve the TAA descriptors in a saved Vulkan capture or run the profile tests with Python 3.10 or newer. [The Division profile](profiles/the-division/README.md)

```sh
python3 -B -m unittest discover -s profiles/the-division
python3 profiles/the-division/division_taa_motion.py --check-example
```

## Add a game profile

1. Add a directory in `profiles/` with the game's identifier, build, graphics API, and any tested Proton version. Describe the render pass and record how you recognized it.
2. Implement an offline resolver if the game needs one. Identify the shader and map descriptor metadata to color, motion, depth, and output roles. Document what remains unknown, including motion units, jitter, and ordering relative to the HUD.
3. Add synthetic tests for the mapping, an unknown build or shader, and missing or ambiguous descriptors. Document any verification on a real device. Share source and observations in the pull request. Keep captures containing game content local.

TuxScaling is a useful comparison on Proton. It estimates motion from presented frames and provides synthetic temporal inputs. The Division profile investigates sourcing the inputs from the game render pass. [TuxScaling](https://github.com/Spexxl/TuxScaling)

Deck FSR source is MIT-licensed. The benchmark uses FSR-Vulkan and AMD FidelityFX SDK sources under their respective MIT notices. The RenderDoc header in the control app carries its own license and attribution. [Project license](LICENSE) · [Third-party notices](licenses/)

### По-русски

FSR 3.1.4 уже проверен на Steam Deck в отдельном стенде. Профиль первой The Division анализирует сохранённый кадр и сопоставляет привязки шейдера с входами апскейлера. Подключение FSR к игре и замеры FPS и автономности впереди. [Профиль The Division](profiles/the-division/README.md)
