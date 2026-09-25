#pragma once

#include "Feature.h"
#include "Upscaling/FoveatedRender.h"
#include "Utils/BootSnapshot.h"
#include "Utils/Subrect.h"

/**
 * @brief Sibling Display tab that owns the RTX reconstruction settings blob.
 *
 * Runtime still executes through Upscaling. This feature persists its own JSON
 * and, when the override is on, pushes that blob over overlapping Upscaling
 * fields so a later official PR can fold the page back without mixing schemas.
 */
struct DLSSBooster : Feature
{
	virtual inline std::string GetName() override { return "RTX DLSS Booster"; }
	virtual std::string GetDisplayName() override { return T("feature.dlss_booster.name", "RTX DLSS Booster"); }
	virtual inline std::string GetShortName() override { return "DLSSBooster"; }
	virtual inline std::string_view GetCategory() const override { return FeatureCategories::kDisplay; }
	virtual inline bool SupportsVR() override { return true; }
	virtual inline bool IsCore() const override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.dlss_booster.description",
					 "DLSS Neural Reconstruction debug host. Depends on Upscaling."),
			{ T("feature.dlss_booster.key_feature_1", "Own settings file, separate from Upscaling"),
				T("feature.dlss_booster.key_feature_2", "Conservative default; RTX 5090 reference preset"),
				T("feature.dlss_booster.key_feature_3", "Per-pass backends; DLSS model is chosen on that pass") } };
	}

	struct Settings
	{
		bool overrideEnabled = true;
		uint reconstructionPreset = 0;  // 0=Conservative, 1=RTX 5090, 2=Custom
		FoveatedRender::Settings reconstruction{};
		uint frameGenerationMode = 1;
		uint frameGenerationForceEnable = 1;
		bool frameGenerationAllowInMenus = false;
		bool preferFSRFrameGen = false;
		uint dlssgFramesToGenerate = 1;
		uint frameLimitMode = 1;
		bool reflexLowLatencyMode = true;
		bool reflexLowLatencyBoost = true;
		bool reflexUseMarkersToOptimize = true;
		bool reflexUseFPSLimit = true;
		float reflexFPSLimit = 60.0f;
	};

	inline static constexpr Util::Settings::RestartTable<Settings, 4> kRestartFields{ {
		UTIL_RESTART_FIELD(Settings, overrideEnabled, "RTX DLSS Booster Override"),
		UTIL_RESTART_FIELD(Settings, frameGenerationMode, "Frame Generation"),
		UTIL_RESTART_FIELD(Settings, preferFSRFrameGen, "Prefer FSR Frame Generation"),
		UTIL_RESTART_FIELD(Settings, frameGenerationForceEnable, "Force Enable Frame Generation"),
	} };

	Settings settings;
	Util::Settings::BootSnapshot<Settings> bootSnapshot{ kRestartFields };

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	/**
	 * @brief VR crop owned by this page. When the override is on, the six-pass uses it
	 * instead of Upscaling's foveated subrect.
	 */
	bool TryGetVrSubrect(const Util::Subrect::UVRegion*& left, const Util::Subrect::UVRegion*& right);

	virtual std::span<const Util::Settings::RestartFieldInfo> GetRestartRequiredFields() const override
	{
		return { kRestartFields.data(), kRestartFields.size() };
	}
	const void* GetBootValue(std::string_view jsonKey) const override { return bootSnapshot.RawBoot(jsonKey); }
	const void* GetSettingsBlob() const override { return &settings; }
	size_t GetSettingsBlobSize() const override { return sizeof(settings); }

private:
	void DrawIntro();
	void DrawReconstructionPreset();
	void ImportFromRuntime();
	void PullFromRuntime();
	void PushToRuntime() const;
	void PrepareVrSubrect();

	Util::Subrect::Controller vrSubrect;
	bool vrSubrectSeeded = false;
	bool vrSubrectHasSavedCrop = false;
};
