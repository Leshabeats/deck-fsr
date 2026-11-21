# Tom Clancy’s The Division

This experimental offline profile targets Steam App ID `365590`, build `21161679`, with DirectX 12 through Proton Experimental, vkd3d-proton and RADV. The profile has no in-game runtime yet.

## Observed TAA dispatch

| TAA option | Shader label hash | SHA-256 of Vulkan shader bytes |
| --- | --- | --- |
| Supersampling | `454827a21acbab8d` | `f9f53bbbb48a38b1a8c71ee99fd22820afb68d0aaef722e62996d9bb9cbd39b4` |
| Stabilization | `eb009069ee26b2be` | `77d430846e46b5f81cb2ef28f4b0cb102680646bd441fbbf04c35da47de23811` |

Both use 8×8×1 compute workgroups. An RGP trace showed a 160×100×1 supersampling dispatch, covering a 1280×800 dispatch grid. The game texture views, mip dimensions, and formats still need confirmation from a replayable frame.

Textures use descriptor set 1, binding 1. Push constants hold the address of four unsigned indices `I` at byte 0, the image base at byte 8, the output base at byte 20, and the global base at byte 32.

| Role | Array element |
| --- | --- |
| Scene color, history color candidate, scalar history input, motion | `imageBase + I[0..3]` respectively |
| Scene depth candidate | `globalBase + 26` |
| Pixel flags | `globalBase + 15` |
| Main output, auxiliary output | `outputBase`, `outputBase + 1` |

The coordinate UBO uses set 2, binding 0. The array element comes from push-constant byte 12. Both shaders add decoded motion to the reprojection coordinate, then multiply it by `C[0].xy` and add `C[0].zw` to reach history UV. The profile decodes the first 80 UBO bytes. The resulting numeric motion scale depends on captured values and the active render region. Texture formats, depth convention and projection jitter are open checks.

In frame 3832, the supersampling TAA dispatch precedes an indexed UI draw in command order. The draw's active attachment and the composition order of individual HUD elements remain unknown. [Capture evidence](integration.md)

## Resolve a saved frame

From the repository root, use a Vulkan RDC containing the dispatch and RenderDoc 1.46 Python replay support. Set `taa_event` to the dispatch's event ID.

```sh
python3 tools/inspect_dispatch.py frame.rdc --event "$taa_event" --index-count 4 > dispatch.json
python3 profiles/the-division/resolve_dispatch.py dispatch.json > profile.json
python3 profiles/the-division/division_taa_motion.py profile.json
```

The resolver compares the exact shader hash, resolves each used descriptor by its set, binding, element and class, and reports the selected view's format and mip extent. Missing or ambiguous bindings produce `unresolved` and exit 1. The nine resolver tests cover both shader signatures and malformed or incomplete input. RenderDoc replay has been verified on the standalone Vulkan control app. End-to-end verification on a The Division RDC remains open. [Control app](../../control/README.md)
