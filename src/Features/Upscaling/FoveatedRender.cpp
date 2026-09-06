#include "FoveatedRender.h"

#include "../../Globals.h"
#include "../../I18n/I18n.h"
#include "../../Utils/Subrect.h"
#include "../../Utils/UI.h"
#include "../FoveatedCommon.h"
#include "../Upscaling.h"
#include "FoveatedRender/Core.h"
#include "NeuralRendering/Integration.h"
#include "NeuralRendering/Renderer.h"
#include "Profiler.h"

#include <algorithm>

#define I18N_KEY_PREFIX "feature.upscaling."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoveatedRender::Settings,
	enabled,
	dlssMode,
	stretchMode,
	peripheryBlurRadius,
	debugVisualize,
	peripheryAAMode,
	peripheryTemporalAlpha,
	subrectBlendMode,
	subrectMaskMode,
	subrectFeatherWidth,
	subrectFalloffCurve,
	subrectDitherStrength,
	neuralRenderingEnabled,
	flatPipelineAct1Backend,
	flatPipelineAct1Quality,
	flatPipelineAct1Preset,
	flatPipelineAct1Jitter,
	flatPipelineAct1Dsr,
	flatPipelineNrScale,
	flatPipelineAct2Backend,
	flatPipelineAct2Preset,
	flatPipelineAct2Jitter,
	flatPipelineFusionDirection,
	flatPipelineFusionDenoise,
	flatPipelineFusionDenoiseStrength,
	flatPipelineFusionSoftness,
	flatPipelineFusionEnhancedGuidance,
	flatPipelineFusionFrequencyGuidance,
	flatPipelineFusionDepthGuidance,
	flatPipelineFusionColorGuidance,
	flatPipelineSharpen,
	flatPipelineSharpness,
	flatAct1Enabled,
	flatAct1QualityMode,
	flatAct1Preset,
	flatAct1Sharpness,
	flatAct2QualityMode,
	flatAct2Preset,
	flatAct2Sharpness,
	neuralRenderingPreset,
	neuralRenderingIntensity,
	neuralRenderingLocalTone,
	neuralRenderingLocalStructure,
	neuralRenderingSkinStructure,
	neuralRenderingStyle,
	neuralRenderingAutoMask,
	neuralRenderingUICorrection,
	neuralRenderingBypassOutput,
	neuralRenderingResetEveryFrame,
	neuralRenderingInputPrefilter,
	neuralRenderingMotionVectorMode,
	neuralRenderingHalfResolution,
	neuralRenderingWorkingScale,
	neuralRenderingOutputMode,
	neuralRenderingNisSharpness,
	neuralRenderingFusionPreset,
	neuralRenderingFusionLuma,
	neuralRenderingFusionChroma,
	neuralRenderingFusionEdgeProtection,
	neuralRenderingFusionResidualLimit);

// ============================================================================
// Lifecycle
// ============================================================================

void FoveatedRender::PostPostLoad()
{
	bootSnapshot.LatchIfNeeded(settings);

	// Opt into the stereo extension so the controller tracks a separate
	// right-eye UV (HMD nose-side overlap symmetry).
	subrectController.SetStereoEnabled(true);

	// Seed sensible foveal presets. Empty-case only — user edits persist.
	// "Center N%" presets are symmetric per eye (no rightUV → auto-mirror, which
	// for centered UVs produces an identical right-eye UV). "Nasal Convergence"
	// is asymmetric: left eye biased toward its right edge, right eye biased
	// toward its left edge — both targeting the nose-side region where HMD
	// binocular fusion is strongest, so DLSS reconstruction lands in the actual
	// stereo overlap zone rather than diverging left/right fields.
	subrectController.SeedDefaultPresets({
		{ .name = kPresetFullEye, .uv = { 0.0f, 0.0f, 1.0f, 1.0f } },
		{ .name = kPresetCenter75, .uv = { 0.125f, 0.125f, 0.75f, 0.75f } },
		{ .name = kPresetCenter50, .uv = { 0.25f, 0.25f, 0.5f, 0.5f } },
		{ .name = kPresetNasalConvergence50,
			.uv = { 0.5f, 0.25f, 0.5f, 0.5f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.25f, 0.5f, 0.5f } },
		{ .name = kPresetNasalConvergence60,
			.uv = { 0.4f, 0.2f, 0.6f, 0.6f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.2f, 0.6f, 0.6f } },
		{ .name = kPresetNasalConvergence70,
			.uv = { 0.3f, 0.15f, 0.7f, 0.7f },
			.rightUV = Util::Subrect::UVRegion{ 0.0f, 0.15f, 0.7f, 0.7f } },
	});
	// PostPostLoad runs after settings load, so a user with an older, shorter
	// persisted preset list (from before these names existed) still sees every
	// current preset in the DrawEditor dropdown, not just whichever ones they
	// happened to click as buttons.
	subrectController.MaterializeNewDefaults();
	stl::write_vfunc<0x1, UICompositeRenderHook>(RE::VTABLE_BSImagespaceShaderCopyDynamicFetchDisabled[3]);
}

void FoveatedRender::UICompositeRenderHook::thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param)
{
	NeuralRendering::ApplyFoveatedLdr();
	func(imageSpaceShader, shape, param);
}

void FoveatedRender::ClearShaderCache()
{
	FoveatedRenderImpl::Core::ClearShaderCache();
	FoveatedRenderImpl::Core::ClearResources();
}

// ============================================================================
// Settings I/O — driven from Upscaling::Save/LoadSettings under a nested key
// ============================================================================

void FoveatedRender::SaveSettings(json& o_json)
{
	o_json = settings;
	subrectController.SaveSettings(o_json);
}

void FoveatedRender::LoadSettings(const json& o_json)
{
	settings = o_json;
	// Util::Subrect::Controller::LoadSettings takes `const json&` (Subrect.h:68)
	// so no const_cast is needed — keeping it would imply mutation that never
	// happens.
	subrectController.LoadSettings(o_json);
	ClampSettings();
}

void FoveatedRender::RestoreDefaultSettings()
{
	settings = {};
	ClampSettings();
}

