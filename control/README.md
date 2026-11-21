# Vulkan capture control app

One compute dispatch reads an index from a buffer device address passed in push constants. The index selects one of two `R8G8B8A8_UNORM` views of a 4×4 image. Slot 0 selects the 4×4 red mip and slot 1 selects the 2×2 blue mip. The app reads back and checks the packed RGBA value.

Build on Linux with C and Vulkan development headers, `glslc`, and SPIR-V Tools. Run these commands from the repository root.

```sh
cc -O2 -Wall -Wextra -Werror -o control/vk-control control/control.c -ldl
glslc --target-env=vulkan1.2 -O -DNONUNIFORM -o control/control-nonuniform.spv control/control.comp
spirv-val --target-env vulkan1.2 control/control-nonuniform.spv
control/vk-control control/control-nonuniform.spv - 0 --nonuniform
control/vk-control control/control-nonuniform.spv - 1 --nonuniform
```

The runtime requires a Vulkan 1.2 device with a buffer device address and non-uniform indexing for storage images. Both selections passed on RADV VANGOGH.

For a capture on SteamOS with RenderDoc 1.46, load its Vulkan capture layer for this process and specify an output prefix. Adjust the shared library path if RenderDoc lives elsewhere.

```sh
ENABLE_VULKAN_RENDERDOC_CAPTURE=1 LD_PRELOAD=/usr/lib/librenderdoc.so \
  control/vk-control control/control-nonuniform.spv "$PWD/control/control" 1 --nonuniform
python3 tools/inspect_dispatch.py control/control_capture.rdc --index-count 1
```

The last command needs the RenderDoc 1.46 Python replay module on the machine replaying the capture. On a Deck with `renderdoc-minimal`, use a RenderDoc build with Python replay support. The experiment extracted both index values, selected mips and dimensions from independent captures.

The application API header is MIT-licensed and includes its original copyright and license notice. [RenderDoc source](https://github.com/baldurk/renderdoc/blob/v1.46/renderdoc/api/app/renderdoc_app.h)
