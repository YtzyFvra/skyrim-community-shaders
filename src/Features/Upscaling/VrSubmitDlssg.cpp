#include "VrSubmitDlssg.h"

#include "../../Globals.h"
#include "../../State.h"

#include <cstring>
#include <d3d11.h>

namespace
{
	std::uint32_t ownedFrame = UINT32_MAX;
	bool ownedSubmitted = false;
	bool loggedSubmit = false;
	bool loggedSwallow = false;

	ID3D11Resource* Rt(RE::RENDER_TARGETS::RENDER_TARGET slot)
	{
		auto* renderer = globals::game::renderer;
		if (!renderer)
			return nullptr;
		return renderer->GetRuntimeData().renderTargets[slot].texture;
	}

	bool IsOverlayRt(ID3D11Resource* handle)
	{
		if (!handle)
			return false;
#ifdef ENABLE_SKYRIM_VR
		return handle == Rt(RE::RENDER_TARGETS::kHUDMENU) ||
			handle == Rt(RE::RENDER_TARGETS::kPROJECTEDMENU) ||
			handle == Rt(RE::RENDER_TARGETS::kFADERUI) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI0) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI1) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI2) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI3) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI4) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI5) ||
			handle == Rt(RE::RENDER_TARGETS::kWORLDUI6);
#else
		return false;
#endif
	}

#ifdef ENABLE_SKYRIM_VR
	thread_local bool g_inOwnedSubmit = false;

	const char* RtName(ID3D11Resource* handle)
	{
		if (!handle)
			return "null";
		struct Named
		{
			RE::RENDER_TARGETS::RENDER_TARGET slot;
			const char* name;
		};
		const Named named[]{
			{ RE::RENDER_TARGETS::kFRAMEBUFFER, "kFRAMEBUFFER" },
			{ RE::RENDER_TARGETS::kMENUBG, "kMENUBG" },
			{ RE::RENDER_TARGETS::kHUDMENU, "kHUDMENU" },
			{ RE::RENDER_TARGETS::kPROJECTEDMENU, "kPROJECTEDMENU" },
			{ RE::RENDER_TARGETS::kFADERUI, "kFADERUI" },
			{ RE::RENDER_TARGETS::kTOTAL, "kTOTAL" },
			{ RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY, "kTEMP" },
		};
		for (const auto& entry : named) {
			if (handle == Rt(entry.slot))
				return entry.name;
		}
		return "other";
	}

	void DescribeKnownRt(ID3D11Resource* handle, UINT& width, UINT& height)
	{
		width = 0;
		height = 0;
		if (!handle || std::strcmp(RtName(handle), "other") == 0)
			return;
		auto* tex = static_cast<ID3D11Texture2D*>(handle);
		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc(&desc);
		width = desc.Width;
		height = desc.Height;
	}

	void LogEngineSubmit(ID3D11Resource* handle, bool overlay, bool swallow)
	{
		static std::uint32_t n = 0;
		if (n >= 16)
			return;
		++n;
		UINT width = 0;
		UINT height = 0;
		DescribeKnownRt(handle, width, height);
		logger::info("[VrSubmitDlssg] engine Submit #{} {} {}x{} overlay={} swallow={} handle={}",
			n, RtName(handle), width, height, overlay, swallow, static_cast<void*>(handle));
	}
#endif
}

void VrSubmitDlssg::Install()
{
}

void VrSubmitDlssg::NoteOwnedFrame(std::uint32_t frame)
{
	ownedFrame = frame;
	if (frame == UINT32_MAX)
		ownedSubmitted = false;
}

#ifdef ENABLE_SKYRIM_VR
bool VrSubmitDlssg::SubmitOwnedSbs(ID3D11Texture2D* sbs, std::uint32_t width, std::uint32_t height)
{
	if (!globals::game::isVR || !sbs || !width || !height)
		return false;
	auto* openVR = RE::BSOpenVR::GetSingleton();
	if (!openVR)
		return false;

	// Virtual 0x03 so later VDXR hooks still see it. That vfunc already submits both eyes.
	// The engine's parameter is the ID3D11Texture2D itself, not a vr::Texture_t: every engine
	// call observed carries a d3d11.dll vtable at *param. The header's type is a misdeclaration;
	// passing a real vr::Texture_t makes the runtime call through our struct and execute 0x0.
	g_inOwnedSubmit = true;
	const auto err = openVR->Submit(reinterpret_cast<const vr::Texture_t*>(sbs));
	g_inOwnedSubmit = false;
	if (err != vr::VRCompositorError_None) {
		logger::warn("[VrSubmitDlssg] BSOpenVR Submit failed err={} {}x{}",
			static_cast<int>(err), width, height);
		return false;
	}

	ownedSubmitted = true;
	if (!loggedSubmit) {
		logger::info("[VrSubmitDlssg] Owned SBS via BSOpenVR::Submit {}x{}", width, height);
		loggedSubmit = true;
	}
	return true;
}

bool VrSubmitDlssg::TrySubmit(RE::BSOpenVR*, const vr::Texture_t* texture,
	const vr::VRTextureBounds_t*, vr::EVRSubmitFlags,
	const REL::Relocation<SubmitFn>&, vr::EVRCompositorError& outError)
{
	outError = vr::VRCompositorError_None;
	if (g_inOwnedSubmit || !ownedSubmitted)
		return false;

	// See SubmitOwnedSbs: the engine hands the ID3D11Texture2D pointer itself in this slot.
	ID3D11Resource* handle = nullptr;
	const auto textureAddr = reinterpret_cast<std::uintptr_t>(texture);
	if (texture && textureAddr >= 0x10000 && textureAddr <= 0x00007FFFFFFFFFFFull)
		handle = reinterpret_cast<ID3D11Resource*>(const_cast<vr::Texture_t*>(texture));
	const bool overlay = IsOverlayRt(handle);
	if (overlay) {
		LogEngineSubmit(handle, true, false);
		return false;
	}

	LogEngineSubmit(handle, false, true);
	if (!loggedSwallow) {
		logger::info("[VrSubmitDlssg] Swallowing engine world Submit after owned SBS");
		loggedSwallow = true;
	}
	return true;
}
#endif
