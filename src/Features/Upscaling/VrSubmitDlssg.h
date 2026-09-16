#pragma once

#include <cstdint>
#include <d3d11.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#endif

namespace VrSubmitDlssg
{
	void Install();
	void ClearShaderCache();

	/** @brief Clears owned Submit state (UINT32_MAX = nothing owned this frame); frame id is for logs. */
	void NoteOwnedFrame(std::uint32_t frame);

#ifdef ENABLE_SKYRIM_VR
	using SubmitFn = vr::EVRCompositorError(RE::BSOpenVR*, const vr::Texture_t*,
		const vr::VRTextureBounds_t*, vr::EVRSubmitFlags);

	/**
	 * @brief One BSOpenVR::Submit (vfunc 0x03 already does both eyes) of an SBS texture, then the
	 * engine's own world Submit for this frame is swallowed. VDXR counts on this path.
	 */
	bool SubmitOwnedSbs(ID3D11Texture2D* sbs, std::uint32_t width, std::uint32_t height);

	/**
	 * @brief After form-1 kTEMP writeback: 0 no-op; 1–3 prior tests; 4 retired.
	 * No-op while VR ARISE Tail owns the frame loop.
	 */
	void RunSubmitTest(ID3D11Texture2D* kTemp, std::uint32_t mode);

	/** @brief True if engine Submit should be swallowed. Overlay RTs are left alone unless ARISE. */
	bool TrySubmit(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
		const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
		const REL::Relocation<SubmitFn>& original, vr::EVRCompositorError& outError);
#endif
}
