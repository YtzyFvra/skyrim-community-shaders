#pragma once

// ============================================================================
// FoveatedRender — VR DLSS enhancement mode of Upscaling
// ============================================================================
//
// Foveated subrect-DLSS path: only the user-selected region gets full DLSS
// upscaling; the periphery is cheaply stretched via SubrectStretchCS. Halves
// (or more) the DLSS workload. Composes with VRS, Screenshot, and the lossless
// recording feature through the shared Util::Subrect module — use the same
// preset for consistent results across them.
//
// Architecturally a mode inside Upscaling (mirroring DLSSperf): a static-
// inline member, not a peer Feature. Settings that overlap with Upscaling's
// (quality mode, sharpness, DLSS preset, Streamline log level) read directly
// from `globals::features::upscaling.settings` rather than being duplicated.
// VR only -- flat has no lens-driven periphery quality cliff to exploit, so
// the perf/quality tradeoff doesn't carry over. Supports both DLSS and FSR3
// (host path); under FSR, only DlssMode::kDefault is available -- kFaster
// relies on Streamline's SBS-subrect read, which has no FSR equivalent.
//
// ============================================================================

#include "../../Utils/BootSnapshot.h"
#include "../../Utils/Subrect.h"

#include <atomic>
#include <chrono>

struct FoveatedRender
{
	enum class FlatAct1Backend : uint { kBypass, kDLSS, kFSR3 };
	enum class FlatAct2Backend : uint { kDLSS, kFSR3, kNIS, kFusion, kLanczos };
	enum class FlatAct2SrBackend : uint { kDLSS, kFSR3, kNIS, kLanczos };
	enum class ResampleFilter : uint { kLanczos, kStandard, kFootprint };

	struct Extent
	{
		uint32_t width = 0;
		uint32_t height = 0;
	};

	struct UnifiedPipelineSettings
	{
		bool act1Srss = true;
		uint act1SrssQuality = 4;
		uint act1SrssPreset = 4;
		float guidanceScale = 1.23f;
		uint act1Downsample = static_cast<uint>(ResampleFilter::kLanczos);
		uint act2SrBackend = static_cast<uint>(FlatAct2SrBackend::kLanczos);
		uint act2Upsample = static_cast<uint>(ResampleFilter::kLanczos);
		bool fusionEnabled = true;
	};

	inline static constexpr Util::Settings::RestartTable<UnifiedPipelineSettings, 4> kUnifiedRestartFields{ {
		UTIL_RESTART_FIELD(UnifiedPipelineSettings, act1Srss, "SRSS"),
		UTIL_RESTART_FIELD(UnifiedPipelineSettings, act1SrssQuality, "SRSS Mode"),
		UTIL_RESTART_FIELD(UnifiedPipelineSettings, act1SrssPreset, "SRSS Preset"),
		UTIL_RESTART_FIELD(UnifiedPipelineSettings, guidanceScale, "Guidance Scale"),
	} };

	struct FlatPipelinePlan
	{
		Extent renderR{};
		Extent guidanceG{};
		Extent neuralN{};
		Extent displayD{};
		uint32_t displayWidth = 0;
		uint32_t displayHeight = 0;
		uint32_t engineWidth = 0;
		uint32_t engineHeight = 0;
		uint32_t act1OutputWidth = 0;
		uint32_t act1OutputHeight = 0;
		uint32_t nrWidth = 0;
		uint32_t nrHeight = 0;
		uint32_t nrOutputWidth = 0;
		uint32_t nrOutputHeight = 0;
		uint32_t act2OutputWidth = 0;
		uint32_t act2OutputHeight = 0;
		FlatAct1Backend act1Backend = FlatAct1Backend::kBypass;
		FlatAct2Backend act2Backend = FlatAct2Backend::kNIS;
		FlatAct2SrBackend act2SrBackend = FlatAct2SrBackend::kNIS;
		ResampleFilter act1Downsample = ResampleFilter::kLanczos;
		ResampleFilter act2Upsample = ResampleFilter::kLanczos;
		uint32_t act1Quality = 1;
		uint32_t act1Preset = 0;
		uint32_t act2Quality = 1;
		bool act1Dsr = false;
		bool srss = false;
		bool fusion = false;
		bool sharpen = false;
	};
	// DLSS execution mode for VR
	enum class DlssMode : uint
	{
		kDefault = 0,  // Per-eye isolation: 2 extra resource sets, 2 evaluates. Supports F/J/K/L/M.
		kFaster = 1,   // SBS viewport: tell SL to read subrect from SBS directly, no extra resources, 2 evaluates. J/K incompatible, only L/M/F.
	};

