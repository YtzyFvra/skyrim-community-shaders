#include "Bridge.h"

#include "../../../Globals.h"
#include "../../DLSSBooster.h"
#include "../../Upscaling.h"
#include "../FoveatedRender.h"

bool FoveatedRenderImpl::Bridge::IsRouteActive()
{
	// IsActive() already checks: enabledAtBoot && isVR
	//                            && GetUpscaleMethod() is kDLSS or kFSR (selected, not just available)
	return globals::features::upscaling.foveatedRender.IsActive();
}

void FoveatedRenderImpl::Bridge::BootSequence()
{
	auto& enhancer = globals::features::upscaling.foveatedRender;
	enhancer.LatchEnabled();
	enhancer.LatchQualityMode();
}

void FoveatedRenderImpl::Bridge::ComputeMvecScale(uint32_t eyeIndex, float& outX, float& outY)
{
	// Default: identity (caller's normal Streamline path).
	outX = 1.0f;
	outY = 1.0f;

	if (!IsRouteActive() && !foveatedEvaluating)
		return;

	auto& enhancer = globals::features::upscaling.foveatedRender;
	// DB's VR box wins over Upscaling while the override is on. Eyes can differ (nasal).
	const Util::Subrect::UVRegion* dbLeft = nullptr;
	const Util::Subrect::UVRegion* dbRight = nullptr;
	const bool dbCrop = globals::features::dlssBooster.TryGetVrSubrect(dbLeft, dbRight);
	const auto& uv = dbCrop ? (eyeIndex == 1 ? *dbRight : *dbLeft) :
		((eyeIndex == 1) ? enhancer.subrectController.GetRightEyeUV() : enhancer.subrectController.GetUV());
	const bool isFullEye = uv.IsFullEye();

	if (isFullEye)
		return;

	// Default + Faster both use per-eye DLSS calls (not strip-merged), so
	// motion vectors scale by 1/UV.w on x.
	outX = (uv.w > 0.0f) ? (1.0f / uv.w) : 1.0f;
	outY = (uv.h > 0.0f) ? (1.0f / uv.h) : 1.0f;
}
