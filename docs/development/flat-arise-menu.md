# Flat ARISE Menu Extents

In-repo invariants for logical-R / physical-D menus. The longer working notes live in the local knowledge base (`开发说明/OSDLSSNR/`, excluded from git). Update both when the contract changes.

## Extents

- Engine color/depth/UI buffers that Model 3 owns stay at logical **R**.
- Window, mouse, and the real DXGI swapchain stay at physical **D**.
- Do not “fix” menus by changing display mode, lying in DXGI `GetDesc`, or installing a global `RSSetViewports` hook.

## What is patched

- **Title / loading:** `BlitTitleBackgroundToSwapChain` at Present, only when `!worldRenderedThisFrame && IsMainOrLoadingMenuOpen()`. `kMENUBG` (RGBA8) is shader-blitted onto `SwapChainBuffer` (often R10). `CopyResource` cannot convert.
- **Map:** logical-target scope on Map / WorldMap imagespace. DistantBlur does the same for world frames.
- **ESC pause and TAB:** intentionally unpatched. Blurry or cropped backgrounds are accepted.

## Do not restore

- Per-frame `kMENUBG` → `SwapChainBuffer` on pause (hitch, frozen shot, TAB/Map reuse last ESC, first frame black).
- Skipping engine `ISCopy` while a pause menu is open (breaks the pause compositor).
- Forged depth/MV so menus can run world-path NR.
- VR PerfMode’s stretch-`ISCopy` as a flat fix (opposite size error; HUD is in enlarged `kMENUBG` on VR only).

Frame generation does not require `R10G10B10A2`. That format is chosen because the HDR Display feature is loaded, not because `enableHDR` or DLSS-G is on.

`Menu ISCopy Diagnostics` stays. Default off.
