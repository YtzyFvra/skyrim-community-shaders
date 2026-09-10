#pragma once

#include <cstdint>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#endif

namespace VrSubmitDlssg
{
	void Install();
	bool IsReady();

#ifdef ENABLE_SKYRIM_VR
	using SubmitFn = vr::EVRCompositorError(RE::BSOpenVR*, const vr::Texture_t*,
		const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

	/** @brief Owns Submit when VR single-thread DLSS-G is on. Returns true if it called Submit. */
	bool TrySubmit(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
		const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
		const REL::Relocation<SubmitFn>& original, vr::EVRCompositorError& outError);
#endif
}
