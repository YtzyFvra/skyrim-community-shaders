#include "VrSubmitDlssg.h"

#include "../../Globals.h"
#include "../../State.h"
#include "../Upscaling.h"
#include "GpuPass.h"
#include "NeuralRendering/ArisePipeline.h"
#include "Utils/LazyShader.h"
#include "VrAriseTail.h"
#include "VrBoosterTail.h"

#include <Windows.h>
#include <atomic>
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
	Util::LazyShader<ID3D11ComputeShader> bandCS;
	eastl::unique_ptr<Texture2D> testSbsA;
	eastl::unique_ptr<Texture2D> testSbsB;
	eastl::unique_ptr<ConstantBuffer> bandCB;
	bool loggedMode2 = false;
	bool loggedMode4 = false;
	bool loggedPaintFail = false;
	winrt::com_ptr<ID3D11Query> gpuFence;

	struct BandCB
	{
		float color[4];
		std::uint32_t width;
		std::uint32_t height;
		std::uint32_t pad[2];
	};
	STATIC_ASSERT_ALIGNAS_16(BandCB);

	bool EnsureTestSbs(eastl::unique_ptr<Texture2D>& slot, ID3D11Texture2D* kTemp,
		const D3D11_TEXTURE2D_DESC& desc, const char* name)
	{
		if (slot && slot->resource) {
			const auto& have = slot->desc;
			if (have.Width == desc.Width && have.Height == desc.Height && have.Format == desc.Format)
				return slot->uav != nullptr;
		}
		slot = Upscaling::CreateTextureFromSource(kTemp, desc.Width, desc.Height, false, true, true, name);
		return slot && slot->resource && slot->uav;
	}

	bool PaintSolid(ID3D11DeviceContext* context, ID3D11UnorderedAccessView* uav,
		std::uint32_t width, std::uint32_t height, const float color[4])
	{
		auto* cs = bandCS.Get(L"Data/Shaders/Upscaling/VrSubmitTestBand.hlsl", {}, "cs_5_0", "main",
			"VrSubmitDlssg::BandCS");
		if (!cs || !uav || !width || !height) {
			if (!loggedPaintFail) {
				logger::warn("[VrSubmitDlssg] mode 2 solid fill CS missing");
				loggedPaintFail = true;
			}
			return false;
		}
		if (!bandCB)
			bandCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<BandCB>(), "VrSubmitDlssg::BandCB");

		BandCB cb{};
		cb.color[0] = color[0];
		cb.color[1] = color[1];
		cb.color[2] = color[2];
		cb.color[3] = color[3];
		cb.width = width;
		cb.height = height;
		bandCB->Update(cb);

		CS_GPU_PASS("VrSubmitDlssg::SolidFill");
		context->CSSetShader(cs, nullptr, 0);
		ID3D11Buffer* cbs[]{ bandCB->CB() };
		context->CSSetConstantBuffers(0, 1, cbs);
		ID3D11UnorderedAccessView* uavs[]{ uav };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		ID3D11UnorderedAccessView* nullUav[]{ nullptr };
		ID3D11Buffer* nullCb[]{ nullptr };
		context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
		context->CSSetConstantBuffers(0, 1, nullCb);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	void FlushAndFence(ID3D11DeviceContext* context)
	{
		auto* device = globals::d3d::device;
		if (!device || !context)
			return;
		if (!gpuFence) {
			D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
			if (FAILED(device->CreateQuery(&queryDesc, gpuFence.put())))
				return;
		}
		context->End(gpuFence.get());
		context->Flush();
		while (context->GetData(gpuFence.get(), nullptr, 0, 0) == S_FALSE)
			Sleep(0);
	}

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
#ifdef ENABLE_SKYRIM_VR
	if (globals::game::isVR)
		NeuralRendering::ARISE::Pipeline::Instance().Install();
#endif
	VrAriseTail::Install();
}

