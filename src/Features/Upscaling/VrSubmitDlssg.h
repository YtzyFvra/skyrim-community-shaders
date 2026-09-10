#pragma once

#include <cstdint>
#include <d3d11.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#endif

namespace VrSubmitDlssg
{
	void Install();

	/** @brief This frame already submitted both eyes; swallow engine world Submit. */
	void NoteOwnedFrame(std::uint32_t frame);

#ifdef ENABLE_SKYRIM_VR
	using SubmitFn = vr::EVRCompositorError(RE::BSOpenVR*, const vr::Texture_t*,
		const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

	/** @brief One eye compositor Submit. Does not Present; per-eye DLSS-G needs its own chains. */
	bool SubmitOwnedEye(std::uint32_t eye, ID3D11Resource* color,
		std::uint32_t width, std::uint32_t height);

	/** @brief True if this engine Submit was swallowed. */
	bool TrySubmit(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
		const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
		const REL::Relocation<SubmitFn>& original, vr::EVRCompositorError& outError);
#endif
}