	// Subrect blend mode when writing DLSS output back over stretched background
	enum class SubrectBlendMode : uint
	{
		kHardCopy = 0,  // CopySubresourceRegion (no blending — sharp edge)
		kFeather = 1,   // smoothstep alpha ramp over N pixels
		kDither = 2,    // Blue-noise binary threshold in feather band
	};

	// Composite mask shape only; the upscaler's input and output stay rectangular.
	enum class SubrectMaskMode : uint
	{
		kRectangle = 0,  // Preserve the rectangular edge behavior
		kOval = 1,       // Distance-corrected elliptical transition
	};

	// Periphery AA algorithm applied after background stretch
	enum class PeripheryAAMode : uint
	{
		kNone = 0,            // No dedicated periphery AA
		kTemporalSmooth = 1,  // Motion-compensated temporal accumulation (anti-flicker)
	};

	// Stretch algorithm for DRS → full-eye background (used by SubrectStretchCS shader)
	enum class StretchMode : uint
	{
		kBilinear = 0,      // Default bilinear sampling (clean upscale)
		kPoint = 1,         // Nearest-neighbor / point (cheapest, VRS-like broadcast)
		kGaussianBlur = 2,  // 3x3 Gaussian blur (soft periphery)
	};

	/** @brief Translated display names for the enums above -- single source shared by
	 *  DrawSettings' dropdowns and Upscaling::GetProfilePreviewText. */
	static const char* DlssModeName(DlssMode mode);
	static const char* StretchModeName(StretchMode mode);
	static const char* PeripheryAAModeName(PeripheryAAMode mode);
	static const char* SubrectBlendModeName(SubrectBlendMode mode);
	static const char* SubrectMaskModeName(SubrectMaskMode mode);