void VrSubmitDlssg::ClearShaderCache()
{
#ifdef ENABLE_SKYRIM_VR
	bandCS.Reset();
#endif
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

void VrSubmitDlssg::RunSubmitTest(ID3D11Texture2D* kTemp, std::uint32_t mode)
{
	if (!kTemp || mode == 0 || VrAriseTail::IsActive())
		return;
	D3D11_TEXTURE2D_DESC desc{};
	kTemp->GetDesc(&desc);
	if (mode == 1) {
		SubmitOwnedSbs(kTemp, desc.Width, desc.Height);
		return;
	}
	if (mode == 2) {
		auto* context = globals::d3d::context;
		if (!context ||
			!EnsureTestSbs(testSbsA, kTemp, desc, "VrSubmitDlssg::TestSbsA") ||
			!EnsureTestSbs(testSbsB, kTemp, desc, "VrSubmitDlssg::TestSbsB"))
			return;
		// Private copies only — never paint kTEMP. Magenta = first Submit, lime = second.
		const float magenta[4]{ 1.f, 0.f, 1.f, 1.f };
		const float lime[4]{ 0.f, 1.f, 0.f, 1.f };
		const bool paintedA = PaintSolid(context, testSbsA->uav.get(), desc.Width, desc.Height, magenta);
		context->Flush();
		SubmitOwnedSbs(static_cast<ID3D11Texture2D*>(testSbsA->resource.get()), desc.Width, desc.Height);
		const bool paintedB = PaintSolid(context, testSbsB->uav.get(), desc.Width, desc.Height, lime);
		context->Flush();
		SubmitOwnedSbs(static_cast<ID3D11Texture2D*>(testSbsB->resource.get()), desc.Width, desc.Height);
		if (!loggedMode2) {
			logger::info("[VrSubmitDlssg] mode 2 magenta then lime private SBS {}x{} paintA={} paintB={}",
				desc.Width, desc.Height, paintedA, paintedB);
			loggedMode2 = true;
		}
		return;
	}
	if (mode == 3) {
		static bool loggedMode3 = false;
		if (!loggedMode3) {
			logger::info("[VrSubmitDlssg] mode 3 retired; Submit_Default is not used");
			loggedMode3 = true;
		}
		return;
	}
	if (mode == 4) {
		if (!loggedMode4) {
			logger::info("[VrSubmitDlssg] mode 4 retired; cross-thread Submit is not used");
			loggedMode4 = true;
		}
		return;
	}
	static bool loggedUnsupported = false;
	if (!loggedUnsupported) {
		logger::info("[VrSubmitDlssg] vrSubmitTest mode {} not wired", mode);
		loggedUnsupported = true;
	}
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
	if (handle && !isOverlay && VrAriseTail::IsActive() && !VrBoosterTail::OwnsSubmit()) {
		// VR HUD is kMENUBG (KIT enlarges that slot). kHUDMENU is the flat enum.
		if (ID3D11Resource* menu = Rt(RE::RENDER_TARGETS::kMENUBG))
			VrAriseTail::NoteHud(static_cast<ID3D11Texture2D*>(menu));
		VrAriseTail::PublishKTemp(static_cast<ID3D11Texture2D*>(handle));
	}

	// Contract B: swallow vs not-takeover only. Overlay RTs are not a
	// passthrough. Out of scope this round: HUD/menu is swallowed and not folded into SBS.
	if (isOverlay) {
		if (handle == Rt(RE::RENDER_TARGETS::kMENUBG))
			VrAriseTail::NoteHud(static_cast<ID3D11Texture2D*>(handle));
		static std::atomic<std::uint32_t> overlaySwallowed{ 0 };
		const std::uint32_t n = ++overlaySwallowed;
		if (n <= 16 || (n % 120) == 0)
			logger::warn("[VrSubmitDlssg] overlay Submit #{} {} swallowed (HUD stamped after FG)",
				n, RtName(handle));
	}

	if (!isOverlay && VrBoosterTail::OwnsSubmit())
		return true;
	if (VrAriseTail::ShouldSwallowEngineSubmit())
		return true;
	if (!ownedSubmitted)
		return false;

	LogEngineSubmit(handle, false, true);
	if (!loggedSwallow) {
		logger::info("[VrSubmitDlssg] Swallowing engine world Submit after owned SBS");
		loggedSwallow = true;
	}
	return true;
}
#endif
