# VR Frame Generation

The running path is FSR frame generation on the DX12 doorbell, after DLSS SR and neural rendering. The DLSS-G pump below is not what this line submits to the headset.

## Running path

```text
Publish at the UI-composite hook (color, depth, mvec, kMENUBG, camera, frame interval)
  -> ARISE returns after the copy is queued; Single-thread waits for Submit
  -> one D3D12 queue: SR, NR, stretch, blend, then FSR on the SBS
  -> interpolated pose is the midpoint of the two publishes that were not skipped
  -> a skipped publish resets FSR and clears that midpoint
  -> beat thread: pair id, interpolated Submit, then real Submit
  -> do not reuse those images until the Submit copy has finished
```

`motionVectorScale` is `(guideWidth/2, guideHeight)`: 1.0 in the buffer is one eye. Do not negate Y. OpenComposite must not play a later interpolated frame ahead of an older real frame.

## Earlier DLSS-G pump (not this submit path)

Two backends were designed to share pose-tagged Submit:

- **DLSS-G pump (`VrFgPump`, primary).** A private D3D12 flip chain is the doorbell; G's own thread presents interpolated and real frames; our hook on the native `Present` copies each presented backbuffer and forwards it to the engine `BSOpenVR::Submit` path with the matching pose. This configuration has shown a compositor-measured 2× once (12 Sep, first cut), with a frozen colour input — the plumbing was right and the colour path was wrong. Everything since that cut that touched ordering, holding, harvesting or the Submit route is reverted.
- **FFX Frame Interpolation (`FfxFrameInterpolator`, fallback).** Operator per eye at E×E on the game thread, no swapchain. Used if the pump cannot pass acceptance, and the only route that can avoid the SBS seam (one camera per eye).

Both submit the interpolated frame **with its own pose** (`Submit_TextureWithPose`); see below.

## Why the pose is the whole game

The compositor reprojects every submitted frame from the pose the app declares for it to the pose at scan-out. A frame interpolated between N-1 and N was rendered, in effect, at pose N-½. Submitting it under pose N (the last `WaitGetPoses`) makes the compositor reproject it by the wrong half-frame of head motion: the image doubles and snaps when turning. That is the "turn away / turn back judder" seen on every pump build. `VRTextureWithPose_t` with `mDeviceToAbsoluteTracking = interp(pose[N-1], pose[N], 0.5)` (quaternion slerp for rotation, lerp for translation) fixes it independently of *when* the frame is submitted. Timing then only affects smoothness, not correctness.

## DLSS-G pump contract

```text
Game thread, per frame (FG on, not in menus):
  eyes -> pack HUDLess SBS (GetDesc(kTEMP))                          existing
  PoseRing.Push(pose[N])
  CopyGuides: HUDLess / depth / mvec (/ UI) into shared 11/12 textures, Flush(), Signal(fence)
  queue.Wait(fence); copy HUDLess -> backbuffer[ proxy->GetCurrentBackBufferIndex() ] via proxy->GetBuffer
  Tag depth / mvec / HUDLess (/ UI); ConfigureDLSSG(on after first Present)
  proxy->Present(0, ALLOW_TEARING)                                    doorbell; window is shown, tiny

G thread, per native Present (order is [interp, real]):
  copy presented backbuffer -> submitRing[k] on the same queue; Signal; original Present; CPU-wait fence
  pose = first-in-pair ? PoseRing.Interpolated(0.5) : PoseRing.Latest()
  engine BSOpenVR::Submit(submitRing[k], pose)                         VDXR counts this path only
```

Colour freshness is the invariant that broke the first cut: the buffer G interpolates must be the one the proxy reports as current, written after the D3D11 side has flushed. Verify with a frame-counter stamp drawn into HUDLess before the copy; if the stamp does not advance on the HMD while motion shimmers, the colour path is stale — fix that, not the ordering.

## Pipeline (TD = E), FFX fallback