	// FoveatedRender-specific settings. Quality mode / sharpness / DLSS preset /
	// Streamline log level live on Upscaling::Settings and are read through
	// the accessors below — do not duplicate them here. Sharpening on/off is
	// controlled by the shared sharpnessDLSS slider (0 disables RCAS).
	//
	// Do not add UI sliders for MV-dilation / reactive-mask / transparency-mask
	// toggles until the shader permutations are plumbed: EncodeTexturesCS needs
	// per-toggle defines, the encode pass needs a conditional skip when all are
	// off, and EvaluateDLSS needs per-toggle arg gating. Knobs without wiring
	// are silent no-ops that mislead users.
	struct Settings
	{
		uint enabled = 0;  // opt-in: requires restart to take effect via LatchEnabled()
		uint dlssMode = (uint)DlssMode::kDefault;
		uint stretchMode = (uint)StretchMode::kGaussianBlur;
		float peripheryBlurRadius = 1.0f;
		uint debugVisualize = 0;  // tint cheap-stretched periphery red; runtime toggle
		uint peripheryAAMode = static_cast<uint>(PeripheryAAMode::kTemporalSmooth);
		float peripheryTemporalAlpha = 0.16f;
		uint subrectBlendMode = static_cast<uint>(SubrectBlendMode::kHardCopy);
		uint subrectMaskMode = static_cast<uint>(SubrectMaskMode::kRectangle);
		float subrectFeatherWidth = 64.0f;
		float subrectFalloffCurve = 1.0f;
		float subrectDitherStrength = 1.0f;
		bool neuralRenderingEnabled = true;
		bool ariseShadowScheduler = false;
		bool ariseVisualValidation = false;
		bool ariseWorkerNr = false;
		bool ariseModel3 = false;
		bool singleThreadPerfMode = true;
		uint ariseBufferCount = 3;
		uint flatPipelineAct1Backend = static_cast<uint>(FlatAct1Backend::kDLSS);
		uint flatPipelineAct1Quality = 1;
		uint flatPipelineAct1Preset = 0;
		uint flatPipelineAct1Jitter = 0;
		bool flatPipelineAct1Dsr = false;
		float flatPipelineNrScale = 0.62f;
		uint flatPipelineAct2Backend = static_cast<uint>(FlatAct2Backend::kLanczos);
		UnifiedPipelineSettings unifiedPipeline{};
		uint flatPipelineAct2Preset = 0;
		uint flatPipelineAct2Jitter = 1;
		uint flatPipelineFusionDirection = 1;
		uint flatPipelineFusionDenoise = 1;
		float flatPipelineFusionDenoiseStrength = 0.40f;
		float flatPipelineFusionSoftness = 0.20f;
		bool flatPipelineFusionEnhancedGuidance = true;
		float flatPipelineFusionFrequencyGuidance = 0.45f;
		float flatPipelineFusionDepthGuidance = 0.65f;
		float flatPipelineFusionColorGuidance = 0.70f;
		bool flatPipelineSharpen = true;
		float flatPipelineSharpness = 1.0f;
		bool flatAct1Enabled = true;
		uint flatAct1QualityMode = 1;
		uint flatAct1Preset = 0;
		float flatAct1Sharpness = 0.0f;
		uint flatAct2QualityMode = 1;
		uint flatAct2Preset = 0;
		float flatAct2Sharpness = 0.0f;
		uint neuralRenderingPreset = 0;
		float neuralRenderingIntensity = 2.0f;
		float neuralRenderingLocalTone = 2.0f;
		float neuralRenderingLocalStructure = 2.0f;
		float neuralRenderingSkinStructure = 2.0f;
		uint neuralRenderingStyle = 0;
		bool neuralRenderingAutoMask = true;
		bool neuralRenderingUICorrection = false;
		bool neuralRenderingBypassOutput = false;
		bool neuralRenderingResetEveryFrame = false;
		uint neuralRenderingInputPrefilter = 0;
		uint neuralRenderingMotionVectorMode = 0;
		bool neuralRenderingHalfResolution = true;
		uint neuralRenderingWorkingScale = 0;
		uint neuralRenderingOutputMode = 0;
		float neuralRenderingNisSharpness = 0.35f;
		uint neuralRenderingFusionPreset = 4;
		float neuralRenderingFusionLuma = 0.80f;
		float neuralRenderingFusionChroma = 0.45f;
		float neuralRenderingFusionEdgeProtection = 0.30f;
		float neuralRenderingFusionResidualLimit = 0.14f;
	};

