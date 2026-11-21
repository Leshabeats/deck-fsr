"""Inspect one compute dispatch in a Vulkan .rdc using RenderDoc 1.46 Python.

Buffer prefixes are read at the end of the selected dispatch. The optional
index table must be read-only for the duration of that dispatch.
"""

import argparse
import hashlib
import json
import struct

import renderdoc as rd


def dispatches(actions):
    for action in actions:
        if action.flags & rd.ActionFlags.Dispatch:
            yield action
        yield from dispatches(action.children)


def inspect(controller, action, index_count):
    controller.SetFrameEvent(action.eventId, True)
    stage = rd.ShaderStage.Compute
    pipe = controller.GetPipelineState()
    shader = pipe.GetShaderReflection(stage)
    if shader is None:
        raise ValueError("The selected event has no compute shader reflection")
    textures = {tex.resourceId: tex for tex in controller.GetTextures()}
    result = {
        "event": action.eventId,
        "groups": list(action.dispatchDimension),
        "shader_sha256": hashlib.sha256(bytes(shader.rawBytes)).hexdigest(),
        "entries": [],
    }
    binding_lists = (
        ("read_only", pipe.GetReadOnlyResources(stage, True), shader.readOnlyResources),
        ("read_write", pipe.GetReadWriteResources(stage, True), shader.readWriteResources),
        ("constant", pipe.GetConstantBlocks(stage, True), shader.constantBlocks),
    )
    for kind, used, reflected in binding_lists:
        for entry in used:
            desc = entry.descriptor
            access = entry.access
            if access.index >= len(reflected):
                continue
            binding = reflected[access.index]
            if kind == "constant" and not binding.bufferBacked:
                continue  # Push constants have their own byte array below.
            row = {
                "class": kind, "set": binding.fixedBindSetOrSpace,
                "binding": binding.fixedBindNumber, "element": access.arrayElement,
                "resource": str(desc.resource), "view": str(desc.view),
            }
            tex = textures.get(desc.resource)
            if tex is not None:
                size = (tex.width, tex.height, tex.depth)
                row.update(resource_format=tex.format.Name(), view_format=desc.format.Name(),
                           resource_extent=list(size), mip=desc.firstMip, mip_count=desc.numMips,
                           layer=desc.firstSlice, layer_count=desc.numSlices,
                           mip_extent=[max(1, v >> desc.firstMip) for v in size])
            elif desc.resource != rd.ResourceId():
                row.update(offset=desc.byteOffset, length=desc.byteSize)
                if kind == "constant" and desc.byteSize:
                    raw = controller.GetBufferData(desc.resource, desc.byteOffset, min(desc.byteSize, 80))
                    row["prefix_hex"] = bytes(raw).hex()
            result["entries"].append(row)
    push = bytes(controller.GetVulkanPipelineState().pushconsts)
    result["push_hex"] = push.hex()
    if index_count:
        if len(push) < 8:
            raise ValueError("Expected a buffer device address in the first eight push-constant bytes")
        address, = struct.unpack_from("<Q", push)
        size = 4 * index_count
        candidates = [buf for buf in controller.GetBuffers() if buf.gpuAddress
                      and 0 <= address - buf.gpuAddress <= buf.length - size]
        if len(candidates) != 1:
            raise ValueError(f"Expected one matching buffer device address range, found {len(candidates)}")
        buf, = candidates
        offset = address - buf.gpuAddress
        raw = controller.GetBufferData(buf.resourceId, offset, size)
        result["indirect"] = {"resource": str(buf.resourceId), "offset": offset,
                              "indices": list(struct.unpack("<" + "I" * index_count, raw))}
    return result


def main(argv=None, owns_replay=True):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rdc")
    parser.add_argument("--event", type=int, help="required if the capture has multiple dispatches")
    parser.add_argument("--index-count", type=int, choices=range(1, 5),
                        help="read 1–4 uint32 indices through the BDA at push-constant offset 0")
    args = parser.parse_args(argv)
    if owns_replay:
        rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    capture = rd.OpenCaptureFile()
    try:
        status = capture.OpenFile(args.rdc, "", None)
        if status != rd.ResultCode.Succeeded:
            raise ValueError(str(status))
        status, controller = capture.OpenCapture(rd.ReplayOptions(), None)
        if status != rd.ResultCode.Succeeded:
            raise ValueError(str(status))
        try:
            choices = [a for a in dispatches(controller.GetRootActions())
                       if args.event is None or a.eventId == args.event]
            if len(choices) != 1:
                raise ValueError(f"Expected one compute dispatch, found {len(choices)}. Specify --event.")
            print(json.dumps(inspect(controller, choices[0], args.index_count), indent=2))
        finally:
            controller.Shutdown()
    finally:
        capture.Shutdown()
        if owns_replay:
            rd.ShutdownReplay()


if __name__ == "__main__":
    main()
