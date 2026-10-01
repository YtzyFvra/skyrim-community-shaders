# VR Owned Submit (TD = E)

Public contract for the SkyrimVR doorbell. FSR-FG working notes (git-excluded): `开发说明/VR-FSRFG知识谱/`. DLSS-G pump notes stay under `开发说明/OSDLSSNR/VR-FG/` and are not this path.

## Contract

```text
Engine world @ logical R SBS (kTEMP)
  -> UI-composite hook, before kMENUBG blends onto kTOTAL
  -> On the engine: crop each eye, stretch the backdrop, downsample depth and motion for NR
  -> copy those prepared resources, plus full-frame depth, motion, and kMENUBG, once into the shared slots
  -> ARISE: engine returns once that copy is submitted
  -> Single-thread: engine waits until this frame's OpenVR Submit returns
  -> DX12 doorbell on one queue: one copy, then DLSS SR, NR, center blend, then FSR
  -> stamp kMENUBG with 1.0 - alpha onto the real and generated SBS
  -> beat thread Submits interpolated, then real, each with its own pose
  -> OpenComposite plays an interpolated frame only if its real frame is queued behind it
```

A skipped publish resets FSR. Do not slerp pose from the frame before the gap to the frame after it. Camera, jitter, clipping planes, FOV, and the engine frame interval are captured with the publish. Do not overwrite the mail image until that Submit has copied it. Do not write back to kTEMP. Do not flip `useArise` on VR. Do not resize the shared slot (that resets the D3D11 fence).

`LoadingMenu` sleeps reconstruction and frame generation. A source that is not the slot size is scaled into the existing slot. Do not create or resize the slot: recreating it resets the D3D11 fence to 0. The first real publish after the menu closes resets frame generation once. The HMD follows the loading image.

## Inject

`Main_PostProcessing` only runs Flat after Post. VR runs in `UICompositeRenderHook`, before the engine blends `kMENUBG` onto `kTOTAL`. World color, depth, and motion are still read from `kTEMP` (the copy into `kTOTAL` has already happened). `kMENUBG` is copied in the same hook. Tonemap itself is not patched on VR. Failed isolated passes do not Submit (engine world Submit stays live). The first history-reset frame does not Submit.

Early `Upscale()` encode + KIT/standard DLSS are skipped when `IsVrDlssPostInjectActive()` or RTX DLSS Booster override is on (`ShouldSkipUpscalingDlss`), so ACT1 owns reconstruction.

`ConfigureUpscaling` also locks engine DRS to 1×1 and sizes jitter from the per-eye 8-pass plan.

## Sizes

| Symbol | Source |
|---|---|
| E | `kIMAGESPACE_TEMP_COPY` half-eye. Single-thread PerfMode makes this logical R |
| R | E × UV box. ACT1 and NR see only this center |
| D | TD × UV box. The center's place in the HMD image |
| TD | HMD eye. Submit and FG are the full 2·TD×TD SBS |

Current DB sequence and which stage eats 4K vs TD: `开发说明/VR-FSRFG知识谱/DB架构-时序与逻辑-2026-09-25.md`.

Single-thread PerfMode (restart) spoofs `GetRenderTargetSize` to logical R from the ACT1 preset, the HMD eye, and SRSS. The engine renders that size at scale 1. kTEMP, depth, and motion stay small. The fork upsamples the low-res eye to TD, then composites the ACT1 center. It does not enlarge engine RTs.

Owned pixels live in a private `2E×E` SBS. We call `BSOpenVR::Submit` once; the game splits both eyes. FG 2× means VDXR compositor fps, not hook Present count.

Engine motion is per-eye NDC UV (`GetSSMotionVector`). The R crop and `ResizeGuides` keep those values. Feature 18 does `pixels = mvec * scale` on the texture it sees, so NR1 (all N) uses `MVecScale = E * N / R`. ACT3 stays on the D-pixel grid after a window crop, so it uses encoding `E`.

Host Feature 18 keeps four slots: 0/1 = per-eye NR1, 2/3 = per-eye ACT3. With the display-resolution motion-vector flag off, `motionVectorScale` is one render-size eye: half the SBS guide width and the full height. Do not negate Y.

The DLSS-G private flip-chain pump is not the headset submit path. Frame generation details: [VR Frame Generation](vr-frame-generation.md).

## Screenshot / foveated preview

After a successful owned Submit, preview and screenshot read the packed SBS. Fallback is still `kTOTAL` if that frame did not own Submit. In-world HUD is `kMENUBG`, mixed with `1.0 - alpha`.
