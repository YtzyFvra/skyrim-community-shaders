#include "../PerfMode.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "../../../State.h"
#include "../../Upscaling.h"

// Quality mode -> render-scale resolution is supplied by the FFX SDK helper
// (same one Upscaling.cpp uses), avoiding a duplicate scale table here.
#include <FidelityFX/host/ffx_fsr3.h>

// Quality mode (1..4) whose upscale ratio is nearest to the requested one.
// NativeAA (0) is excluded: an explicit sub-native scale is always an upscale,
// and DLAA rejects render extents below the output size.
static uint32_t NearestQualityModeForRatio(float a_ratio)
{
	uint32_t best = 1;
	float bestDelta = FLT_MAX;
	for (uint32_t mode = 1; mode <= 4; ++mode) {
		const float delta = std::abs(Upscaling::GetQualityModeRatio(mode) - a_ratio);
		if (delta < bestDelta) {
			bestDelta = delta;
			best = mode;
		}
	}
	return best;
}

void PerfMode::InstallRenderTargetSizeHook()
{
	if (!globals::game::isVR)
		return;

	if (hookActive)
		return;

	// Eager capture — get real HMD resolution BEFORE installing hook
	auto* openvr = RE::BSOpenVR::GetSingleton();
	if (!openvr || !openvr->vrSystem) {
		logger::error("[PerfMode] BSOpenVR or vrSystem not available — hook NOT installed");
		return;
	}

	uint32_t w = 0, h = 0;
	openvr->vrSystem->GetRecommendedRenderTargetSize(&w, &h);
	if (w == 0 || h == 0) {
		logger::error("[PerfMode] GetRecommendedRenderTargetSize returned {}x{} — hook NOT installed", w, h);
		return;
	}

	displayEyeWidth = w;
	displayEyeHeight = h;

	// BSShaderRenderTargets::Create runs after SKSE feature settings load, so
	// upscaling.settings.qualityMode here reflects the user-saved value. We
	// snapshot the corresponding scale at install time and never re-read it —
	// the engine's RT allocations happen once, so a later UI quality change
	// can't shrink/grow RTs anyway. (Requires a game restart, same as DLSS.)
	//
	// Validate before division: a bad/corrupt JSON could put qualityMode
	// outside FFX's range, returning 0/inf/NaN; that would propagate to bogus
	// renderEye dimensions and silently mis-size every engine RT. Fail closed
	// — leave hookActive=false so the rest of PerfMode is dormant and DLSS
	// runs on dev's standard path.
	auto& upscaling = globals::features::upscaling;
	const uint32_t qualityModeRaw = upscaling.settings.qualityMode;
	uint32_t qualityMode = std::clamp<uint32_t>(qualityModeRaw, 0, 4);  // FfxFsr3QualityMode range
	float scale = ffxFsr3GetUpscaleRatioFromQualityMode(static_cast<FfxFsr3QualityMode>(qualityMode));
	if (!std::isfinite(scale) || scale <= 0.0f) {
		logger::error("[PerfMode] FFX returned invalid upscale ratio {} for qualityMode {} (raw {}); hook NOT installed", scale, qualityMode, qualityModeRaw);
		return;
	}

	// An explicit scale overrides the preset ratio (the preset becomes inert) and
	// re-derives the mode so NGX extent-vs-mode validation matches the allocation.
	const float explicitScaleRaw = upscaling.settings.vrRenderScale;
	const bool explicitScale = explicitScaleRaw > 0.0f;
	if (explicitScale) {
		const float clamped = std::clamp(explicitScaleRaw, Upscaling::kVRRenderScaleMin, Upscaling::kVRRenderScaleMax);
		if (clamped != explicitScaleRaw)
			logger::warn("[PerfMode] vrRenderScale {} out of range, clamped to {}", explicitScaleRaw, clamped);
		scale = 1.0f / clamped;
		qualityMode = NearestQualityModeForRatio(scale);
	}

	// Even dimensions: odd widths truncate the engine's half-res buffers
	// (underwater mask, volumetrics) and misalign SBS stereo math by a pixel.
	renderEyeWidth = std::max<uint32_t>(2, (uint32_t)(w / scale)) & ~1u;
	renderEyeHeight = std::max<uint32_t>(2, (uint32_t)(h / scale)) & ~1u;

	// An explicit scale can land between preset ratios; clamp the allocation into
	// the latched mode's NGX range (never the dispatch extent: RTs must match).
	if (explicitScale && upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS)
		upscaling.streamline.ClampToDLSSRenderRange(qualityMode, displayEyeWidth, displayEyeHeight, renderEyeWidth, renderEyeHeight);

	latchedQualityMode = qualityMode;
	explicitScaleLatched = explicitScale;
	logger::info("[PerfMode] Latched display {}x{} -> render {}x{} per eye (quality mode {}, {})",
		displayEyeWidth, displayEyeHeight, renderEyeWidth, renderEyeHeight, qualityMode,
		explicitScale ? "explicit scale" : "preset-derived");

	// Restart-required settings snapshot is latched by the render-target
	// creation hook, but keep this robust to call-order changes.
	globals::features::upscaling.bootSnapshot.LatchIfNeeded(globals::features::upscaling.settings);

	stl::write_vfunc<0x12, GetRenderTargetSize_Hook>(RE::VTABLE_BSOpenVR[0]);

	// Per-frame renderer detours chained via stl::detour_thunk.
	if (!setDirtyStatesHookInstalled) {
		stl::detour_thunk<BSGraphics_SetDirtyStates_Hook>(REL::RelocationID(75580, 77386));
		setDirtyStatesHookInstalled = true;
	}
	if (!updateViewPortHookInstalled) {
		stl::detour_thunk<BSGraphics_Renderer_UpdateViewPort_Hook>(REL::RelocationID(75455, 77240));
		updateViewPortHookInstalled = true;
	}

	hookActive = true;
}

