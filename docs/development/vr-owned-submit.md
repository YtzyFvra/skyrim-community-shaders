# VR Owned Submit

Public contract for SkyrimVR Neural Rendering. Working notes (git-excluded): `开发说明/OSDLSSNR/`.

## Contract

```text
Engine world @ native or whatever the engine allocated
  -> engine Post / Tonemap
  -> kIMAGESPACE_TEMP_COPY  (KIT "kTEMP": RGBA8 SBS)
  -> per-eye 8-pass (same SRSS / AA plan as Flat)
  -> private eye textures
  -> IVRCompositor::Submit(Left) then Submit(Right)
  -> swallow engine world Submit of kTOTAL / kVR_FRAMEBUFFER
Overlay / floating-menu Submit is not those two textures; it is not swallowed.
```

Do not write the 8-pass result back to `kTOTAL`. Do not flip `useArise` on VR. Do not require VR PerfMode (no Hybrid Post, no `testTexture`, no `GetRenderTargetSize` lie).

ARISE / DX12 Tail / physical DXGI Present stay flat-only.

## Inject

`Main_PostProcessing` runs the engine Post chain, then `NeuralRendering::ApplyFoveatedLdr()` → `ApplyVrSixPass`. `UICompositeRenderHook` may call it again; `vrSixPassDoneFrame` makes the second call a no-op.

## Sizes

| Symbol | Source |
|---|---|
| R (engine eye) | `kIMAGESPACE_TEMP_COPY` width/2 × height |
| D (display eye) | OpenVR recommended size, else `kTOTAL` eye |

If PerfMode is still on, the engine may be rendering cheaper. That is optional and independent. The 8-pass path does not read `perfMode.Get*Eye*()`.

## Frame generation

This cut Submits the 8-pass color directly. A single hidden swapchain cannot Present left then right without mixing DLSS-G history. Per-eye chains are a follow-up.

## Screenshot / foveated preview

Prefer the private owned SBS after the first successful submit. Fall back to `kTOTAL` / `kTEMP` only before that.