void FoveatedRender::ClampSettings()
{
	settings.enabled = std::min(settings.enabled, 1u);
	settings.dlssMode = std::min(settings.dlssMode, 1u);
	settings.stretchMode = std::min(settings.stretchMode, 2u);
	settings.debugVisualize = std::min(settings.debugVisualize, 1u);
	settings.peripheryAAMode = std::min(settings.peripheryAAMode, 1u);
	settings.subrectBlendMode = std::min(settings.subrectBlendMode, 2u);
	settings.subrectMaskMode = std::min(settings.subrectMaskMode, 1u);
	settings.peripheryBlurRadius = std::clamp(settings.peripheryBlurRadius, 0.5f, 4.0f);
	settings.peripheryTemporalAlpha = std::clamp(settings.peripheryTemporalAlpha, 0.05f, 0.5f);
	settings.subrectFeatherWidth = std::clamp(settings.subrectFeatherWidth, 2.0f, 128.0f);
	settings.subrectFalloffCurve = std::clamp(settings.subrectFalloffCurve, 0.5f, 2.0f);
	settings.subrectDitherStrength = std::clamp(settings.subrectDitherStrength, 0.0f, 2.0f);
	settings.neuralRenderingPreset = std::min(settings.neuralRenderingPreset, 4u);
	settings.flatPipelineAct1Backend = std::min(settings.flatPipelineAct1Backend, 2u);
	settings.flatPipelineAct1Quality = std::min(settings.flatPipelineAct1Quality, 4u);
	settings.flatPipelineAct1Preset = std::min(settings.flatPipelineAct1Preset, 4u);
	settings.flatPipelineAct1Jitter = std::min(settings.flatPipelineAct1Jitter, 1u);
	settings.flatPipelineNrScale = std::clamp(settings.flatPipelineNrScale, 0.33f, 0.95f);
	settings.flatPipelineAct2Backend = std::min(settings.flatPipelineAct2Backend, 4u);
	settings.flatPipelineAct2Preset = std::min(settings.flatPipelineAct2Preset, 4u);
	settings.flatPipelineAct2Jitter = std::min(settings.flatPipelineAct2Jitter, 1u);
	settings.flatPipelineFusionDirection = std::min(settings.flatPipelineFusionDirection, 1u);
	settings.flatPipelineFusionDenoise = std::min(settings.flatPipelineFusionDenoise, 4u);
	settings.flatPipelineFusionDenoiseStrength = std::clamp(settings.flatPipelineFusionDenoiseStrength, 0.0f, 1.0f);
	settings.flatPipelineFusionSoftness = std::clamp(settings.flatPipelineFusionSoftness, 0.0f, 1.0f);
	settings.flatPipelineFusionFrequencyGuidance = std::clamp(settings.flatPipelineFusionFrequencyGuidance, 0.0f, 1.0f);
	settings.flatPipelineFusionDepthGuidance = std::clamp(settings.flatPipelineFusionDepthGuidance, 0.0f, 1.0f);
	settings.flatPipelineFusionColorGuidance = std::clamp(settings.flatPipelineFusionColorGuidance, 0.0f, 1.0f);
	settings.flatPipelineSharpness = std::clamp(settings.flatPipelineSharpness, 0.0f, 1.0f);
	settings.flatAct1QualityMode = std::min(settings.flatAct1QualityMode, 4u);
	settings.flatAct1Preset = std::min(settings.flatAct1Preset, 4u);
	settings.flatAct1Sharpness = std::clamp(settings.flatAct1Sharpness, 0.0f, 1.0f);
	settings.flatAct2QualityMode = std::min(settings.flatAct2QualityMode, 4u);
	settings.flatAct2Preset = std::min(settings.flatAct2Preset, 4u);
	settings.flatAct2Sharpness = std::clamp(settings.flatAct2Sharpness, 0.0f, 1.0f);
	settings.neuralRenderingIntensity = std::clamp(settings.neuralRenderingIntensity, 0.0f, 2.0f);
	settings.neuralRenderingLocalTone = std::clamp(settings.neuralRenderingLocalTone, 0.0f, 2.0f);
	settings.neuralRenderingLocalStructure = std::clamp(settings.neuralRenderingLocalStructure, 0.0f, 2.0f);
	settings.neuralRenderingSkinStructure = std::clamp(settings.neuralRenderingSkinStructure, 0.0f, 2.0f);
	settings.neuralRenderingStyle = std::min(settings.neuralRenderingStyle, 3u);
	settings.neuralRenderingInputPrefilter = std::min(settings.neuralRenderingInputPrefilter, 3u);
	settings.neuralRenderingMotionVectorMode = std::min(settings.neuralRenderingMotionVectorMode, 5u);
	settings.neuralRenderingWorkingScale = std::min(settings.neuralRenderingWorkingScale, 2u);
	settings.neuralRenderingOutputMode = std::min(settings.neuralRenderingOutputMode, 3u);
	settings.neuralRenderingNisSharpness = std::clamp(settings.neuralRenderingNisSharpness, 0.0f, 1.0f);
	settings.neuralRenderingFusionPreset = std::min(settings.neuralRenderingFusionPreset, 5u);
	settings.neuralRenderingFusionLuma = std::clamp(settings.neuralRenderingFusionLuma, 0.0f, 1.5f);
	settings.neuralRenderingFusionChroma = std::clamp(settings.neuralRenderingFusionChroma, 0.0f, 1.5f);
	settings.neuralRenderingFusionEdgeProtection = std::clamp(settings.neuralRenderingFusionEdgeProtection, 0.0f, 1.0f);
	settings.neuralRenderingFusionResidualLimit = std::clamp(settings.neuralRenderingFusionResidualLimit, 0.01f, 0.5f);
	// Preset clamping reads from Upscaling::Settings now.
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	sharedPreset = std::min(sharedPreset, 5u);
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

FoveatedRender::FlatPipelinePlan FoveatedRender::BuildFlatPipelinePlan(
	uint32_t displayWidth, uint32_t displayHeight) const
{
	FlatPipelinePlan plan{};
	plan.displayWidth = displayWidth;
	plan.displayHeight = displayHeight;
	plan.act1Backend = static_cast<FlatAct1Backend>(std::min(settings.flatPipelineAct1Backend, 2u));
	plan.act2Backend = static_cast<FlatAct2Backend>(std::min(settings.flatPipelineAct2Backend, 4u));
	plan.act1Dsr = settings.flatPipelineAct1Dsr && plan.act1Backend != FlatAct1Backend::kBypass;
	plan.sharpen = settings.flatPipelineSharpen;
	if (!displayWidth || !displayHeight)
		return plan;

	const auto scaled = [](uint32_t value, uint quality, bool dsr) {
		const float multiplier = dsr ? 2.0f : 1.0f;
		return std::max(1u, static_cast<uint32_t>(std::lround(
			value * multiplier / Upscaling::GetQualityModeRatio(std::min(quality, 4u)))));
	};

	if (plan.act1Backend == FlatAct1Backend::kBypass) {
		plan.engineWidth = displayWidth;
		plan.engineHeight = displayHeight;
		plan.act1OutputWidth = displayWidth;
		plan.act1OutputHeight = displayHeight;
	} else {
		const uint act1Quality = plan.act1Dsr ? 4u : settings.flatPipelineAct1Quality;
		plan.engineWidth = std::min(displayWidth, scaled(displayWidth, act1Quality, plan.act1Dsr));
		plan.engineHeight = std::min(displayHeight, scaled(displayHeight, act1Quality, plan.act1Dsr));
		plan.act1OutputWidth = displayWidth * (plan.act1Dsr ? 2u : 1u);
		plan.act1OutputHeight = displayHeight * (plan.act1Dsr ? 2u : 1u);
	}

	plan.nrWidth = std::min(plan.act1OutputWidth, std::max(1u, static_cast<uint32_t>(
		std::lround(displayWidth * settings.flatPipelineNrScale))));
	plan.nrHeight = std::min(plan.act1OutputHeight, std::max(1u, static_cast<uint32_t>(
		std::lround(displayHeight * settings.flatPipelineNrScale))));
	plan.nrOutputWidth = plan.nrWidth;
	plan.nrOutputHeight = plan.nrHeight;
	plan.act2OutputWidth = displayWidth;
	plan.act2OutputHeight = displayHeight;
	if (plan.act2Backend == FlatAct2Backend::kDLSS) {
		float closestScaleDistance = std::numeric_limits<float>::max();
		const float actualScale = static_cast<float>(plan.nrWidth) / static_cast<float>(displayWidth);
		for (uint candidate = 1; candidate <= 4; ++candidate) {
			const float candidateScale = 1.0f / Upscaling::GetQualityModeRatio(candidate);
			const float distance = std::abs(candidateScale - actualScale);
			if (distance < closestScaleDistance) {
				closestScaleDistance = distance;
				plan.act2Quality = candidate;
			}
		}
		struct DlssRangeCache
		{
			uint32_t quality = UINT32_MAX;
			uint32_t outputWidth = 0;
			uint32_t outputHeight = 0;
			uint32_t requestedWidth = 0;
			uint32_t requestedHeight = 0;
			uint32_t clampedWidth = 0;
			uint32_t clampedHeight = 0;
		};
		static DlssRangeCache rangeCache;
		if (Upscaling::streamline.slDLSSGetOptimalSettings) {
			if (rangeCache.quality != plan.act2Quality || rangeCache.outputWidth != displayWidth ||
				rangeCache.outputHeight != displayHeight || rangeCache.requestedWidth != plan.nrWidth ||
				rangeCache.requestedHeight != plan.nrHeight) {
				rangeCache = { plan.act2Quality, displayWidth, displayHeight, plan.nrWidth, plan.nrHeight,
					plan.nrWidth, plan.nrHeight };
				Upscaling::streamline.ClampToDLSSRenderRange(plan.act2Quality, displayWidth, displayHeight,
					rangeCache.clampedWidth, rangeCache.clampedHeight);
			}
			plan.nrWidth = rangeCache.clampedWidth;
			plan.nrHeight = rangeCache.clampedHeight;
		}
		plan.nrOutputWidth = plan.nrWidth;
		plan.nrOutputHeight = plan.nrHeight;
	}
	return plan;
}

// ============================================================================
// Activation + accessors
// ============================================================================

bool FoveatedRender::IsActive() const
{
	// Gate on DLSS/FSR being the *selected* method, not just available
	// (IsRuntimeSupported): otherwise foveation and its SSR consumer run under
	// TAA/None and the route derefs unallocated upscaler resources — the crash
	// seen under RenderDoc, whose DX12 swapchain disables DLSS.
	if (!enabledAtBoot || !IsRuntimeSupported())
		return false;
	const auto method = globals::features::upscaling.GetUpscaleMethod();
	if (method != Upscaling::UpscaleMethod::kDLSS && method != Upscaling::UpscaleMethod::kFSR)
		return false;

	// A Full Eye region pays the isolation/stretch overhead (snapshot copies, mask
	// clears, StretchDRS) for a subrect equal to the full frame -- no savings, real
	// cost. Skip the route so the standard full-frame path runs instead.
	const bool isFullEye = subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye();
	return !isFullEye;
}

bool FoveatedRender::ShouldForceVisualize() const
{
	using namespace std::chrono_literals;
	return std::chrono::steady_clock::now() - lastDragTime < 3s;
}

bool FoveatedRender::IsRuntimeSupported() const
{
	// FSR's host path has no adapter/runtime prerequisite, so VR alone is
	// sufficient to enable the option -- IsActive() is what actually gates
	// on the *selected* method (kDLSS/kFSR) being one the route supports.
	return globals::game::isVR;
}

FoveatedRender::DlssMode FoveatedRender::GetDlssMode() const
{
	if (globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR)
		return DlssMode::kDefault;
	return (DlssMode)std::min(settings.dlssMode, 1u);
}

FoveatedRender::FoveationProfile FoveatedRender::GetFoveationProfile() const
{
	FoveationProfile profile;
	if (!IsActive())
		return profile;

	const auto& leftUV = subrectController.GetUV();
	const auto& rightUV = subrectController.GetRightEyeUV();

	// Map the rectangular subrect onto the centered superellipse the mask helper expects: vertical
	// extent drives coverageScale (radiusY = coverageScale/2), the rect aspect drives the horizontal
	// stretch (radiusX = coverageScale * hScale/2). The mask carries one scale for both eyes (only
	// the center offset is per-eye), so size comes from the less-foveated (larger) extent of the two:
	// the center is the superset enclosing both eyes' full-quality regions, so neither eye's sharp
	// zone is ever foveated (min would shrink it below an eye's sharp region and foveate it). A
	// full eye therefore yields full coverage and disables foveation (the gate below).
	const float coverageH = std::max(leftUV.h, rightUV.h);
	const float coverageW = std::max(leftUV.w, rightUV.w);
	const float coverageScale = FoveatedCommon::ClampCenterScale(coverageH);

	// Availability keys off the clamped scale: if the larger eye rounds up to full coverage there is
	// nothing to foveate, leave the default (available == false).
	if (!FoveatedCommon::IsActiveCoverage(coverageScale))
		return profile;

	profile.available = true;
	profile.coverageScale = coverageScale;
	profile.centerHorizontalScale = FoveatedCommon::ClampCenterHorizontalScale(
		coverageH > 1e-4f ? coverageW / coverageH : 1.0f);
	profile.centerOffsets[0] = float2{ (leftUV.x + leftUV.w * 0.5f) - 0.5f, (leftUV.y + leftUV.h * 0.5f) - 0.5f };
	profile.centerOffsets[1] = float2{ (rightUV.x + rightUV.w * 0.5f) - 0.5f, (rightUV.y + rightUV.h * 0.5f) - 0.5f };
	return profile;
}

void FoveatedRender::LatchQualityMode()
{
	qualityModeAtBoot = std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActiveQualityMode() const
{
	return std::clamp(globals::features::upscaling.settings.qualityMode, 1u, 4u);
}

uint FoveatedRender::GetActivePresetDLSS() const
{
	return std::min(globals::features::upscaling.settings.presetDLSS, 5u);
}

float FoveatedRender::GetActiveSharpnessDLSS() const
{
	return std::clamp(globals::features::upscaling.settings.sharpnessDLSS, 0.0f, 1.0f);
}

float FoveatedRender::GetRenderScaleForQuality(uint qualityMode)
{
	return Upscaling::GetQualityModeRatio(qualityMode);
}

bool FoveatedRender::IsPresetCompatibleWithMode(uint presetIndex) const
{
	// Preset indices: 0=Default, 1=J, 2=K, 3=L, 4=M, 5=F
	// Faster mode: J(1) and K(2) are incompatible.
	if (GetDlssMode() == DlssMode::kFaster) {
		return presetIndex != 1 && presetIndex != 2;
	}
	return true;
}

void FoveatedRender::ClampPresetToMode()
{
	auto& sharedPreset = globals::features::upscaling.settings.presetDLSS;
	if (!IsPresetCompatibleWithMode(sharedPreset)) {
		sharedPreset = 3;  // Fall back to L
	}
}

// ============================================================================
// UI — FoveatedRender-specific knobs only. Quality / sharpness / preset /
// Streamline log level live on Upscaling's panel and apply to both DLSS paths.
// Called from Upscaling::DrawSettings inside a TreeNode.
// ============================================================================

void FoveatedRender::DrawEnable()
{
	ClampSettings();

	ImGui::TextWrapped("%s", T(TKEY("foveated_overview"),
								 "Foveated subrect upscaling: only the user-selected region gets full DLSS/FSR "
								 "upscaling, the periphery is cheaply stretched. Significant upscaler cost reduction "
								 "at the cost of peripheral sharpness. VR only."));

	const bool runtimeSupported = IsRuntimeSupported();
	if (!runtimeSupported) {
		settings.enabled = 0;
	}

	if (!runtimeSupported)
		ImGui::BeginDisabled();
	bool enabledBool = settings.enabled != 0;
	if (ImGui::Checkbox(T(TKEY("foveated_enable"), "Enable Foveated Upscaling (region source)"), &enabledBool)) {
		settings.enabled = enabledBool ? 1u : 0u;
		// Full Eye is a deliberate no-op (see IsActive()) -- enabling straight from
		// it would silently do nothing until the user finds the region picker below.
		if (enabledBool && subrectController.GetUV().IsFullEye() && subrectController.GetRightEyeUV().IsFullEye())
			subrectController.ApplyPresetByName(kPresetCenter75);
	}
	if (!runtimeSupported)
		ImGui::EndDisabled();

	Util::UI::DrawSettingDiff(bootSnapshot, settings, &Settings::enabled);

	if (enabledAtBoot) {
		const auto method = globals::features::upscaling.GetUpscaleMethod();
		const bool methodOk = method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR;
		if (IsActive())
			Util::Text::WrappedInfo("%s", T(TKEY("foveated_active"), "Active: foveated subrect upscaling is enabled (skipped in the main and loading menus / on preflight failure)."));
		else if (!methodOk)
			Util::Text::Warning("%s", T(TKEY("foveated_standing_by"), "Standing by: only active while the Upscaling Method is DLSS or FSR. Inactive right now."));
		else
			Util::Text::Warning("%s", T(TKEY("foveated_standing_by_full_eye"), "Standing by: region is Full Eye (no crop) -- shrink it in Subrect Region below to see savings."));
	}

	if (!globals::game::isVR) {
		Util::Text::Warning("%s", T(TKEY("foveated_vr_only"), "VR only -- flat has no equivalent lens-driven periphery quality cliff to exploit."));
	}
}

const char* FoveatedRender::DlssModeName(DlssMode mode)
{
	return mode == DlssMode::kFaster ?
	           T(TKEY("foveated_dlss_mode_faster"), "Faster") :
	           T(TKEY("foveated_dlss_mode_default"), "Default");
}

const char* FoveatedRender::StretchModeName(StretchMode mode)
{
	switch (mode) {
	case StretchMode::kPoint:
		return T(TKEY("foveated_stretch_point"), "Point");
	case StretchMode::kGaussianBlur:
		return T(TKEY("foveated_stretch_gaussian"), "Gaussian Blur");
	default:
		return T(TKEY("foveated_stretch_bilinear"), "Bilinear");
	}
}

const char* FoveatedRender::PeripheryAAModeName(PeripheryAAMode mode)
{
	return mode == PeripheryAAMode::kTemporalSmooth ?
	           T(TKEY("foveated_periphery_aa_temporal"), "Temporal Smooth") :
	           T(TKEY("foveated_periphery_aa_none"), "None");
}

const char* FoveatedRender::SubrectBlendModeName(SubrectBlendMode mode)
{
	switch (mode) {
	case SubrectBlendMode::kFeather:
		return T(TKEY("foveated_blend_feather"), "Feather");
	case SubrectBlendMode::kDither:
		return T(TKEY("foveated_blend_dither"), "Dither");
	default:
		return T(TKEY("foveated_blend_hard_copy"), "Hard Copy");
	}
}

const char* FoveatedRender::SubrectMaskModeName(SubrectMaskMode mode)
{
	return mode == SubrectMaskMode::kOval ?
	           T(TKEY("foveated_mask_oval"), "Oval") :
	           T(TKEY("foveated_mask_rectangle"), "Rectangle");
}

void FoveatedRender::DrawNeuralRenderingSettings()
{
	ClampSettings();

	if (ImGui::CollapsingHeader(T(TKEY("neural_rendering_header"), "DLSS Neural Rendering"), ImGuiTreeNodeFlags_DefaultOpen)) {
		auto& upscaling = globals::features::upscaling;
		static const char* qualityModes[] = { "DLAA / Native", "Quality", "Balanced", "Performance", "Ultra Performance" };
		static const char* dlssPresets[] = { "Default", "Preset J", "Preset K", "Preset L", "Preset M" };
		static const char* jitterModes[] = { "Actual Camera Jitter", "Zero" };
		if (!globals::game::isVR) {
			static const char* act1Backends[] = { "Bypass", "DLSS", "FSR3" };
			ImGui::TextDisabled("Injection Point: PostInject LDR / Before UI (fixed)");
			ImGui::TextDisabled("Execution: ACT1 -> NR -> ACT2 -> RCAS in one hook");
			ImGui::Separator();
			ImGui::TextUnformatted("ACT1 - LDR Reconstruction");
			ImGui::Combo("ACT1 Backend", reinterpret_cast<int*>(&settings.flatPipelineAct1Backend), act1Backends, IM_ARRAYSIZE(act1Backends));
			if (settings.flatPipelineAct1Backend != 0) {
				ImGui::Combo("ACT1 Quality", reinterpret_cast<int*>(&settings.flatPipelineAct1Quality), qualityModes, IM_ARRAYSIZE(qualityModes));
				if (settings.flatPipelineAct1Backend == static_cast<uint>(FlatAct1Backend::kDLSS))
					ImGui::Combo("ACT1 DLSS Preset", reinterpret_cast<int*>(&settings.flatPipelineAct1Preset), dlssPresets, IM_ARRAYSIZE(dlssPresets));
				ImGui::Combo("ACT1 Jitter Submission", reinterpret_cast<int*>(&settings.flatPipelineAct1Jitter), jitterModes, IM_ARRAYSIZE(jitterModes));
				ImGui::Checkbox("ACT1 DSR 2x", &settings.flatPipelineAct1Dsr);
			}
			ImGui::Separator();
		}

		const bool supportedRoute = globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS &&
			!globals::features::upscaling.IsFrameGenerationConfiguredForSession() &&
			(!globals::game::isVR || (GetDlssMode() == DlssMode::kDefault &&
				globals::features::upscaling.perfMode.IsHookActive()));
		if (!supportedRoute) {
			if (globals::features::upscaling.IsFrameGenerationConfiguredForSession())
				Util::Text::Warning("Disable Frame Generation and restart the game before enabling DLSS Neural Rendering.");
			else
				Util::Text::Warning(T(TKEY("neural_rendering_unavailable"),
					"Requires DLSS. VR additionally requires Foveated Default mode and active PerfMode."));
			ImGui::BeginDisabled();
		}
		ImGui::TextUnformatted("Neural Rendering");
		ImGui::Checkbox(T(TKEY("neural_rendering_enable"), "Enable DLSS Neural Rendering"), &settings.neuralRenderingEnabled);

		if (settings.neuralRenderingEnabled) {
			if (!globals::game::isVR) {
				ImGui::Separator();
				ImGui::TextUnformatted("NR");
				ImGui::SliderFloat("NR Resolution Scale", &settings.flatPipelineNrScale, 0.33f, 0.95f, "%.2f");
				{
					const auto preview = BuildFlatPipelinePlan(
						globals::state ? static_cast<uint32_t>(globals::state->screenSize.x) : 0u,
						globals::state ? static_cast<uint32_t>(globals::state->screenSize.y) : 0u);
					const double displayPixels = static_cast<double>(preview.displayWidth) * preview.displayHeight;
					const double nrPixels = static_cast<double>(preview.nrWidth) * preview.nrHeight;
					ImGui::TextDisabled("NR extent: %ux%u | %.1f%% pixels",
						preview.nrWidth, preview.nrHeight, displayPixels > 0.0 ? nrPixels * 100.0 / displayPixels : 0.0);
				}
				{
					static const char* act2Backends[] = { "DLSS", "FSR3", "NIS", "Fusion", "HQ Lanczos" };
					ImGui::Separator();
					ImGui::TextUnformatted("ACT2");
					ImGui::Combo("ACT2 Backend", reinterpret_cast<int*>(&settings.flatPipelineAct2Backend), act2Backends, IM_ARRAYSIZE(act2Backends));
					if (settings.flatPipelineAct2Backend == static_cast<uint>(FlatAct2Backend::kDLSS))
						ImGui::Combo("ACT2 DLSS Preset", reinterpret_cast<int*>(&settings.flatPipelineAct2Preset), dlssPresets, IM_ARRAYSIZE(dlssPresets));
					if (settings.flatPipelineAct2Backend == static_cast<uint>(FlatAct2Backend::kDLSS) ||
						settings.flatPipelineAct2Backend == static_cast<uint>(FlatAct2Backend::kFSR3))
						ImGui::Combo("ACT2 Jitter Submission", reinterpret_cast<int*>(&settings.flatPipelineAct2Jitter), jitterModes, IM_ARRAYSIZE(jitterModes));
					if (settings.flatPipelineAct2Backend == static_cast<uint>(FlatAct2Backend::kFusion)) {
						static const char* fusionDirections[] = { "ACT1 guides NR", "NR guides ACT1" };
						static const char* denoiseModes[] = { "Off", "Residual Bilateral", "Soft Gaussian", "Luma Preserve", "Strong Clean" };
						static const char* fusionPresets[] = { "Custom", "Clean", "Soft", "Dreamy", "Detail", "NR Led" };
						int fusionPreset = static_cast<int>(settings.neuralRenderingFusionPreset);
						if (ImGui::Combo("Fusion Preset", &fusionPreset, fusionPresets, IM_ARRAYSIZE(fusionPresets))) {
							settings.neuralRenderingFusionPreset = static_cast<uint>(fusionPreset);
							switch (settings.neuralRenderingFusionPreset) {
							case 1: settings.flatPipelineFusionDirection = 0; settings.flatPipelineFusionDenoise = 1; settings.flatPipelineFusionDenoiseStrength = 0.55f; settings.flatPipelineFusionSoftness = 0.25f; settings.flatPipelineFusionFrequencyGuidance = 0.70f; settings.flatPipelineFusionDepthGuidance = 0.80f; settings.neuralRenderingFusionLuma = 0.75f; settings.neuralRenderingFusionChroma = 0.50f; settings.neuralRenderingFusionEdgeProtection = 0.45f; settings.neuralRenderingFusionResidualLimit = 0.12f; break;
							case 2: settings.flatPipelineFusionDirection = 0; settings.flatPipelineFusionDenoise = 2; settings.flatPipelineFusionDenoiseStrength = 0.65f; settings.flatPipelineFusionSoftness = 0.65f; settings.flatPipelineFusionFrequencyGuidance = 0.75f; settings.flatPipelineFusionDepthGuidance = 0.80f; settings.neuralRenderingFusionLuma = 0.65f; settings.neuralRenderingFusionChroma = 0.40f; settings.neuralRenderingFusionEdgeProtection = 0.50f; settings.neuralRenderingFusionResidualLimit = 0.10f; break;
							case 3: settings.flatPipelineFusionDirection = 0; settings.flatPipelineFusionDenoise = 4; settings.flatPipelineFusionDenoiseStrength = 0.80f; settings.flatPipelineFusionSoftness = 0.85f; settings.flatPipelineFusionFrequencyGuidance = 0.85f; settings.flatPipelineFusionDepthGuidance = 0.90f; settings.neuralRenderingFusionLuma = 0.55f; settings.neuralRenderingFusionChroma = 0.35f; settings.neuralRenderingFusionEdgeProtection = 0.60f; settings.neuralRenderingFusionResidualLimit = 0.08f; break;
							case 4: settings.flatPipelineFusionDirection = 1; settings.flatPipelineFusionDenoise = 1; settings.flatPipelineFusionDenoiseStrength = 0.40f; settings.flatPipelineFusionSoftness = 0.20f; settings.flatPipelineFusionFrequencyGuidance = 0.45f; settings.flatPipelineFusionDepthGuidance = 0.65f; settings.neuralRenderingFusionLuma = 0.80f; settings.neuralRenderingFusionChroma = 0.45f; settings.neuralRenderingFusionEdgeProtection = 0.30f; settings.neuralRenderingFusionResidualLimit = 0.14f; break;
							case 5: settings.flatPipelineFusionDirection = 1; settings.flatPipelineFusionDenoise = 3; settings.flatPipelineFusionDenoiseStrength = 0.55f; settings.flatPipelineFusionSoftness = 0.45f; settings.flatPipelineFusionFrequencyGuidance = 0.60f; settings.flatPipelineFusionDepthGuidance = 0.75f; settings.neuralRenderingFusionLuma = 0.55f; settings.neuralRenderingFusionChroma = 0.35f; settings.neuralRenderingFusionEdgeProtection = 0.45f; settings.neuralRenderingFusionResidualLimit = 0.11f; break;
							default: break;
							}
						}
						ImGui::Combo("Fusion Direction", reinterpret_cast<int*>(&settings.flatPipelineFusionDirection), fusionDirections, IM_ARRAYSIZE(fusionDirections));
						ImGui::Combo("Fusion Denoise", reinterpret_cast<int*>(&settings.flatPipelineFusionDenoise), denoiseModes, IM_ARRAYSIZE(denoiseModes));
						bool customFusion = false;
						customFusion |= ImGui::SliderFloat("Fusion Denoise Strength", &settings.flatPipelineFusionDenoiseStrength, 0.0f, 1.0f, "%.2f");
						customFusion |= ImGui::SliderFloat("Fusion Softness", &settings.flatPipelineFusionSoftness, 0.0f, 1.0f, "%.2f");
						ImGui::Checkbox("Enable Enhanced Guidance", &settings.flatPipelineFusionEnhancedGuidance);
						if (settings.flatPipelineFusionEnhancedGuidance) {
							customFusion |= ImGui::SliderFloat("Fusion Frequency Guidance", &settings.flatPipelineFusionFrequencyGuidance, 0.0f, 1.0f, "%.2f");
							customFusion |= ImGui::SliderFloat("Fusion Depth Guidance", &settings.flatPipelineFusionDepthGuidance, 0.0f, 1.0f, "%.2f");
							customFusion |= ImGui::SliderFloat("Fusion Color Guidance", &settings.flatPipelineFusionColorGuidance, 0.0f, 1.0f, "%.2f");
						}
						customFusion |= ImGui::SliderFloat("Fusion Luma", &settings.neuralRenderingFusionLuma, 0.0f, 1.5f, "%.2f");
						customFusion |= ImGui::SliderFloat("Fusion Chroma", &settings.neuralRenderingFusionChroma, 0.0f, 1.5f, "%.2f");
						customFusion |= ImGui::SliderFloat("Fusion Edge Protection", &settings.neuralRenderingFusionEdgeProtection, 0.0f, 1.0f, "%.2f");
						customFusion |= ImGui::SliderFloat("Fusion Residual Limit", &settings.neuralRenderingFusionResidualLimit, 0.01f, 0.5f, "%.3f");
						if (customFusion)
							settings.neuralRenderingFusionPreset = 0;
					}
				}
				ImGui::Separator();
				ImGui::TextUnformatted("Final Sharpen");
				ImGui::Checkbox("Enable RCAS", &settings.flatPipelineSharpen);
				if (settings.flatPipelineSharpen)
					ImGui::SliderFloat("RCAS Sharpness", &settings.flatPipelineSharpness, 0.0f, 1.0f, "%.2f");
			}
			ImGui::Separator();
			ImGui::TextUnformatted("NR Tuning");
			static const char* presets[] = { "Custom", "Balanced", "Fabric Detail", "Natural", "Strong" };
			int preset = static_cast<int>(settings.neuralRenderingPreset);
			if (ImGui::Combo(T(TKEY("neural_rendering_preset"), "Tuning Preset"), &preset, presets, IM_ARRAYSIZE(presets))) {
				settings.neuralRenderingPreset = static_cast<uint>(preset);
				switch (settings.neuralRenderingPreset) {
				case 1: settings.neuralRenderingIntensity = 1.0f; settings.neuralRenderingLocalTone = 1.0f; settings.neuralRenderingLocalStructure = 1.0f; settings.neuralRenderingSkinStructure = 1.0f; break;
				case 2: settings.neuralRenderingIntensity = 1.35f; settings.neuralRenderingLocalTone = 0.9f; settings.neuralRenderingLocalStructure = 1.6f; settings.neuralRenderingSkinStructure = 1.15f; break;
				case 3: settings.neuralRenderingIntensity = 0.8f; settings.neuralRenderingLocalTone = 0.75f; settings.neuralRenderingLocalStructure = 0.9f; settings.neuralRenderingSkinStructure = 0.9f; break;
				case 4: settings.neuralRenderingIntensity = 1.75f; settings.neuralRenderingLocalTone = 1.25f; settings.neuralRenderingLocalStructure = 1.5f; settings.neuralRenderingSkinStructure = 1.3f; break;
				default: break;
				}
			}
			bool custom = false;
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_intensity"), "Intensity"), &settings.neuralRenderingIntensity, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_local_tone"), "Local Tone"), &settings.neuralRenderingLocalTone, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_local_structure"), "Local Structure"), &settings.neuralRenderingLocalStructure, 0.0f, 2.0f, "%.2f");
			custom |= ImGui::SliderFloat(T(TKEY("neural_rendering_skin_structure"), "Skin Structure"), &settings.neuralRenderingSkinStructure, 0.0f, 2.0f, "%.2f");
			static const char* styles[] = { "Style 0", "Style 1", "Style 2", "Style 3" };
			int style = static_cast<int>(settings.neuralRenderingStyle);
			if (ImGui::Combo(T(TKEY("neural_rendering_style"), "Style"), &style, styles, IM_ARRAYSIZE(styles))) {
				settings.neuralRenderingStyle = static_cast<uint>(style);
				custom = true;
			}
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_auto_mask"), "Automatic Mask"), &settings.neuralRenderingAutoMask);
			custom |= ImGui::Checkbox(T(TKEY("neural_rendering_ui_correction"), "UI Correction"), &settings.neuralRenderingUICorrection);
			if (custom)
				settings.neuralRenderingPreset = 0;

			if (ImGui::TreeNodeEx("Diagnostics")) {
				const auto stats = NeuralRendering::GetAct2BackendStats();
				ImGui::TextDisabled("Actual jitter: %.4f, %.4f", upscaling.jitter.x, upscaling.jitter.y);
				ImGui::TextDisabled("ACT1 submits: %s | ACT2 submits: %s",
					(globals::game::isVR ? upscaling.settings.flatAct1JitterMode : settings.flatPipelineAct1Jitter) == 0 ? "Actual" : "Zero",
					(globals::game::isVR ? upscaling.settings.flatAct2JitterMode : settings.flatPipelineAct2Jitter) == 0 ? "Actual" : "Zero");
				ImGui::TextDisabled("ACT1 DLSS successful frames: %llu",
					static_cast<unsigned long long>(Upscaling::streamline.flatAct1SuccessfulFrames));
				ImGui::TextDisabled("ACT2 successful frames: NIS %llu | DLSS %llu | FSR3 %llu",
					static_cast<unsigned long long>(stats.nisSucceeded),
					static_cast<unsigned long long>(stats.dlssSucceeded),
					static_cast<unsigned long long>(stats.fsrSucceeded));
				ImGui::Checkbox("Bypass NR Output (SR Comparison)", &settings.neuralRenderingBypassOutput);
				if (!globals::game::isVR) {
					ImGui::Checkbox("Flat DLSS Input Isolation", &upscaling.settings.flatDlssInputIsolation);
					Util::UI::RestartGatedAnnotate(upscaling.bootSnapshot, upscaling.settings,
						&Upscaling::Settings::flatDlssInputIsolation,
						"Copies the active viewport into exact RenderRes inputs. Restart required.");
					ImGui::Checkbox("Reset NR History Every Frame", &settings.neuralRenderingResetEveryFrame);
					if (globals::state && globals::state->IsDeveloperMode()) {
						static const char* inputPrefilters[] = { "Off", "77% Roundtrip", "66% Roundtrip", "50% Roundtrip" };
						int inputPrefilter = static_cast<int>(settings.neuralRenderingInputPrefilter);
						if (ImGui::Combo("Full NR Input Prefilter", &inputPrefilter, inputPrefilters, IM_ARRAYSIZE(inputPrefilters)))
							settings.neuralRenderingInputPrefilter = static_cast<uint>(inputPrefilter);
						static const char* motionVectorModes[] = {
							"Dilated", "Raw Engine MV", "Zero MV", "Inverted MV", "0.5x MV", "2.0x MV"
						};
						int motionVectorMode = static_cast<int>(settings.neuralRenderingMotionVectorMode);
						if (ImGui::Combo("NR Motion Vector Test", &motionVectorMode, motionVectorModes, IM_ARRAYSIZE(motionVectorModes)))
							settings.neuralRenderingMotionVectorMode = static_cast<uint>(motionVectorMode);
					}
				}
				ImGui::TreePop();
			}

			auto& neuralRenderer = NeuralRendering::Renderer::Instance();
			if (neuralRenderer.IsFailureLatched()) {
				Util::Text::Warning("DLSS Neural Rendering failed and is disabled for this session. Check CommunityShaders.log.");
				if (ImGui::Button("Reset Neural Rendering Failure"))
					neuralRenderer.Reset();
			}
			if (globals::state && globals::state->IsDeveloperMode()) {
				ImGui::TextDisabled("Status: %s | NGX: 0x%08X | Evaluations: %llu",
					neuralRenderer.StatusText(), neuralRenderer.NgxResult(),
					static_cast<unsigned long long>(neuralRenderer.SuccessfulFrames()));
			}
			static double endToEndGpuMs = 0.0;
			static std::uint32_t observedCapturedFrame = 0;
			static std::uint32_t endToEndCapturedFrame = 0;
			bool endToEndAvailable = endToEndCapturedFrame != 0;
			if (globals::profiler) {
				globals::profiler->RequestCapture();
				const std::uint32_t capturedFrame = globals::profiler->GetCapturedFrameCount();
				const std::string_view timerName = globals::game::isVR ?
					"NeuralRendering::EvaluateStereo" : "NeuralRendering::Evaluate";
				if (capturedFrame != observedCapturedFrame) {
					observedCapturedFrame = capturedFrame;
					endToEndCapturedFrame = 0;
					endToEndAvailable = false;
					for (const auto& result : globals::profiler->GetResults()) {
						if (result.name == timerName && result.activeGpu) {
							endToEndGpuMs = result.gpuTimeMs;
							endToEndCapturedFrame = capturedFrame;
							endToEndAvailable = true;
							break;
						}
					}
				}
			}
			const double dx12TotalGpuMs = neuralRenderer.LastD3D12TotalGpuMs();
			const double pureNgxGpuMs = neuralRenderer.LastEvaluationGpuMs();
			const double endToEndSampleGpuMs = endToEndGpuMs;
			const double dx12WrapperGpuMs = std::max(0.0, dx12TotalGpuMs - pureNgxGpuMs);
			const double interopResidualGpuMs = endToEndAvailable ? std::max(0.0, endToEndSampleGpuMs - dx12TotalGpuMs) : 0.0;
			const auto percent = [endToEndSampleGpuMs, endToEndAvailable](double value) {
				return endToEndAvailable && endToEndSampleGpuMs > 0.0 ? value * 100.0 / endToEndSampleGpuMs : 0.0;
			};
			if (endToEndAvailable)
				ImGui::TextDisabled("NR End-to-End GPU: %.3f ms | captured frame: %u", endToEndGpuMs, endToEndCapturedFrame);
			else
				ImGui::TextDisabled("NR End-to-End GPU: waiting for profiler capture");
			if (endToEndAvailable) {
				ImGui::TextDisabled("NR DX12 Total GPU: %.3f ms (%.1f%%) | samples: %llu",
					dx12TotalGpuMs, percent(dx12TotalGpuMs),
					static_cast<unsigned long long>(neuralRenderer.D3D12TotalTimingSamples()));
				ImGui::TextDisabled("NR Pure NGX GPU: %.3f ms (%.1f%%) | samples: %llu",
					pureNgxGpuMs, percent(pureNgxGpuMs),
					static_cast<unsigned long long>(neuralRenderer.EvaluationTimingSamples()));
				ImGui::TextDisabled("DX12 wrapper: %.3f ms (%.1f%%)", dx12WrapperGpuMs, percent(dx12WrapperGpuMs));
				ImGui::TextDisabled("Interop/copy/fence residual: %.3f ms (%.1f%%)",
					interopResidualGpuMs, percent(interopResidualGpuMs));
			} else {
				ImGui::TextDisabled("NR DX12 Total GPU: %.3f ms | share: N/A | samples: %llu",
					dx12TotalGpuMs, static_cast<unsigned long long>(neuralRenderer.D3D12TotalTimingSamples()));
				ImGui::TextDisabled("NR Pure NGX GPU: %.3f ms | share: N/A | samples: %llu",
					pureNgxGpuMs, static_cast<unsigned long long>(neuralRenderer.EvaluationTimingSamples()));
				ImGui::TextDisabled("DX12 wrapper: %.3f ms | share: N/A", dx12WrapperGpuMs);
				ImGui::TextDisabled("Interop/copy/fence residual: N/A");
			}
			ImGui::TextDisabled("Percentages compare latest asynchronous samples.");
			ImGui::TextDisabled("Interop CPU backpressure: %llu | last wait: %.1f ms",
				static_cast<unsigned long long>(neuralRenderer.CpuBackpressureCount()),
				neuralRenderer.LastCpuBackpressureMs());
		}
		if (!supportedRoute)
			ImGui::EndDisabled();
	}
}