```text
Engine Tonemap -> kTEMP SBS
  -> per-eye six-pass -> eyeColor[eye] (E×E, private)          existing
  -> pose[N] = HMD pose from this frame's WaitGetPoses          VrPoseRing
  -> per eye:
       OpticalFlow.dispatch(eyeColor[eye])                      FfxOpticalflow context (E×E)
       FI.prepare(depth[eye], mvec[eye], camera[eye])            FfxFrameInterpolationPrepare
       FI.dispatch(eyeColor[eye] as current, OF, -> interp[eye]) FfxFrameInterpolationDispatch
  -> pack eyeColor -> realSbs[N], pack interp -> interpSbs[N-½]  existing pack CS, 2-slot rings
  -> submit interpSbs[N-½] with pose(N-½)   (Submit_TextureWithPose)
  -> submit realSbs[N]     with pose(N)     (Submit_TextureWithPose, or default)
  -> engine world Submit swallowed; overlay Submits pass         existing
```

Stage 1 submits both frames back-to-back on the game thread. Stage 2 moves the interpolated Submit to `VrSubmitScheduler`, a worker that sleeps until the midpoint of the app frame interval (`IVRCompositor::GetFrameTiming` for the interval, QPC for the target) and then submits. Only Stage 2 can show 2× on the compositor's frame counter; Stage 1 exists to prove image and pose correctness without threads.

## Inputs FI needs (per eye)

| Input | Source | Note |
|---|---|---|
| current color | `eyeColor[eye]` E×E, RGBA8 | FI keeps its own previous frame |
| depth | `kMAIN` eye half, cropped to E×E via `CopyDepthSubrect` | `FFX_FRAMEINTERPOLATION_ENABLE_DEPTH_INVERTED` only if engine depth is reversed (it is not: sky = 1) |
| motion vectors | `kMOTION_VECTOR` eye half | Same scale convention as `Act2FsrScaler` feeds FSR3 upscaler; reuse its `motionVectorScale` |
| camera | per-eye projection from `BSOpenVR` | `cameraNear/Far` from `Util::GetCameraData`, `cameraFovAngleVertical` per eye, `viewSpaceToMetersFactor = 0.01428` |
| frameTimeDelta | ms since last dispatch | From QPC, clamp 1..100 ms |
| reset | first frame after `planChanged` or history reset | Same frames that already skip Submit |

Optical flow runs on the color at E×E; its outputs (`opticalFlowVector`, `opticalFlowSCD`) go into the FI dispatch. Use `ffxFrameInterpolationGetSharedResourceDescriptions` for dilated depth / mvec / reconstructed previous depth and allocate them once per eye.

## Rules

- FG off, menus, pause, map: `ShouldUseFrameGenerationThisFrame()` false → exactly the current owned Submit, one frame, no FI dispatch. `LoadingMenu` sleeps six-pass and FG. A logical-eye Submit that is not true-display size is scaled to that size before it touches the shared slot, so the slot is never created at engine SBS (see `vr-owned-submit.md`).
- `planChanged` or a failed eye: no pack, no Submit, FI `reset = true` next frame.
- Never `WaitGetPoses` ourselves. Poses are read from the engine's own call.
- Never submit frames out of order: interp(N-½) then real(N), every frame, or real(N) alone when FI failed.
- The scheduler never touches the D3D11 immediate context. It only calls Submit on textures the game thread finished (fence or `Flush` + `Signal` on the game thread before hand-off).
- Do not flip `useArise`. Do not write back to kTEMP/kTOTAL.
- Acceptance is the compositor's frame counter ≈ 2× engine, no judder turning, desktop mirror unaffected. Internal counters are not acceptance.

## Code map

```text
src/Features/Upscaling/VrFrameGen/
  VrFrameGen.h/.cpp          IVrFrameGenerator + factory; owns per-eye contexts, rings, poses
  VrPoseRing.h               pose ring + interpolation (header-only)
  VrSubmitScheduler.h/.cpp   Stage 2 worker: timed Submit with pose
  FfxFrameInterpolator.h/.cpp FFX OF + FI per eye (D3D11 backend), Stage 1
src/Features/Upscaling/VrSubmitDlssg.cpp   SubmitOwnedSbs gains a pose overload
src/Features/Upscaling/VrFgPump.cpp        DLSS-G experiment, backend = 1 only
```