	inline static constexpr Util::Settings::RestartTable<Settings, 5> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, enabled, "Foveated DLSS"),
		UTIL_RESTART_FIELD(Settings, ariseModel3, "ARISE Model 3"),
		UTIL_RESTART_FIELD(Settings, singleThreadPerfMode, "Single-thread PerfMode"),
		UTIL_RESTART_FIELD(Settings, flatPipelineAct1Backend, "Logical R Backend"),
		UTIL_RESTART_FIELD(Settings, flatPipelineAct1Quality, "Logical R Preset"),
	} };
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };
	Util::Settings::BootSnapshot<UnifiedPipelineSettings> unifiedBootSnapshot{ kUnifiedRestartFields };

	/** @brief Region-preset display names, shared by PostPostLoad's seed list, the
	 *  top-level preset buttons, and Upscaling::ApplyPerformanceProfile. */
	static constexpr const char* kPresetFullEye = "Full Eye";                          ///< No crop; full-eye DLSS coverage.
	static constexpr const char* kPresetCenter75 = "Center 75%";                       ///< Centered crop covering 75% of the eye.
	static constexpr const char* kPresetCenter50 = "Center 50%";                       ///< Centered crop covering 50% of the eye.
	static constexpr const char* kPresetNasalConvergence50 = "Nasal Convergence 50%";  ///< 50% crop biased toward nasal convergence.
	static constexpr const char* kPresetNasalConvergence60 = "Nasal Convergence 60%";  ///< 60% crop biased toward nasal convergence.
	static constexpr const char* kPresetNasalConvergence70 = "Nasal Convergence 70%";  ///< 70% crop biased toward nasal convergence.

	Settings settings;
	std::atomic_bool ariseDimensionDiagnostics = false;
	std::atomic_bool ariseDisableUiComposite = false;
	std::atomic_bool ariseForceSynchronous = false;
	std::atomic_bool ariseBypassGraph = false;
	std::atomic_bool menuCopyDiagnostics = false;
	/** @brief One-line menu compositor dump. No-op unless menuCopyDiagnostics is on. */
	void LogMenuPipeline(const char* stage) const;
	/** @brief Title/loading only: blit kMENUBG onto SwapChainBuffer at dest format. */
	void BlitTitleBackgroundToSwapChain() const;
	Util::Subrect::Controller subrectController;

	// Called from Upscaling::DrawSettings. DrawEnable renders the always-visible
	// header + Enable checkbox at the parent's top level; DrawSettings renders
	// the body knobs inside a collapsible TreeNode (Upscaling wraps it in
	// BeginDisabled when settings.enabled == 0).
	void DrawEnable();
	void DrawSettings();
	void DrawNeuralRenderingSettings(bool wrapHeader = true);
	/** @brief Origin / Single-thread / ARISE radios. Restart-gated; Flat only. */
	void DrawLogicalPerfModeControls();
	FlatPipelinePlan BuildFlatPipelinePlan(uint32_t displayWidth, uint32_t displayHeight,
		uint32_t engineWidth = 0, uint32_t engineHeight = 0) const;
	// Called from Upscaling::SaveSettings / LoadSettings to round-trip JSON.
	void SaveSettings(json& o_json);
	void LoadSettings(const json& o_json);
	void RestoreDefaultSettings();
	void ClearShaderCache();
	[[nodiscard]] bool WantsLogicalPerfMode() const { return settings.ariseModel3 || settings.singleThreadPerfMode; }
	[[nodiscard]] bool BootUsesLogicalPerfMode() const
	{
		return bootSnapshot.Boot(&Settings::ariseModel3) || bootSnapshot.Boot(&Settings::singleThreadPerfMode);
	}
	[[nodiscard]] UnifiedPipelineSettings ActiveUnifiedPipelineSettings() const
	{
		auto active = settings.unifiedPipeline;
		if (BootUsesLogicalPerfMode() && unifiedBootSnapshot.IsLatched()) {
			active.act1Srss = unifiedBootSnapshot.Boot(&UnifiedPipelineSettings::act1Srss);
			active.act1SrssQuality = unifiedBootSnapshot.Boot(&UnifiedPipelineSettings::act1SrssQuality);
			active.act1SrssPreset = unifiedBootSnapshot.Boot(&UnifiedPipelineSettings::act1SrssPreset);
			active.guidanceScale = unifiedBootSnapshot.Boot(&UnifiedPipelineSettings::guidanceScale);
		}
		return active;
	}
	// Called from Upscaling::PostPostLoad to seed subrect presets.
	void PostPostLoad();
	void InstallD3DHook(ID3D11DeviceContext* context);

	struct UICompositeRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct MapRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct WorldMapRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct WorldMapNoSkyBlurRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct HDRTonemapRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct HDRTonemapFadeRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct RendererUpdateViewPortHook
	{
		static void thunk(RE::BSGraphics::Renderer* renderer, std::uint32_t width, std::uint32_t height, bool disableScale);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct DistantBlurRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct DistantBlurFoggedRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct DistantBlurMaskedFoggedRenderHook
	{
		static void thunk(void* imageSpaceShader, RE::BSTriShape* shape, RE::ImageSpaceEffectParam* param);
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct ID3D11DeviceContext_RSSetViewportsHook
	{
		static void thunk(ID3D11DeviceContext* context, UINT viewportCount, const D3D11_VIEWPORT* viewports);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	bool IsRuntimeSupported() const;
	bool IsActive() const;
	bool IsLoaded() const { return enabledAtBoot; }

	/** @brief True while drag-resizing the crop region, and for a few seconds after.
	 *  Read by the stretch pass alongside settings.debugVisualize. */
	bool ShouldForceVisualize() const;

	// Foveation region for per-pixel foveated effects (e.g. SSR): the rectangular DLSS subrect mapped
	// to centered-superellipse params. available is false when foveation is inactive or full-eye.
	struct FoveationProfile
	{
		bool available = false;
		float coverageScale = 1.0f;          // linear center coverage scale [0.25, 1.0]
		float centerHorizontalScale = 1.0f;  // [1.0, 2.0]
		float2 centerOffsets[2] = {};        // [0]=left eye, [1]=right eye
	};
	FoveationProfile GetFoveationProfile() const;

	// Main enable: latched at boot, change requires restart
	void LatchEnabled() { enabledAtBoot = (settings.enabled != 0); }

	// Quality mode reads through Upscaling::Settings — latch the boot value so
	// downstream RT allocations stay coherent if the user moves the slider.
	void LatchQualityMode();
	uint GetQualityModeAtBoot() const { return qualityModeAtBoot; }

	/// Render-to-display scale denominator for a quality mode index
	/// (1=Quality .. 4=UltraPerformance). Delegates to the FFX SDK ratio table.
	static float GetRenderScaleForQuality(uint qualityMode);

	// Faster mode relies on Streamline's SBS-subrect read, which has no FSR
	// equivalent -- forced to kDefault whenever FSR is the selected method.
	DlssMode GetDlssMode() const;
	StretchMode GetStretchMode() const { return (StretchMode)std::min(settings.stretchMode, 2u); }
	PeripheryAAMode GetPeripheryAAMode() const { return static_cast<PeripheryAAMode>(std::min(settings.peripheryAAMode, 1u)); }
	SubrectBlendMode GetSubrectBlendMode() const { return static_cast<SubrectBlendMode>(std::min(settings.subrectBlendMode, 2u)); }
	SubrectMaskMode GetSubrectMaskMode() const { return static_cast<SubrectMaskMode>(std::min(settings.subrectMaskMode, 1u)); }

	// Active getters: clamp + route shared fields through Upscaling::Settings.
	uint GetActiveQualityMode() const;
	uint GetActivePresetDLSS() const;
	float GetActiveSharpnessDLSS() const;

	// Re-clamp cross-feature settings (preset vs DLSS mode). Idempotent; safe to call
	// from Upscaling::LoadSettings after JSON has overwritten shared fields.
	void ClampSettings();

private:
	bool enabledAtBoot = false;                            // latched from settings.enabled at boot
	uint qualityModeAtBoot = 4;                            // latched from Upscaling::Settings::qualityMode at boot
	std::chrono::steady_clock::time_point lastDragTime{};  // epoch -- no force-visualize at boot

	bool IsPresetCompatibleWithMode(uint presetIndex) const;
	void ClampPresetToMode();
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	FoveatedRender::UnifiedPipelineSettings,
	act1Srss,
	act1SrssQuality,
	act1SrssPreset,
	guidanceScale,
	act1Downsample,
	act2SrBackend,
	act2Upsample,
	fusionEnabled);

// Settings has 65 fields; nlohmann's NON_INTRUSIVE macros cap at 64.
void to_json(nlohmann::json& j, const FoveatedRender::Settings& s);
void from_json(const nlohmann::json& j, FoveatedRender::Settings& s);