void PerfMode::InstallVrLogicalEyeHook()
{
	if (!globals::game::isVR || hookActive || logicalEyeActive)
		return;

	auto* openvr = RE::BSOpenVR::GetSingleton();
	if (!openvr || !openvr->vrSystem) {
		logger::error("[PerfMode] VR logical eye: BSOpenVR unavailable");
		return;
	}
	uint32_t hmdW = 0, hmdH = 0;
	openvr->vrSystem->GetRecommendedRenderTargetSize(&hmdW, &hmdH);
	if (!hmdW || !hmdH) {
		logger::error("[PerfMode] VR logical eye: HMD size {}x{}", hmdW, hmdH);
		return;
	}

	auto& foveated = globals::features::upscaling.foveatedRender;
	foveated.bootSnapshot.LatchIfNeeded(foveated.settings);
	foveated.unifiedBootSnapshot.LatchIfNeeded(foveated.settings.unifiedPipeline);
	using Backend = FoveatedRender::FlatAct1Backend;
	const auto backend = static_cast<Backend>(std::min(
		foveated.bootSnapshot.Boot(&FoveatedRender::Settings::flatPipelineAct1Backend), 2u));
	if (backend == Backend::kBypass) {
		logger::info("[PerfMode] VR logical eye skipped: ACT1 bypass leaves nothing to upscale into");
		return;
	}
	const auto unified = foveated.ActiveUnifiedPipelineSettings();
	const bool srss = unified.act1Srss && backend == Backend::kDLSS;
	const uint32_t quality = std::min(srss ? unified.act1SrssQuality :
		foveated.bootSnapshot.Boot(&FoveatedRender::Settings::flatPipelineAct1Quality), 4u);
	const float guidanceScale = srss ? std::max(unified.guidanceScale, 1.0f) : 1.0f;
	const float ratio = Upscaling::GetQualityModeRatio(quality);
	if (!std::isfinite(ratio) || ratio <= 0.0f) {
		logger::error("[PerfMode] VR logical eye: bad quality ratio {} mode {}", ratio, quality);
		return;
	}

	auto even = [](float value) {
		const auto n = static_cast<uint32_t>(std::lround(value)) & ~1u;
		return std::max<uint32_t>(2u, n);
	};
	uint32_t guidanceW = even(static_cast<float>(hmdW) * guidanceScale);
	uint32_t guidanceH = even(static_cast<float>(hmdH) * guidanceScale);
	uint32_t renderW = even(static_cast<float>(guidanceW) / ratio);
	uint32_t renderH = even(static_cast<float>(guidanceH) / ratio);
	auto& upscaling = globals::features::upscaling;
	if (backend == Backend::kDLSS && upscaling.streamline.slDLSSGetOptimalSettings)
		upscaling.streamline.ClampToDLSSRenderRange(quality, guidanceW, guidanceH, renderW, renderH);
	renderW = std::max<uint32_t>(2u, renderW & ~1u);
	renderH = std::max<uint32_t>(2u, renderH & ~1u);

	displayEyeWidth = hmdW;
	displayEyeHeight = hmdH;
	renderEyeWidth = renderW;
	renderEyeHeight = renderH;
	latchedQualityMode = quality;
	explicitScaleLatched = false;
	logicalEyeActive = true;
	stl::write_vfunc<0x12, GetRenderTargetSize_Hook>(RE::VTABLE_BSOpenVR[0]);
	logger::info("[PerfMode] VR logical eye TD={}x{} G={}x{} R={}x{} mode={} srss={} G/D={:.3f}",
		hmdW, hmdH, guidanceW, guidanceH, renderW, renderH, quality, srss ? 1 : 0, guidanceScale);
}

void PerfMode::GetRenderTargetSize_Hook::thunk(RE::BSOpenVR* a_this, uint32_t* a_width, uint32_t* a_height)
{
	// Call original to get real HMD resolution
	func(a_this, a_width, a_height);

	auto& perfMode = globals::features::upscaling.perfMode;

	// Baseline against this hook's own first result, not displayEyeWidth/Height
	// (a separate, differently-scaled call made before hook install) -- that
	// mismatch false-positives on frame one.
	if (!perfMode.hookBaselineCaptured) {
		perfMode.hookBaselineEyeWidth = *a_width;
		perfMode.hookBaselineEyeHeight = *a_height;
		perfMode.hookBaselineCaptured = true;
	} else {
		// Live, not latched: matches every other restart banner in this codebase
		// (e.g. bootSnapshot.HasPendingChange), which clears if the live value
		// returns to the boot-latched one instead of staying stuck.
		const bool mismatched = *a_width != perfMode.hookBaselineEyeWidth || *a_height != perfMode.hookBaselineEyeHeight;
		if (mismatched != perfMode.displaySizeChanged) {
			if (mismatched) {
				logger::warn("[PerfMode] HMD render target size changed from {}x{} to {}x{} mid-session; restart required to re-latch",
					perfMode.hookBaselineEyeWidth, perfMode.hookBaselineEyeHeight, *a_width, *a_height);
			} else {
				logger::info("[PerfMode] HMD render target size returned to {}x{}", *a_width, *a_height);
			}
			perfMode.displaySizeChanged = mismatched;
		}
	}

	*a_width = perfMode.renderEyeWidth;
	*a_height = perfMode.renderEyeHeight;
}
