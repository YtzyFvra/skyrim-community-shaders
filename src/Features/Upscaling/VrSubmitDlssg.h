#pragma once

#include <cstdint>
#include <d3d11.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#endif

namespace VrSubmitDlssg
{
	void Install();

	/** @brief Clears owned Submit state on Reset; frame id is for logs. */
	void NoteOwnedFrame(std::uint32_t frame);

	/** @brief True after a successful owned compositor Submit this frame. */
	bool HasPendingOwnedSbs();

	/** @brief Swallow engine world Submit once the pump owns the frame. */
	void ArmWorldSwallow();

#ifdef ENABLE_SKYRIM_VR
	using SubmitFn = vr::EVRCompositorError(RE::BSOpenVR*, const vr::Texture_t*,
		const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

	/** @brief One BSOpenVR::Submit (vfunc 0x03 already does both eyes). VDXR sits on this path. */
	bool SubmitOwnedSbs(ID3D11Texture2D* sbs, std::uint32_t width, std::uint32_t height);

	bool SubmitOwnedEye(std::uint32_t eye, ID3D11Resource* color,
		std::uint32_t width, std::uint32_t height);

	/** @brief True if engine world Submit should be swallowed. Overlay RTs are left alone. */
	bool TrySubmit(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
		const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
		const REL::Relocation<SubmitFn>& original, vr::EVRCompositorError& outError);
#endif
}