void FoveatedRender::DrawSettings()
{
	ClampSettings();

	Util::Text::WrappedInfo("%s", T(TKEY("foveated_shared_panel_note"), "Quality and Sharpness are on the main Upscaling panel — changes there apply to foveated rendering too. DLSS Preset also applies there when DLSS is the selected upscaler."));

	// ── VR-only knobs ──
	if (globals::game::isVR) {
		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_dlss_mode_header"), "VR DLSS Mode"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_dlss_mode_tooltip"),
								  "Default — highest quality. Each eye gets its own isolated copy of color/depth/motion\n"
								  "vectors so DLSS can't sample across the stereo midline. 5 copies per eye per frame.\n"
								  "All DLSS presets supported. Best for screenshots or when Faster shows edge artifacts.\n"
								  "\n"
								  "Faster — lower overhead. DLSS reads directly from the frame buffer using a viewport\n"
								  "offset instead of isolating each eye. 1 snapshot + 2 mask clears per frame.\n"
								  "DLSS may sample 1-2 pixels from the neighboring eye near the stereo center — usually\n"
								  "invisible in motion. Presets J and K are incompatible and auto-clamp to L."));
		}

		const bool isFSR = globals::features::upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kFSR;
		if (isFSR)
			ImGui::BeginDisabled();
		uint prevMode = settings.dlssMode;
		ImGui::SliderInt(T(TKEY("foveated_dlss_mode_label"), "DLSS Mode"), reinterpret_cast<int*>(&settings.dlssMode), 0, 1, DlssModeName((DlssMode)std::min(settings.dlssMode, 1u)));
		if (settings.dlssMode != prevMode) {
			const uint prevPreset = globals::features::upscaling.settings.presetDLSS;
			ClampPresetToMode();
			if (globals::features::upscaling.settings.presetDLSS != prevPreset) {
				logger::info("[FOVEATED] DLSS preset clamped from {} to {} after mode switch (J/K incompatible with Faster)",
					prevPreset, globals::features::upscaling.settings.presetDLSS);
			}
		}
		if (isFSR) {
			ImGui::EndDisabled();
			ImGui::TextWrapped("%s", T(TKEY("foveated_dlss_mode_fsr_desc"), "Not used by FSR -- applies only when DLSS is the selected upscaler."));
		} else {
			switch (GetDlssMode()) {
			case DlssMode::kDefault:
				ImGui::TextWrapped("%s", T(TKEY("foveated_dlss_mode_default_desc"), "Per-eye isolation: 5 copies per frame, 2 DLSS evaluates. All presets."));
				break;
			case DlssMode::kFaster:
				ImGui::TextWrapped("%s", T(TKEY("foveated_dlss_mode_faster_desc"), "Viewport offset: 1 snapshot, 2 mask clears, 2 DLSS evaluates. Presets J/K unavailable."));
				break;
			default:
				break;
			}
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_periphery_header"), "Periphery Rendering"));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_periphery_tooltip"),
								  "The area outside your selected subrect is filled cheaply rather than running\n"
								  "the selected upscaler. These settings control how that cheap fill looks and\n"
								  "whether it flickers.\n"
								  "\n"
								  "Stretch method: how pixels outside the subrect are reconstructed from the lower-res\n"
								  "render buffer. Does not affect the upscaled subrect region at all.\n"
								  "\n"
								  "Periphery AA: reduces temporal flicker in the stretched area using motion-compensated\n"
								  "history blending. Independent of the upscaled subrect.\n"
								  "\n"
								  "Edge Blend: controls how the upscaled subrect edge meets the stretched periphery.\n"
								  "Hard Copy leaves a sharp seam; Feather/Dither soften it. Edge Shape selects a\n"
								  "rectangle or oval composite."));
		}

		ImGui::SliderInt(T(TKEY("foveated_stretch_label"), "Stretch"), reinterpret_cast<int*>(&settings.stretchMode), 0, 2, StretchModeName((StretchMode)settings.stretchMode));
		switch (GetStretchMode()) {
		case StretchMode::kBilinear:
			ImGui::TextWrapped("%s", T(TKEY("foveated_stretch_bilinear_desc"), "Bilinear: smooth upscale of the render buffer. Looks soft but clean."));
			break;
		case StretchMode::kPoint:
			ImGui::TextWrapped("%s", T(TKEY("foveated_stretch_point_desc"), "Point: cheapest, visibly pixelated. Good for benchmarking foveated savings."));
			break;
		case StretchMode::kGaussianBlur:
			ImGui::TextWrapped("%s", T(TKEY("foveated_stretch_gaussian_desc"), "Gaussian: blurs the periphery further into soft focus. Good default for foveated use."));
			ImGui::SliderFloat(T(TKEY("foveated_blur_radius"), "Blur Radius"), &settings.peripheryBlurRadius, 0.5f, 4.0f, "%.1f px");
			break;
		}

		ImGui::SliderInt(T(TKEY("foveated_periphery_aa_label"), "Periphery AA"), reinterpret_cast<int*>(&settings.peripheryAAMode), 0, 1, PeripheryAAModeName((PeripheryAAMode)settings.peripheryAAMode));
		if (GetPeripheryAAMode() == PeripheryAAMode::kTemporalSmooth) {
			ImGui::TextWrapped("%s", T(TKEY("foveated_periphery_aa_temporal_desc"), "Blends the stretched periphery with motion-reprojected history to reduce flicker."));
			ImGui::SliderFloat(T(TKEY("foveated_smoothing"), "Smoothing"), &settings.peripheryTemporalAlpha, 0.05f, 0.5f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_smoothing_tooltip"), "Lower = more temporal history (smoother but may ghost). Higher = more responsive."));
			}
		}

		ImGui::SliderInt(T(TKEY("foveated_edge_blend_label"), "Edge Blend"), reinterpret_cast<int*>(&settings.subrectBlendMode), 0, 2, SubrectBlendModeName((SubrectBlendMode)std::min(settings.subrectBlendMode, 2u)));
		switch (GetSubrectBlendMode()) {
		case SubrectBlendMode::kHardCopy:
			ImGui::TextWrapped("%s", T(TKEY("foveated_blend_hard_copy_desc"), "Sharp seam at the subrect boundary. Lowest cost."));
			break;
		case SubrectBlendMode::kFeather:
			ImGui::TextWrapped("%s", T(TKEY("foveated_blend_feather_desc"), "Smoothstep fade over N pixels at the boundary. Hides the seam. Falloff Curve reshapes the ramp."));
			ImGui::SliderFloat(T(TKEY("foveated_feather_width"), "Feather Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			break;
		case SubrectBlendMode::kDither:
			ImGui::TextWrapped("%s", T(TKEY("foveated_blend_dither_desc"), "Noise-dithered fade — more natural-looking than feather at large subrects."));
			ImGui::SliderFloat(T(TKEY("foveated_band_width"), "Band Width"), &settings.subrectFeatherWidth, 2.0f, 128.0f, "%.0f px");
			ImGui::SliderFloat(T(TKEY("foveated_noise_amount"), "Noise Amount"), &settings.subrectDitherStrength, 0.0f, 2.0f, "%.2f");
			break;
		}

		if (GetSubrectBlendMode() != SubrectBlendMode::kHardCopy) {
			ImGui::SliderFloat(T(TKEY("foveated_falloff_curve"), "Falloff Curve"), &settings.subrectFalloffCurve, 0.5f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_falloff_curve_tooltip"), "Controls how the transition distributes the fade. 1.00 is balanced; lower values carry the upscaled result farther into the band, higher values hold the periphery longer."));
			}

			ImGui::SliderInt(T(TKEY("foveated_mask_shape_label"), "Edge Shape"), reinterpret_cast<int*>(&settings.subrectMaskMode), 0, 1,
				SubrectMaskModeName(GetSubrectMaskMode()));
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("foveated_mask_shape_tooltip"), "Oval rounds the blended edge, so the subrect corners keep the stretched periphery instead of the upscaled image. The upscaler still processes the full rectangle either way."));
			}
		}

		ImGui::Separator();
		ImGui::Text("%s", T(TKEY("foveated_subrect_region_header"), "Subrect Region"));
		ImGui::TextWrapped("%s", T(TKEY("foveated_subrect_region_desc"),
									 "Drag in the preview below to select the region that gets full upscaling. "
									 "The rest is cheaply stretched — saves significant upscaling cost."));
		Util::Text::WrappedInfo("%s", T(TKEY("foveated_screenshot_subrect_note"), "Screenshot has its own subrect; align them only if you want pixel-matched captures."));

		bool debugBool = settings.debugVisualize != 0;
		if (ImGui::Checkbox(T(TKEY("foveated_visualize_regions"), "Visualize regions"), &debugBool))
			settings.debugVisualize = debugBool ? 1u : 0u;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("foveated_visualize_regions_tooltip"),
								  "Diagnostic: tint the cheap-stretched periphery red so the upscaled\n"
								  "subrect (un-tinted) pops visually in-game. Lets you confirm at a glance where\n"
								  "the selected upscaler is actually running vs where the cheap stretch is filling.\n"
								  "No perf impact; runtime toggle, no restart needed. Also shows briefly whenever\n"
								  "you drag-resize the region below, even with this off."));
		}

		// Preview off kVR_FRAMEBUFFER (the final composed SBS image the headset
		// sees) rather than kMAIN. kMAIN is mid-pipeline and carries non-1
		// alpha where Skyrim composited UI plates, so even with the opaque
		// blend callback you see the menu mask outline instead of the rendered
		// world. ScreenshotFeature picks the same RT for the same reason
		// (ScreenshotFeature.cpp:243). Foveated is VR-only so kVR_FRAMEBUFFER
		// is always populated when we get here.
		auto renderer = globals::game::renderer;
		if (renderer) {
			// Real object is VR-sized (kVRTOTAL) here; the multi-runtime build's
			// compile-time array type stays kTOTAL-sized.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warray-bounds"
			auto& fb = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kVR_FRAMEBUFFER];
#pragma clang diagnostic pop
			auto* tex = Util::AsReal(fb.texture);
			subrectController.DrawEditor(Util::AsReal(fb.SRV), tex, 0.5f, 0.0f, Util::Subrect::OpaquePreviewBlendCallback);
		} else {
			subrectController.DrawEditor(nullptr, nullptr, 0.5f);
		}

		if (subrectController.IsDragging())
			lastDragTime = std::chrono::steady_clock::now();
	}
}

#undef I18N_KEY_PREFIX
