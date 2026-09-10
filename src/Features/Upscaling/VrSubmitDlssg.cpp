#include "VrSubmitDlssg.h"

#include "../../Globals.h"
#include "../../State.h"

namespace
{
	std::uint32_t ownedFrame = UINT32_MAX;
	bool logged = false;

	bool IsEngineWorldTexture(ID3D11Resource* resource)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer || !resource)
			return false;
		auto& targets = renderer->GetRuntimeData().renderTargets;
		if (resource == targets[RE::RENDER_TARGETS::kTOTAL].texture)
			return true;
#ifdef ENABLE_SKYRIM_VR
		return resource == targets[RE::RENDER_TARGETS::kVR_FRAMEBUFFER].texture;
#else
		return false;
#endif
	}
}

void VrSubmitDlssg::Install()
{
}

void VrSubmitDlssg::NoteOwnedFrame(std::uint32_t frame)
{
	ownedFrame = frame;
}

#ifdef ENABLE_SKYRIM_VR
bool VrSubmitDlssg::SubmitOwnedEye(std::uint32_t eye, ID3D11Resource* color,
	std::uint32_t width, std::uint32_t height)
{
	if (!globals::game::isVR || !color || !width || !height)
		return false;
	auto* compositor = RE::BSOpenVR::GetIVRCompositor();
	if (!compositor)
		return false;

	vr::Texture_t texture{};
	texture.handle = color;
	texture.eType = vr::TextureType_DirectX;
	texture.eColorSpace = vr::ColorSpace_Auto;
	vr::VRTextureBounds_t bounds{ 0.0f, 1.0f, 1.0f, 0.0f };
	const vr::EVREye vrEye = eye == 0 ? vr::Eye_Left : vr::Eye_Right;
	const auto error = compositor->Submit(vrEye, &texture, &bounds, vr::Submit_Default);
	if (error != vr::VRCompositorError_None) {
		logger::warn("[VrSubmitDlssg] Compositor Submit eye={} error={}", eye, static_cast<int>(error));
		return false;
	}
	if (!logged) {
		logger::info("[VrSubmitDlssg] Owned per-eye Submit; engine world Submit is swallowed");
		logged = true;
	}
	return true;
}

bool VrSubmitDlssg::TrySubmit(RE::BSOpenVR* openVR, const vr::Texture_t* texture,
	const vr::VRTextureBounds_t* bounds, vr::EVRSubmitFlags flags,
	const REL::Relocation<SubmitFn>& original, vr::EVRCompositorError& outError)
{
	outError = vr::VRCompositorError_None;
	if (!globals::game::isVR || !texture || !texture->handle)
		return false;
	const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
	if (ownedFrame == frame && IsEngineWorldTexture(static_cast<ID3D11Resource*>(texture->handle)))
		return true;
	(void)openVR;
	(void)bounds;
	(void)flags;
	(void)original;
	return false;
}
#endif
