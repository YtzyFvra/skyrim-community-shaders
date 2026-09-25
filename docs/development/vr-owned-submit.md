# VR Owned Submit (TD = E)

Public contract for SkyrimVR Neural Rendering. Working notes (git-excluded): `开发说明/OSDLSSNR/`.  
FG: `docs/development/vr-frame-generation.md` and `开发说明/OSDLSSNR/VR帧生成-任务板.md`.

## Contract

```text
Engine world @ native E SBS
  -> engine Post / Tonemap
    -> kIMAGESPACE_TEMP_COPY  (KIT "kTEMP": RGBA8 SBS)
    -> crop SBS E color/depth/mvec to private PE R (the box)
    -> per-eye 8-pass (same SRSS / AA plan as Flat)
    -> compose the box onto a private E×E eye (center crop feathers; full-eye is a hard copy)
    -> pack both eyes into a private 2E×E SBS
    -> BSOpenVR::Submit once on that SBS (vfunc 0x03 already does both eyes)
    -> same path as the engine; VDXR sits here. Do not call GetIVRCompositor from the DLL
    -> engine world Submit is swallowed. Overlay RTs are not passed through.
    -> at UI composite, before the engine blends kMENUBG onto kTOTAL: run the graph from kTEMP, and copy kMENUBG aside
    -> stamp that HUD onto the real and generated SBS after FG
```

Do not write the box back to kTEMP. Do not invent per-eye `{0,1}–{1,0}` bounds (that flipped and zoomed the first cut). Do not flip `useArise` on VR. Do not engage the enlarge-RT PerfMode (kTEMP/kTOTAL grown to display size) on this path. Single-thread logical eye is the VR size lie; without it, TD equals E. No fake Present. No DLSS-G.

`LoadingMenu` sleeps the six-pass and frame generation. Any logical-eye publish whose source is not true-display size is scaled to that size before it touches the shared slot, including the first Submit and a frame the six-pass did not publish. Do not create or resize the slot at engine SBS: recreating it resets the D3D11 fence to 0, and a tail that only prepares when `fenceValue > lastConsumed` then never kicks again. The first real publish after the menu closes resumes and resets frame generation once. Confirmed on device: the HMD follows the loading image instead of holding a pinned frame.

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

Host Feature 18 keeps four slots: 0/1 = per-eye NR1, 2/3 = per-eye ACT3.

SRSS cannot shrink engine R on this path. After `BuildFlatPipelinePlan`, ACT1 quality (and evaluate extent) is reconciled so frozen R stays inside the NGX render range for G.

## Prepare

Crop only: `CopySubresourceRegion` for color and motion, `CopyDepthSubrect` for depth. No `EncodeTexturesCS`, no 5×5 dilate, no KIT Faster / Extreme.

## Frame generation

When Frame Generation is on, pack both eyes into the real SBS texture (`GetDesc` of kTEMP, not a computed `2E×E`). One Present timeline; do not Present per eye. DLSS-G has no Evaluate: the only doorbell is a private D3D12 flip `Present`. That Present is not the HMD path. A shown 8×8 `HWND_BOTTOM` stamp lets DWM retire the flip queue (`ALLOW_TEARING` + `FLIP_DISCARD`); its pixels are discarded.

```text
Game  ApplyVrSixPass → SBS
  │   if pump busy: do not swallow — engine world Submit
  │   else CopyGuides (D3D11) → kick → Arm swallow → return
  │   engine D3D11 Present = desktop
Worker
  │   Tag + slSwapChain->Present (doorbell)
  │     G native Present interp → hook copy+Submit that BB
  │     G native Present real   → hook copy+Submit that BB
```

Upgrade the DXGI factory, then create the chain (a bare factory makes `NvPresent64` AV). The native Present hook is installed on a cloned swapchain vtable. The hook copies the current backbuffer on the same queue, waits the copy fence on the CPU, then `BSOpenVR::Submit`s that ring slot in G Present order. Do not reorder or hold. No harvest / `WaitGetPoses`. Do not blend `kMENUBG` into the engine RT. Depth and motion stay at the engine SBS. FG color is the TD SBS. With the display-resolution motion-vector flag off, `motionVectorScale` is one render-size eye: half the SBS width and the full height. FSR divides that by `renderSize` and adds the result as UV. The precompiled frame-generation DLL is the authority for that multiply; the checked-in `ffx_frameinterpolation.cpp` is not on this path. Do not flip `useArise`. Loading/menus keep Upscaling's FG-off rule (`ShouldUseFrameGenerationThisFrame`). If the pump never owns the frame, Submit the packed SBS at Tonemap. If the pump is live but the previous doorbell is still presenting, leave the engine Submit live — do not Tonemap-Submit. Never Submit from inside the engine Submit hook.

Working notes: `开发说明/OSDLSSNR/VR帧生成-任务板.md`. FG 2× is unverified on VDXR.

## Screenshot / foveated preview

After a successful owned Submit, preview and screenshot read the packed private SBS. Fallback is still `kTOTAL` if that frame did not own Submit.

In-world HUD is `kHUDMENU` (2048² on this HMD, not SBS-sized). The pump tags it for G when dimensions match. Overlay RTs still reach the compositor. Do not compositor-Submit from inside the engine Submit hook.
