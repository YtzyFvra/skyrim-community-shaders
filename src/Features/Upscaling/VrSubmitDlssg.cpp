#include "VrSubmitDlssg.h"

#include "../../Globals.h"
#include "NeuralRendering/ArisePipeline.h"
#include "VrAriseTail.h"
#include <Windows.h>
#include <atomic>
#include <d3d11.h>

namespace
{
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
		return handle == Rt(RE::RENDER_TARGETS::kMENUBG) ||
			handle == Rt(RE::RENDER_TARGETS::kHUDMENU) ||
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
#endif
}

void VrSubmitDlssg::Install()
{
#ifdef ENABLE_SKYRIM_VR
	if (globals::game::isVR)
		NeuralRendering::ARISE::Pipeline::Instance().Install();
#endif
	VrAriseTail::Install();
}

void VrSubmitDlssg::NoteOwnedFrame(std::uint32_t frame)
{
	if (frame == UINT32_MAX)
		ownedSubmitted = false;
}

#ifdef ENABLE_SKYRIM_VR
bool VrSubmitDlssg::SubmitOwnedSbs(ID3D11Texture2D* sbs, std::uint32_t width, std::uint32_t height)
{
	if (!globals::game::isVR || !sbs || !width || !height || VrAriseTail::IsActive())
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
	if (g_inOwnedSubmit)
		return false;

	ID3D11Resource* handle = nullptr;
	const auto textureAddr = reinterpret_cast<std::uintptr_t>(texture);
	if (texture && textureAddr >= 0x10000 && textureAddr <= 0x00007FFFFFFFFFFFull)
		handle = reinterpret_cast<ID3D11Resource*>(const_cast<vr::Texture_t*>(texture));
	const bool isOverlay = handle && IsOverlayRt(handle);
	if (handle && !isOverlay && VrAriseTail::IsActive())
		VrAriseTail::PublishKTemp(static_cast<ID3D11Texture2D*>(handle));

	// Overlay submits are swallowed. Headset HUD is the kMENUBG copy stamped after FSR.
	if (isOverlay) {
		static std::atomic<std::uint32_t> overlaySwallowed{ 0 };
		const std::uint32_t n = ++overlaySwallowed;
		if (n <= 16 || (n % 120) == 0)
			logger::warn("[VrSubmitDlssg] overlay Submit #{} {} swallowed (HUD stamped after FG)",
				n, RtName(handle));
	}

	if (VrAriseTail::ShouldSwallowEngineSubmit())
		return true;
	if (!ownedSubmitted)
		return false;

	if (!loggedSwallow) {
		logger::info("[VrSubmitDlssg] Swallowing engine world Submit after owned SBS");
		loggedSwallow = true;
	}
	return true;
}
#endif
