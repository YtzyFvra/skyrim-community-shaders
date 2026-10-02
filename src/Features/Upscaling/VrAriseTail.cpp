#include "PCH.h"
#include "VrAriseTail.h"

#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/D3D.h"
#include "../../Utils/FileSystem.h"
#include "../../Utils/WinApi.h"
#include "../Upscaling.h"
#include "GpuPass.h"
#include "VrDx12Db.h"
#include "VrPassTimer.h"
#include "VrSharedDevice.h"

#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#ifdef ENABLE_SKYRIM_VR
#	include "RE/B/BSOpenVR.h"
#	include "VrBeat/Metronome.h"
#	include "VrBridge/DbEnd.h"
#	include "VrFfxOp/Operator.h"
#	include "VrPose/Absolute.h"
#	include "VrSwitch/Master.h"
#	include "VrSubmitDlssg.h"
#	include "FoveatedRender/Ops.h"
#	include "../../Buffer.h"
#endif

using Microsoft::WRL::ComPtr;

void NotifyL2(const char* exportName)
{
	using Fn = void (*)();
	HMODULE module = GetModuleHandleW(L"openvr_api.dll");
	if (!module)
		module = GetModuleHandleW(L"vrclient_x64.dll");
	if (!module)
		return;
	auto* fn = reinterpret_cast<Fn>(GetProcAddress(module, exportName));
	if (fn)
		fn();
}

namespace
{
	void ReportQpc(const char* name, std::int64_t start, std::int64_t end)
	{
		if (!name || start <= 0 || end < start)
			return;
		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		if (!freq.QuadPart)
			return;
		const float ms = static_cast<float>(static_cast<double>(end - start) * 1000.0 /
			static_cast<double>(freq.QuadPart));
		VrPassTimer::ReportSpan(name, ms, false);
		VrPassTimer::ReportCpu(name, ms, false);
	}

	std::atomic<bool> installed{ false };
	std::atomic<bool> running{ false };
	std::atomic<bool> active{ false };
	std::atomic<bool> vrReady{ false };
	std::atomic<bool> abandoned{ false };
	std::thread worker;

	std::mutex statusMutex;
	VrAriseTail::Status status{};

	constexpr UINT kSharedSlots = 2;

	bool WaitFenceCpu(ID3D12Fence* fence, HANDLE event, UINT64 value)
	{
		if (!fence || value == 0)
			return true;
		if (fence->GetCompletedValue() >= value)
			return true;
		if (!event)
			return false;
		// A signaled auto-reset event would make the next wait look already done.
		ResetEvent(event);
		if (FAILED(fence->SetEventOnCompletion(value, event)))
			return false;
		return WaitForSingleObject(event, INFINITE) == WAIT_OBJECT_0;
	}

	const char* TotalName(bool asyncTail)
	{
		return asyncTail ? "VrArise::Total" : "VrSingle::Total";
	}

	struct SharedPublish
	{
		UINT64 fenceValue = 0;
		UINT32 slot = 0;
		UINT32 generation = 0;
		UINT32 width = 0;
		UINT32 height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		UINT32 guideWidth = 0;
		UINT32 guideHeight = 0;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT mvecFormat = DXGI_FORMAT_UNKNOWN;
		bool hasGuides = false;
		bool colorOnly = false;
		// Captured at the same instant as the color (in PublishKTemp, on the
		// game thread, right after this frame's UpdatePoses). Rides along
		// with the fenceValue so Tail/Doorbell can be arbitrarily late
		// without pairing this frame's pixels with a later frame's pose.
		VrPose::Body pose{};
		float frameMs = 11.11f;
		void* hudHandle = nullptr;
		sl::Constants constants[2]{};
		bool constantsValid = false;
		UINT64 frameId = 0;
		// Engine-side crop, backdrop and NR guides. The doorbell copies these
		// once and does not run those shaders.
		bool prepared = false;
		UINT32 prepGeneration = 0;
		void* prepTd = nullptr;
		void* prepEyeColor[2]{};
		void* prepEyeDepth[2]{};
		void* prepEyeMotion[2]{};
		void* prepDepthN[2]{};
		void* prepMotionN[2]{};
	};

	std::mutex sharedMutex;
	SharedPublish sharedPublished{};
	ComPtr<ID3D11Texture2D> sharedTex11[kSharedSlots];
	HANDLE sharedTexHandle[kSharedSlots]{};
	ComPtr<ID3D11Texture2D> sharedDepth11[kSharedSlots];
	HANDLE sharedDepthHandle[kSharedSlots]{};
	ComPtr<ID3D11RenderTargetView> sharedDepthRtv[kSharedSlots];
	ComPtr<ID3D11Texture2D> sharedMvec11[kSharedSlots];
	HANDLE sharedMvecHandle[kSharedSlots]{};
	ComPtr<ID3D11Fence> sharedFence11;
	HANDLE sharedFenceHandle = nullptr;
	UINT64 sharedFenceValue = 0;
	UINT32 sharedGeneration = 0;
	UINT32 sharedWidth = 0;
	UINT32 sharedHeight = 0;
	DXGI_FORMAT sharedFormat = DXGI_FORMAT_UNKNOWN;
	UINT32 sharedGuideWidth = 0;
	UINT32 sharedGuideHeight = 0;
	DXGI_FORMAT sharedMvecFormat = DXGI_FORMAT_UNKNOWN;
	bool sharedHasGuides = false;
	sl::Constants sharedConstants[2]{};
	bool sharedConstantsValid = false;
	std::atomic<UINT64> publishedThisFrame{ UINT64_MAX };
	std::atomic<bool> loggedPublish{ false };
	std::atomic<bool> loggedPublishFail{ false };
	std::atomic<bool> publishPausedForLoad{ false };
	ComPtr<ID3D11Texture2D> sharedHud11[kSharedSlots];
	HANDLE sharedHudHandle[kSharedSlots]{};
	UINT sharedHudW = 0;
	UINT sharedHudH = 0;
	DXGI_FORMAT sharedHudFmt = DXGI_FORMAT_UNKNOWN;
	std::atomic<LONGLONG> publishResumeQpc{ 0 };
	std::atomic<bool> historyResetPending{ false };
	std::atomic<UINT64> lastPublishedFrame{ 0 };
	std::atomic<UINT64> lastPublishFence{ 0 };
	std::atomic<UINT64> lastConsumedFence{ 0 };
	const char* noNewWhy = nullptr;
	LARGE_INTEGER noNewStart{};

	void EndNoNewFrame()
	{
		if (!noNewWhy)
			return;
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		LARGE_INTEGER freq{};
		QueryPerformanceFrequency(&freq);
		const double sec = freq.QuadPart ? static_cast<double>(now.QuadPart - noNewStart.QuadPart) / static_cast<double>(freq.QuadPart) : 0.0;
		logger::info("[VrAriseTail] no new frame end ({}) {:.1f} s", noNewWhy, sec);
		noNewWhy = nullptr;
	}

	void NoteNoNewFrame(const char* why)
	{
		historyResetPending.store(true, std::memory_order_release);
		if (noNewWhy == why)
			return;
		EndNoNewFrame();
		noNewWhy = why;
		QueryPerformanceCounter(&noNewStart);
		logger::info("[VrAriseTail] no new frame start ({})", why);
	}

	bool CreateSharedTex(ID3D11Device5* device5, UINT width, UINT height, DXGI_FORMAT format, UINT bindFlags,
		ComPtr<ID3D11Texture2D>& tex, HANDLE& handle, const char* name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bindFlags;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(device5->CreateTexture2D(&desc, nullptr, &tex)))
			return false;
		Util::SetResourceName(tex.Get(), name);
		ComPtr<IDXGIResource1> dxgi;
		if (FAILED(tex.As(&dxgi)))
			return false;
		return SUCCEEDED(dxgi->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle));
	}

	struct PrepTex
	{
		ComPtr<ID3D11Texture2D> tex;
		ComPtr<ID3D11ShaderResourceView> srv;
		ComPtr<ID3D11UnorderedAccessView> uav;
		HANDLE handle = nullptr;

		void Reset()
		{
			if (handle) {
				CloseHandle(handle);
				handle = nullptr;
			}
			uav.Reset();
			srv.Reset();
			tex.Reset();
		}
	};

	struct PrepSlot
	{
		PrepTex td;
		PrepTex eyeColor[2];
		PrepTex eyeDepth[2];
		PrepTex eyeMotion[2];
		PrepTex depthN[2];
		PrepTex motionN[2];

		void Reset()
		{
			td.Reset();
			for (int eye = 0; eye < 2; ++eye) {
				eyeColor[eye].Reset();
				eyeDepth[eye].Reset();
				eyeMotion[eye].Reset();
				depthN[eye].Reset();
				motionN[eye].Reset();
			}
		}
	};

	PrepSlot prepSlots[kSharedSlots];
	UINT prepTdW = 0;
	UINT prepTdH = 0;
	UINT prepCropW = 0;
	UINT prepCropH = 0;
	UINT prepSrcW[2]{};
	UINT prepNrW = 0;
	UINT prepNrH = 0;
	DXGI_FORMAT prepColorFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT prepMvecFormat = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT prepMotionNFormat = DXGI_FORMAT_UNKNOWN;
	UINT32 prepGeneration = 0;
	ComPtr<ID3D11ComputeShader> edgeResizeCs;
	ComPtr<ID3D11Buffer> edgeResizeCb;
	ComPtr<ID3D11ShaderResourceView> edgeColorSrv;
	ID3D11Texture2D* edgeColorSrvOwner = nullptr;

	void UnbindCompute()
	{
		if (!globals::d3d::context)
			return;
		ID3D11ShaderResourceView* nullSrv[2]{};
		ID3D11UnorderedAccessView* nullUav[2]{};
		ID3D11Buffer* nullCb[]{ nullptr };
		globals::d3d::context->CSSetShaderResources(0, 2, nullSrv);
		globals::d3d::context->CSSetUnorderedAccessViews(0, 2, nullUav, nullptr);
		globals::d3d::context->CSSetConstantBuffers(0, 1, nullCb);
		globals::d3d::context->CSSetShader(nullptr, nullptr, 0);
	}

	bool MakePrepTex(ID3D11Device* device, UINT width, UINT height, DXGI_FORMAT format, UINT bind,
		bool srv, bool uav, PrepTex& out, const char* name)
	{
		out.Reset();
		if (!device || !width || !height)
			return false;
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = width;
		desc.Height = height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = bind;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		if (FAILED(device->CreateTexture2D(&desc, nullptr, &out.tex)))
			return false;
		Util::SetResourceName(out.tex.Get(), name);
		if (srv && FAILED(device->CreateShaderResourceView(out.tex.Get(), nullptr, &out.srv)))
			return false;
		if (srv)
			Util::SetResourceName(out.srv.Get(), name);
		if (uav && FAILED(device->CreateUnorderedAccessView(out.tex.Get(), nullptr, &out.uav)))
			return false;
		if (uav)
			Util::SetResourceName(out.uav.Get(), name);
		ComPtr<IDXGIResource1> dxgi;
		if (FAILED(out.tex.As(&dxgi)))
			return false;
		return SUCCEEDED(dxgi->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &out.handle));
	}

	DXGI_FORMAT ChooseMotionNFormat()
	{
		ID3D12Device* device12 = VrSharedDevice::Get(nullptr);
		if (!device12)
			return DXGI_FORMAT_R16G16_FLOAT;
		for (const DXGI_FORMAT format : { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT }) {
			D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ format };
			if (SUCCEEDED(device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
				(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0)
				return format;
		}
		return DXGI_FORMAT_R16G16_FLOAT;
	}

	void ResetPrepSlots()
	{
		for (auto& slot : prepSlots)
			slot.Reset();
		prepTdW = prepTdH = prepCropW = prepCropH = prepNrW = prepNrH = 0;
		prepColorFormat = DXGI_FORMAT_UNKNOWN;
		prepMvecFormat = DXGI_FORMAT_UNKNOWN;
		prepMotionNFormat = DXGI_FORMAT_UNKNOWN;
	}

	bool EnsureEdgeShaders()
	{
		if (edgeResizeCs && edgeResizeCb)
			return true;
		ComPtr<ID3DBlob> resizeBlob;
		ComPtr<ID3DBlob> errors;
		errors.Reset();
		if (!edgeResizeCs && FAILED(D3DCompileFromFile(L"Data/Shaders/Upscaling/NeuralRendering/ResizeGuidesCS.hlsl",
				nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &resizeBlob, &errors))) {
			logger::warn("[VrAriseTail] edge resize compile failed: {}",
				errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
			return false;
		}
		if (!edgeResizeCs && FAILED(globals::d3d::device->CreateComputeShader(
				resizeBlob->GetBufferPointer(), resizeBlob->GetBufferSize(), nullptr, &edgeResizeCs)))
			return false;
		if (edgeResizeCs)
			Util::SetResourceName(edgeResizeCs.Get(), "VrAriseTail::EdgeResizeGuides");
		if (!edgeResizeCb) {
			D3D11_BUFFER_DESC cb{};
			cb.ByteWidth = 32;
			cb.Usage = D3D11_USAGE_DYNAMIC;
			cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(globals::d3d::device->CreateBuffer(&cb, nullptr, &edgeResizeCb)))
				return false;
			Util::SetResourceName(edgeResizeCb.Get(), "VrAriseTail::EdgeResizeCB");
		}
		return edgeResizeCs && edgeResizeCb;
	}

	bool EnsurePrepSlots(ID3D11Device* device, const VrDx12Db::EdgePlan& edge, DXGI_FORMAT colorFormat, DXGI_FORMAT mvecFormat)
	{
		const DXGI_FORMAT motionN = prepMotionNFormat != DXGI_FORMAT_UNKNOWN ? prepMotionNFormat : ChooseMotionNFormat();
		if (prepSlots[0].td.tex && prepTdW == edge.tdW && prepTdH == edge.tdH &&
			prepCropW == edge.cropW && prepCropH == edge.cropH && prepSrcW[0] == edge.srcW[0] && prepSrcW[1] == edge.srcW[1] &&
			prepNrW == edge.nrW && prepNrH == edge.nrH &&
			prepColorFormat == colorFormat && prepMvecFormat == mvecFormat && prepMotionNFormat == motionN)
			return true;
		ResetPrepSlots();
		prepMotionNFormat = motionN;
		const UINT colorBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const UINT copyBind = 0;
		const UINT readBind = D3D11_BIND_SHADER_RESOURCE;
		const UINT writeBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		for (UINT slot = 0; slot < kSharedSlots; ++slot) {
			auto& prep = prepSlots[slot];
			if (!MakePrepTex(device, edge.tdW, edge.tdH, colorFormat, colorBind, false, true, prep.td,
					slot ? "VrAriseTail::PrepTd1" : "VrAriseTail::PrepTd0"))
				return false;
			for (int eye = 0; eye < 2; ++eye) {
				const char* colorName = eye ? "VrAriseTail::PrepEyeColor1" : "VrAriseTail::PrepEyeColor0";
				const char* depthName = eye ? "VrAriseTail::PrepEyeDepth1" : "VrAriseTail::PrepEyeDepth0";
				const char* motionName = eye ? "VrAriseTail::PrepEyeMotion1" : "VrAriseTail::PrepEyeMotion0";
				const char* depthNName = eye ? "VrAriseTail::PrepDepthN1" : "VrAriseTail::PrepDepthN0";
				const char* motionNName = eye ? "VrAriseTail::PrepMotionN1" : "VrAriseTail::PrepMotionN0";
				const UINT eyeW = edge.srcW[eye] ? edge.srcW[eye] : edge.cropW;
				if (!MakePrepTex(device, eyeW, edge.cropH, colorFormat, copyBind, false, false, prep.eyeColor[eye], colorName) ||
					!MakePrepTex(device, eyeW, edge.cropH, DXGI_FORMAT_R32_FLOAT, readBind, true, false, prep.eyeDepth[eye], depthName) ||
					!MakePrepTex(device, eyeW, edge.cropH, mvecFormat, readBind, true, false, prep.eyeMotion[eye], motionName) ||
					!MakePrepTex(device, edge.nrW, edge.nrH, DXGI_FORMAT_R32_FLOAT, writeBind, false, true, prep.depthN[eye], depthNName) ||
					!MakePrepTex(device, edge.nrW, edge.nrH, motionN, writeBind, false, true, prep.motionN[eye], motionNName))
					return false;
			}
		}
		prepTdW = edge.tdW;
		prepTdH = edge.tdH;
		prepCropW = edge.cropW;
		prepCropH = edge.cropH;
		prepSrcW[0] = edge.srcW[0];
		prepSrcW[1] = edge.srcW[1];
		prepNrW = edge.nrW;
		prepNrH = edge.nrH;
		prepColorFormat = colorFormat;
		prepMvecFormat = mvecFormat;
		++prepGeneration;
		return true;
	}

	bool PrepareEngineEdge(ID3D11Device* device, UINT slot, ID3D11Texture2D* colorSrc,
		UINT colorW, UINT colorH, DXGI_FORMAT colorFormat, UINT guideW, UINT guideH, DXGI_FORMAT mvecFormat)
	{
		if (!device || !colorSrc || slot >= kSharedSlots || colorFormat != DXGI_FORMAT_R8G8B8A8_UNORM)
			return false;
		if (!guideW || guideW != colorW || guideH != colorH || !sharedDepth11[slot] || !sharedMvec11[slot])
			return false;
		VrDx12Db::EdgePlan edge{};
		if (!VrDx12Db::ComputeEdgePlan(colorW, colorH, edge) || !edge.srReady || !edge.cropW || !edge.nrW)
			return false;
		if (!EnsureEdgeShaders() || !EnsurePrepSlots(device, edge, colorFormat, mvecFormat)) {
			ResetPrepSlots();
			return false;
		}
		if (edgeColorSrvOwner != colorSrc) {
			edgeColorSrv.Reset();
			if (FAILED(device->CreateShaderResourceView(colorSrc, nullptr, &edgeColorSrv)))
				return false;
			Util::SetResourceName(edgeColorSrv.Get(), "VrAriseTail::EdgeColorSRV");
			edgeColorSrvOwner = colorSrc;
		}
		auto* context = globals::d3d::context;
		auto& prep = prepSlots[slot];
		for (uint32_t eye = 0; eye < 2; ++eye) {
			const UINT copyW = edge.srcW[eye] ? edge.srcW[eye] : edge.cropW;
			const UINT copyX = edge.srcW[eye] ? edge.srcX[eye] : edge.cropX[eye];
			const UINT sbsX = (eye ? edge.eyeW : 0) + copyX;
			const D3D11_BOX box{ sbsX, edge.cropY[eye], 0, sbsX + copyW, edge.cropY[eye] + edge.cropH, 1 };
			context->CopySubresourceRegion(prep.eyeColor[eye].tex.Get(), 0, 0, 0, 0, colorSrc, 0, &box);
			context->CopySubresourceRegion(prep.eyeDepth[eye].tex.Get(), 0, 0, 0, 0, sharedDepth11[slot].Get(), 0, &box);
			context->CopySubresourceRegion(prep.eyeMotion[eye].tex.Get(), 0, 0, 0, 0, sharedMvec11[slot].Get(), 0, &box);
		}
		struct ResizeCB
		{
			uint32_t source[2];
			uint32_t target[2];
			uint32_t guidance[2];
			uint32_t filter;
			float multiplier;
		};
		static_assert(sizeof(ResizeCB) == 32);
		D3D11_MAPPED_SUBRESOURCE mapped{};
		context->CSSetShader(edgeResizeCs.Get(), nullptr, 0);
		ID3D11Buffer* cbs[]{ edgeResizeCb.Get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		for (uint32_t eye = 0; eye < 2; ++eye) {
			if (FAILED(context->Map(edgeResizeCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
				return false;
			const UINT sourceW = edge.srcW[eye] ? edge.srcW[eye] : edge.cropW;
			ResizeCB constants{
				{ sourceW, edge.cropH },
				{ edge.nrW, edge.nrH },
				{ edge.act1W, edge.act1H },
				edge.depthFilter,
				1.0f,
			};
			std::memcpy(mapped.pData, &constants, sizeof(constants));
			context->Unmap(edgeResizeCb.Get(), 0);
			ID3D11ShaderResourceView* srvs[]{ prep.eyeDepth[eye].srv.Get(), prep.eyeMotion[eye].srv.Get() };
			ID3D11UnorderedAccessView* uavs[]{ prep.depthN[eye].uav.Get(), prep.motionN[eye].uav.Get() };
			context->CSSetShaderResources(0, 2, srvs);
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			context->Dispatch((edge.nrW + 7) / 8, (edge.nrH + 7) / 8, 1);
		}
		UnbindCompute();
		FoveatedRenderImpl::Ops::StretchDRSBothEyes(prep.td.uav.Get(), edge.tdEyeW, edge.tdEyeH,
			edge.eyeW, edge.eyeH, colorW, colorH, edgeColorSrv.Get(), 2, 1.0f, false);
		static bool logged = false;
		if (!logged) {
			logged = true;
			logger::info("[VrAriseTail] engine edge crop {}x{} nr {}x{} td {}x{}",
				edge.cropW, edge.cropH, edge.nrW, edge.nrH, edge.tdW, edge.tdH);
		}
		return prep.td.handle != nullptr;
	}

	void CloseSharedHandles()
	{
		for (UINT i = 0; i < kSharedSlots; ++i) {
			if (sharedTexHandle[i]) {
				CloseHandle(sharedTexHandle[i]);
				sharedTexHandle[i] = nullptr;
			}
			sharedTex11[i].Reset();
			if (sharedDepthHandle[i]) {
				CloseHandle(sharedDepthHandle[i]);
				sharedDepthHandle[i] = nullptr;
			}
			sharedDepthRtv[i].Reset();
			sharedDepth11[i].Reset();
			if (sharedMvecHandle[i]) {
				CloseHandle(sharedMvecHandle[i]);
				sharedMvecHandle[i] = nullptr;
			}
			sharedMvec11[i].Reset();
		}
		if (sharedFenceHandle) {
			CloseHandle(sharedFenceHandle);
			sharedFenceHandle = nullptr;
		}
		sharedFence11.Reset();
		sharedHasGuides = false;
	}

	bool EnsureSharedSlots(ID3D11Device5* device5, UINT width, UINT height, DXGI_FORMAT format,
		UINT guideWidth, UINT guideHeight, DXGI_FORMAT mvecFormat)
	{
		const bool wantGuides = guideWidth && guideHeight && mvecFormat != DXGI_FORMAT_UNKNOWN;
		if (sharedTex11[0] && sharedWidth == width && sharedHeight == height && sharedFormat == format &&
			sharedFence11 && sharedHasGuides == wantGuides &&
			(!wantGuides || (sharedGuideWidth == guideWidth && sharedGuideHeight == guideHeight &&
								sharedMvecFormat == mvecFormat)))
			return true;
		CloseSharedHandles();
		for (UINT i = 0; i < kSharedSlots; ++i) {
			if (!CreateSharedTex(device5, width, height, format, 0, sharedTex11[i], sharedTexHandle[i],
					"VrAriseTail::SharedKTemp"))
				return false;
		}
		if (wantGuides) {
			for (UINT i = 0; i < kSharedSlots; ++i) {
				if (!CreateSharedTex(device5, guideWidth, guideHeight, DXGI_FORMAT_R32_FLOAT,
						D3D11_BIND_RENDER_TARGET, sharedDepth11[i], sharedDepthHandle[i],
						"VrAriseTail::SharedDepth") ||
					FAILED(device5->CreateRenderTargetView(sharedDepth11[i].Get(), nullptr, &sharedDepthRtv[i])))
					return false;
				Util::SetResourceName(sharedDepthRtv[i].Get(), "VrAriseTail::SharedDepth RTV");
				if (!CreateSharedTex(device5, guideWidth, guideHeight, mvecFormat, 0, sharedMvec11[i],
						sharedMvecHandle[i], "VrAriseTail::SharedMvec"))
					return false;
			}
			sharedGuideWidth = guideWidth;
			sharedGuideHeight = guideHeight;
			sharedMvecFormat = mvecFormat;
			sharedHasGuides = true;
		}
		if (FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sharedFence11))))
			return false;
		if (FAILED(sharedFence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle)))
			return false;
		sharedWidth = width;
		sharedHeight = height;
		sharedFormat = format;
		++sharedGeneration;
		sharedFenceValue = 0;
		return true;
	}

	SharedPublish SnapshotShared()
	{
		std::lock_guard lock(sharedMutex);
		return sharedPublished;
	}

	bool CopySharedHandle(UINT slot, HANDLE& out)
	{
		std::lock_guard lock(sharedMutex);
		if (slot >= kSharedSlots || !sharedTexHandle[slot])
			return false;
		out = sharedTexHandle[slot];
		return true;
	}

	bool CopyGuideHandle(const HANDLE* handles, UINT slot, HANDLE& out)
	{
		std::lock_guard lock(sharedMutex);
		if (slot >= kSharedSlots || !handles[slot])
			return false;
		out = handles[slot];
		return true;
	}

	HANDLE SharedFenceHandle()
	{
		std::lock_guard lock(sharedMutex);
		return sharedFenceHandle;
	}

	void PublishStatus(const VrAriseTail::Status& next)
	{
		std::scoped_lock lock(statusMutex);
		status = next;
	}

	bool BootLatchedArise()
	{
		if (!globals::game::isVR)
			return false;
		auto& upscaling = globals::features::upscaling;
		upscaling.bootSnapshot.LatchIfNeeded(upscaling.settings);
		return upscaling.bootSnapshot.Boot(&Upscaling::Settings::vrArise);
	}

	// "True Single": ShouldSwallowEngineSubmit blocks every frame on this instead of only
	// at session bind. See Upscaling::Settings::vrSingleBlock.
	bool BootVrSingleBlock()
	{
		if (!globals::game::isVR)
			return false;
		auto& upscaling = globals::features::upscaling;
		upscaling.bootSnapshot.LatchIfNeeded(upscaling.settings);
		return upscaling.bootSnapshot.Boot(&Upscaling::Settings::vrSingleBlock);
	}

#ifdef ENABLE_SKYRIM_VR
	void AbandonTakeover()
	{
		abandoned.store(true, std::memory_order_release);
		running.store(false, std::memory_order_release);
		active.store(false, std::memory_order_release);
		VrBeat::Stop();
	}

	struct UpdatePosesHook
	{
		static void thunk(RE::BSOpenVR* openVR, bool useLivePoses)
		{
			if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire)) {
				func(openVR, useLivePoses);
				return;
			}
			VrBeat::HookCompositorPoses();
			// L1 virtualizes WaitGetPoses inside OC (throttle + cached poses).
			func(openVR, useLivePoses);
			VrPoseAbs::OnEngineDrawStart();
			static bool logged = false;
			if (!logged) {
				logger::info("[VrAriseTail] Engine UpdatePoses; Abs from engine's own OpenVR pose snoop");
				logged = true;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void InstallUpdatePosesHook()
	{
		stl::write_vfunc<0x10, UpdatePosesHook>(RE::VTABLE_BSOpenVR[0]);
	}

	IDXGIAdapter* QueryGameAdapter(ComPtr<IDXGIAdapter>& keep)
	{
		keep.Reset();
		auto* device11 = globals::d3d::device;
		if (!device11)
			return nullptr;
		ComPtr<IDXGIDevice> dxgiDevice;
		if (FAILED(device11->QueryInterface(IID_PPV_ARGS(&dxgiDevice))))
			return nullptr;
		if (FAILED(dxgiDevice->GetAdapter(&keep)))
			return nullptr;
		return keep.Get();
	}

	void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
		D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
	{
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = resource;
		barrier.Transition.StateBefore = before;
		barrier.Transition.StateAfter = after;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		list->ResourceBarrier(1, &barrier);
	}

	DXGI_FORMAT UavTypedFormat(DXGI_FORMAT format)
	{
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			return DXGI_FORMAT_R8G8B8A8_UNORM;
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			return DXGI_FORMAT_B8G8R8A8_UNORM;
		default:
			return format;
		}
	}

	bool FormatAllowsTypedUav(DXGI_FORMAT format)
	{
		switch (format) {
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
		case DXGI_FORMAT_R16G16B16A16_FLOAT:
		case DXGI_FORMAT_R32_FLOAT:
			return true;
		default:
			return false;
		}
	}

	struct Dx12Path
	{
		ComPtr<IDXGIAdapter> adapter;
		ComPtr<ID3D12Device> device;
		ComPtr<ID3D12CommandQueue> queue;
		// Graph list. copyList is the input copy, submitted first so the engine
		// can return before this list is recorded. FSR uses its own lists.
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		ComPtr<ID3D12CommandAllocator> copyAllocator;
		ComPtr<ID3D12GraphicsCommandList> copyList;
		std::uint64_t passSerial = 0;
		LARGE_INTEGER spanStart{};
		LARGE_INTEGER spanCopyEnd{};
		LARGE_INTEGER spanRecordBegin{};
		LARGE_INTEGER spanRecordEnd{};
		LARGE_INTEGER spanSubmitEnd{};
		float spanWaitMs = 0.0f;
		ComPtr<ID3D12CommandAllocator> graphAllocB;
		ComPtr<ID3D12GraphicsCommandList> graphListB;
		UINT64 graphBusy[2]{};
		UINT64 pendingQueryFence = 0;
		// Snapshot resets slot N only after that slot's previous copy has retired.
		ComPtr<ID3D12CommandAllocator> snapAllocator[2];
		ComPtr<ID3D12GraphicsCommandList> snapList[2];
		UINT64 prepareFenceValue = 0;
		ComPtr<ID3D12DescriptorHeap> uavHeap;
		ComPtr<ID3D12RootSignature> opaqueRoot;
		ComPtr<ID3D12PipelineState> opaquePso;
		ComPtr<ID3D12Resource> color;
		ComPtr<ID3D12Resource> shared12[kSharedSlots];
		ComPtr<ID3D12Resource> sharedDepth12[kSharedSlots];
		ComPtr<ID3D12Resource> sharedMvec12[kSharedSlots];
		ComPtr<ID3D12Resource> sharedHud12[kSharedSlots];
		HANDLE openedHudHandle[kSharedSlots]{};
		ComPtr<ID3D12Resource> hudLatch;
		D3D12_RESOURCE_STATES hudLatchState = D3D12_RESOURCE_STATE_COMMON;
		bool hudReady = false;
		ComPtr<ID3D12Resource> depth;
		ComPtr<ID3D12Resource> mvec;
		D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES mvecState = D3D12_RESOURCE_STATE_COMMON;
		UINT guideWidth = 0;
		UINT guideHeight = 0;
		DXGI_FORMAT depthFormat = DXGI_FORMAT_R32_FLOAT;
		DXGI_FORMAT mvecFormat = DXGI_FORMAT_UNKNOWN;
		bool guidesReady = false;
		UINT32 openedPrepGen = 0;
		ComPtr<ID3D12Resource> openTd[kSharedSlots];
		ComPtr<ID3D12Resource> openEyeColor[kSharedSlots][2];
		ComPtr<ID3D12Resource> openEyeDepth[kSharedSlots][2];
		ComPtr<ID3D12Resource> openEyeMotion[kSharedSlots][2];
		ComPtr<ID3D12Resource> openDepthN[kSharedSlots][2];
		ComPtr<ID3D12Resource> openMotionN[kSharedSlots][2];
		ComPtr<ID3D12Resource> prepTd;
		D3D12_RESOURCE_STATES prepTdState = D3D12_RESOURCE_STATE_COMMON;
		ComPtr<ID3D12Resource> prepEyeColor[2];
		D3D12_RESOURCE_STATES prepEyeColorState[2]{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
		ComPtr<ID3D12Resource> prepEyeDepth[2];
		D3D12_RESOURCE_STATES prepEyeDepthState[2]{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
		ComPtr<ID3D12Resource> prepEyeMotion[2];
		D3D12_RESOURCE_STATES prepEyeMotionState[2]{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
		ComPtr<ID3D12Resource> prepDepthN[2];
		D3D12_RESOURCE_STATES prepDepthNState[2]{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
		ComPtr<ID3D12Resource> prepMotionN[2];
		D3D12_RESOURCE_STATES prepMotionNState[2]{ D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON };
		bool prepReady = false;
		ComPtr<ID3D12Fence> fence;
		ComPtr<ID3D12Fence> openedSharedFence;
		HANDLE openedFenceHandle = nullptr;
		UINT32 openedGeneration = 0;
		HANDLE fenceEvent = nullptr;
		UINT64 fenceValue = 0;
		UINT64 lastConsumed = 0;
		D3D12_RESOURCE_STATES colorState = D3D12_RESOURCE_STATE_COMMON;
		ID3D12Resource* present = nullptr;
		D3D12_RESOURCE_STATES presentState = D3D12_RESOURCE_STATE_COMMON;
		UINT presentW = 0;
		UINT presentH = 0;
		ID3D12Resource* stripColor = nullptr;
		ID3D12Resource* stripDepth = nullptr;
		ID3D12Resource* stripMotion = nullptr;
		VrDx12Db::StripView strip{};
		std::uint32_t width = 0;
		std::uint32_t height = 0;

		// Diagnostics only: PrepareShared has ~6 distinct early-return sites
		// and the call site only ever logged "failed" once for the whole
		// session, so a real occurrence couldn't be told apart from a
		// one-off hitch. lastFailReason names which check tripped; failCount
		// tracks whether it happened once or is stuck failing every cycle.
		const char* lastFailReason = "";
		std::uint64_t failCount = 0;
		bool logNextSuccess = false;

		~Dx12Path()
		{
			if (fenceEvent)
				CloseHandle(fenceEvent);
		}

		bool CreateDevice()
		{
			if (device)
				return true;
			IDXGIAdapter* chosen = QueryGameAdapter(adapter);
			device = VrSharedDevice::Get(chosen);
			queue = VrSharedDevice::Queue(chosen);
			if (!device || !queue) {
				logger::warn("[VrAriseTail] shared D3D12 device or queue unavailable");
				return false;
			}
			auto& sl = globals::features::upscaling.streamlineDX12;
			if (sl.initialized) {
				sl.SetD3DDevice12(device.Get());
				sl.CheckFeatures(chosen);
				sl.PostDevice();
			}
			logger::info("[VrAriseTail] doorbell uses the shared D3D12 queue");
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))))
				return false;
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
				return false;
			list->Close();
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&graphAllocB))))
				return false;
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, graphAllocB.Get(), nullptr, IID_PPV_ARGS(&graphListB))))
				return false;
			graphListB->Close();
			if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator))))
				return false;
			if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAllocator.Get(), nullptr, IID_PPV_ARGS(&copyList))))
				return false;
			copyList->Close();
			for (int i = 0; i < 2; ++i) {
				if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&snapAllocator[i]))))
					return false;
				if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, snapAllocator[i].Get(), nullptr, IID_PPV_ARGS(&snapList[i]))))
					return false;
				snapList[i]->Close();
			}
			if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
				return false;
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			return fenceEvent != nullptr;
		}

		bool WaitFenceTimeout(UINT64 value)
		{
			return WaitFenceCpu(fence.Get(), fenceEvent, value);
		}

		bool EnsureOpaqueCs()
		{
			if (opaquePso)
				return true;
			static const char kSrc[] =
				"RWTexture2D<float4> Out : register(u0);\n"
				"[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {\n"
				"  float4 c = Out[id.xy]; c.a = 1.0; Out[id.xy] = c;\n"
				"}\n";
			ComPtr<ID3DBlob> blob;
			ComPtr<ID3DBlob> errors;
			if (FAILED(D3DCompile(kSrc, sizeof(kSrc) - 1, "VrAriseOpaque", nullptr, nullptr, "main", "cs_5_1",
					0, 0, &blob, &errors))) {
				logger::warn("[VrAriseTail] ForceOpaque CS compile failed: {}",
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown");
				return false;
			}
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
			range.NumDescriptors = 1;
			D3D12_ROOT_PARAMETER param{};
			param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			param.DescriptorTable.NumDescriptorRanges = 1;
			param.DescriptorTable.pDescriptorRanges = &range;
			D3D12_ROOT_SIGNATURE_DESC rsDesc{};
			rsDesc.NumParameters = 1;
			rsDesc.pParameters = &param;
			ComPtr<ID3DBlob> rsBlob;
			if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr)))
				return false;
			if (FAILED(device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
					IID_PPV_ARGS(&opaqueRoot))))
				return false;
			D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
			pso.pRootSignature = opaqueRoot.Get();
			pso.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
			if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&opaquePso))))
				return false;
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
			heapDesc.NumDescriptors = 1;
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&uavHeap))))
				return false;
			return true;
		}

		bool EnsureColorFor(UINT w, UINT h, DXGI_FORMAT format)
		{
			if (color && width == w && height == h && color->GetDesc().Format == format)
				return true;
			color.Reset();
			width = w;
			height = h;
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color)))) {
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
				if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&color))))
					return false;
			}
			colorState = D3D12_RESOURCE_STATE_COMMON;
			color->SetName(L"VrAriseTail::SharedCopy");
			return true;
		}

		bool EnsureGuide(ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES& state,
			UINT w, UINT h, DXGI_FORMAT format, const wchar_t* name)
		{
			if (resource) {
				const auto desc = resource->GetDesc();
				if (desc.Width == w && desc.Height == h && desc.Format == format)
					return true;
			}
			resource.Reset();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
				return false;
			resource->SetName(name);
			state = D3D12_RESOURCE_STATE_COMMON;
			return true;
		}

		bool ImportShared(const SharedPublish& published)
		{
			if (openedGeneration != published.generation) {
				for (auto& resource : shared12)
					resource.Reset();
				for (auto& resource : sharedDepth12)
					resource.Reset();
				for (auto& resource : sharedMvec12)
					resource.Reset();
				for (auto& resource : sharedHud12)
					resource.Reset();
				openedHudHandle[0] = openedHudHandle[1] = nullptr;
				openedSharedFence.Reset();
				openedFenceHandle = nullptr;
				openedGeneration = published.generation;
			}
			const HANDLE fenceHandle = SharedFenceHandle();
			if (!openedSharedFence) {
				if (!fenceHandle || FAILED(device->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&openedSharedFence))))
					return false;
				openedFenceHandle = fenceHandle;
			}
			if (!shared12[published.slot]) {
				HANDLE texHandle = nullptr;
				if (!CopySharedHandle(published.slot, texHandle) ||
					FAILED(device->OpenSharedHandle(texHandle, IID_PPV_ARGS(&shared12[published.slot]))))
					return false;
			}
			return shared12[published.slot] != nullptr;
		}

		bool ImportGuides(const SharedPublish& published)
		{
			if (!published.hasGuides)
				return false;
			if (!sharedDepth12[published.slot]) {
				HANDLE handle = nullptr;
				if (!CopyGuideHandle(sharedDepthHandle, published.slot, handle) ||
					FAILED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&sharedDepth12[published.slot]))))
					return false;
			}
			if (!sharedMvec12[published.slot]) {
				HANDLE handle = nullptr;
				if (!CopyGuideHandle(sharedMvecHandle, published.slot, handle) ||
					FAILED(device->OpenSharedHandle(handle, IID_PPV_ARGS(&sharedMvec12[published.slot]))))
					return false;
			}
			return sharedDepth12[published.slot] && sharedMvec12[published.slot];
		}

		bool ImportHud(const SharedPublish& published)
		{
			if (!published.hudHandle)
				return false;
			if (openedHudHandle[published.slot] != published.hudHandle) {
				sharedHud12[published.slot].Reset();
				openedHudHandle[published.slot] = nullptr;
				if (FAILED(device->OpenSharedHandle(static_cast<HANDLE>(published.hudHandle),
						IID_PPV_ARGS(&sharedHud12[published.slot]))))
					return false;
				openedHudHandle[published.slot] = static_cast<HANDLE>(published.hudHandle);
			}
			return sharedHud12[published.slot] != nullptr;
		}

		bool EnsureCopied(ComPtr<ID3D12Resource>& resource, D3D12_RESOURCE_STATES& state,
			UINT w, UINT h, DXGI_FORMAT format, bool uav, const wchar_t* name)
		{
			if (resource) {
				const auto desc = resource->GetDesc();
				const bool hasUav = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;
				if (desc.Width == w && desc.Height == h && desc.Format == format && hasUav == uav)
					return true;
			}
			resource.Reset();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = format;
			desc.SampleDesc.Count = 1;
			if (uav)
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource))))
				return false;
			resource->SetName(name);
			state = D3D12_RESOURCE_STATE_COMMON;
			return true;
		}

		bool OpenHandle(void* handle, ComPtr<ID3D12Resource>& dest)
		{
			if (dest)
				return true;
			if (!handle)
				return false;
			return SUCCEEDED(device->OpenSharedHandle(static_cast<HANDLE>(handle), IID_PPV_ARGS(&dest)));
		}

		void CopyInto(ID3D12GraphicsCommandList* cmd, ID3D12Resource* destination, D3D12_RESOURCE_STATES& state, ID3D12Resource* source)
		{
			if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, destination, state, D3D12_RESOURCE_STATE_COPY_DEST);
				state = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			cmd->CopyResource(destination, source);
			Transition(cmd, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			state = D3D12_RESOURCE_STATE_COMMON;
		}

		bool CopyPrepared(ID3D12GraphicsCommandList* cmd, const SharedPublish& published)
		{
			if (!published.prepTd)
				return false;
			if (openedPrepGen != published.prepGeneration) {
				for (UINT i = 0; i < kSharedSlots; ++i) {
					openTd[i].Reset();
					for (int eye = 0; eye < 2; ++eye) {
						openEyeColor[i][eye].Reset();
						openEyeDepth[i][eye].Reset();
						openEyeMotion[i][eye].Reset();
						openDepthN[i][eye].Reset();
						openMotionN[i][eye].Reset();
					}
				}
				openedPrepGen = published.prepGeneration;
			}
			const UINT slot = published.slot;
			if (!OpenHandle(published.prepTd, openTd[slot]))
				return false;
			for (int eye = 0; eye < 2; ++eye) {
				if (!OpenHandle(published.prepEyeColor[eye], openEyeColor[slot][eye]) ||
					!OpenHandle(published.prepEyeDepth[eye], openEyeDepth[slot][eye]) ||
					!OpenHandle(published.prepEyeMotion[eye], openEyeMotion[slot][eye]) ||
					!OpenHandle(published.prepDepthN[eye], openDepthN[slot][eye]) ||
					!OpenHandle(published.prepMotionN[eye], openMotionN[slot][eye]))
					return false;
			}
			const auto tdDesc = openTd[slot]->GetDesc();
			if (!EnsureCopied(prepTd, prepTdState, static_cast<UINT>(tdDesc.Width), tdDesc.Height, tdDesc.Format, true, L"VrAriseTail::Backdrop"))
				return false;
			CopyInto(cmd, prepTd.Get(), prepTdState, openTd[slot].Get());
			const wchar_t* colorNames[2]{ L"VrAriseTail::EyeColor0", L"VrAriseTail::EyeColor1" };
			const wchar_t* depthNames[2]{ L"VrAriseTail::EyeDepth0", L"VrAriseTail::EyeDepth1" };
			const wchar_t* motionNames[2]{ L"VrAriseTail::EyeMotion0", L"VrAriseTail::EyeMotion1" };
			const wchar_t* depthNNames[2]{ L"VrAriseTail::DepthN0", L"VrAriseTail::DepthN1" };
			const wchar_t* motionNNames[2]{ L"VrAriseTail::MotionN0", L"VrAriseTail::MotionN1" };
			for (int eye = 0; eye < 2; ++eye) {
				const auto colorDesc = openEyeColor[slot][eye]->GetDesc();
				const auto depthDesc = openEyeDepth[slot][eye]->GetDesc();
				const auto motionDesc = openEyeMotion[slot][eye]->GetDesc();
				const auto depthNDesc = openDepthN[slot][eye]->GetDesc();
				const auto motionNDesc = openMotionN[slot][eye]->GetDesc();
				if (!EnsureCopied(prepEyeColor[eye], prepEyeColorState[eye], static_cast<UINT>(colorDesc.Width), colorDesc.Height, colorDesc.Format, false, colorNames[eye]) ||
					!EnsureCopied(prepEyeDepth[eye], prepEyeDepthState[eye], static_cast<UINT>(depthDesc.Width), depthDesc.Height, depthDesc.Format, false, depthNames[eye]) ||
					!EnsureCopied(prepEyeMotion[eye], prepEyeMotionState[eye], static_cast<UINT>(motionDesc.Width), motionDesc.Height, motionDesc.Format, false, motionNames[eye]) ||
					!EnsureCopied(prepDepthN[eye], prepDepthNState[eye], static_cast<UINT>(depthNDesc.Width), depthNDesc.Height, depthNDesc.Format, false, depthNNames[eye]) ||
					!EnsureCopied(prepMotionN[eye], prepMotionNState[eye], static_cast<UINT>(motionNDesc.Width), motionNDesc.Height, motionNDesc.Format, false, motionNNames[eye]))
					return false;
				CopyInto(cmd, prepEyeColor[eye].Get(), prepEyeColorState[eye], openEyeColor[slot][eye].Get());
				CopyInto(cmd, prepEyeDepth[eye].Get(), prepEyeDepthState[eye], openEyeDepth[slot][eye].Get());
				CopyInto(cmd, prepEyeMotion[eye].Get(), prepEyeMotionState[eye], openEyeMotion[slot][eye].Get());
				CopyInto(cmd, prepDepthN[eye].Get(), prepDepthNState[eye], openDepthN[slot][eye].Get());
				CopyInto(cmd, prepMotionN[eye].Get(), prepMotionNState[eye], openMotionN[slot][eye].Get());
			}
			return true;
		}

		bool CopyImports(const SharedPublish& published)
		{
			bool timing = false;
			auto fail = [&](const char* why) {
				if (timing)
					VrPassTimer::Abort();
				lastFailReason = why;
				++failCount;
				return false;
			};
			if (!published.fenceValue || !queue || !copyList)
				return fail("no fence/queue");
			if (!ImportShared(published))
				return fail("ImportShared");
			// Previous use of these textures must finish before they are overwritten.
			// The comparable latency clock starts once that wait is over.
			if (!WaitFenceTimeout(fenceValue)) {
				logger::warn("[VrAriseTail] own fence late want={} done={} lastConsumed={}",
					fenceValue, fence ? fence->GetCompletedValue() : 0, lastConsumed);
				logNextSuccess = true;
				return fail("wait own prior-copy fence (Tail-side hitch)");
			}
			QueryPerformanceCounter(&spanStart);
			spanCopyEnd = {};
			spanRecordEnd = {};
			spanSubmitEnd = {};
			spanWaitMs = 0.0f;
			if (!EnsureColorFor(published.width, published.height, published.format))
				return fail("EnsureColorFor");
			if (FAILED(copyAllocator->Reset()) || FAILED(copyList->Reset(copyAllocator.Get(), nullptr)))
				return fail("copy allocator/list Reset");
			++passSerial;
			auto* cmd = copyList.Get();
			DrainQuery();
			VrPassTimer::Begin(device.Get(), queue.Get(), cmd, passSerial);
			timing = true;
			VrPassTimer::Mark(cmd, "VrDb::Copy");
			prepReady = false;
			if (published.prepared) {
				if (!CopyPrepared(cmd, published)) {
					cmd->Close();
					return fail("prep copy");
				}
				prepReady = true;
			} else {
				if (colorState != D3D12_RESOURCE_STATE_COPY_DEST) {
					Transition(cmd, color.Get(), colorState, D3D12_RESOURCE_STATE_COPY_DEST);
					colorState = D3D12_RESOURCE_STATE_COPY_DEST;
				}
				cmd->CopyResource(color.Get(), shared12[published.slot].Get());
			}
			hudReady = false;
			if (ImportHud(published)) {
				const D3D12_RESOURCE_DESC hudDesc = sharedHud12[published.slot]->GetDesc();
				if (EnsureGuide(hudLatch, hudLatchState, static_cast<UINT>(hudDesc.Width), hudDesc.Height,
						hudDesc.Format, L"VrAriseTail::HudLatch")) {
					if (hudLatchState != D3D12_RESOURCE_STATE_COPY_DEST) {
						Transition(cmd, hudLatch.Get(), hudLatchState, D3D12_RESOURCE_STATE_COPY_DEST);
						hudLatchState = D3D12_RESOURCE_STATE_COPY_DEST;
					}
					cmd->CopyResource(hudLatch.Get(), sharedHud12[published.slot].Get());
					Transition(cmd, hudLatch.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
					hudLatchState = D3D12_RESOURCE_STATE_COMMON;
					hudReady = true;
				}
			}
			if (!prepReady && FormatAllowsTypedUav(published.format) && EnsureOpaqueCs()) {
				VrPassTimer::Mark(cmd, "VrDb::Opaque");
				Transition(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				colorState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
				D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = UavTypedFormat(published.format);
				uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(color.Get(), nullptr, &uavDesc,
					uavHeap->GetCPUDescriptorHandleForHeapStart());
				ID3D12DescriptorHeap* heaps[]{ uavHeap.Get() };
				cmd->SetDescriptorHeaps(1, heaps);
				cmd->SetComputeRootSignature(opaqueRoot.Get());
				cmd->SetPipelineState(opaquePso.Get());
				cmd->SetComputeRootDescriptorTable(0, uavHeap->GetGPUDescriptorHandleForHeapStart());
				cmd->Dispatch((published.width + 7) / 8, (published.height + 7) / 8, 1);
				Transition(cmd, color.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			} else if (!prepReady) {
				Transition(cmd, color.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			}
			colorState = D3D12_RESOURCE_STATE_COMMON;
			guidesReady = false;
			if (published.hasGuides && ImportGuides(published) &&
				EnsureGuide(depth, depthState, published.guideWidth, published.guideHeight,
					published.depthFormat, L"VrAriseTail::Depth") &&
				EnsureGuide(mvec, mvecState, published.guideWidth, published.guideHeight,
					published.mvecFormat, L"VrAriseTail::Mvec")) {
				VrPassTimer::Mark(cmd, "VrDb::Guides");
				if (depthState != D3D12_RESOURCE_STATE_COPY_DEST) {
					Transition(cmd, depth.Get(), depthState, D3D12_RESOURCE_STATE_COPY_DEST);
					depthState = D3D12_RESOURCE_STATE_COPY_DEST;
				}
				if (mvecState != D3D12_RESOURCE_STATE_COPY_DEST) {
					Transition(cmd, mvec.Get(), mvecState, D3D12_RESOURCE_STATE_COPY_DEST);
					mvecState = D3D12_RESOURCE_STATE_COPY_DEST;
				}
				cmd->CopyResource(depth.Get(), sharedDepth12[published.slot].Get());
				cmd->CopyResource(mvec.Get(), sharedMvec12[published.slot].Get());
				Transition(cmd, depth.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				Transition(cmd, mvec.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				depthState = D3D12_RESOURCE_STATE_COMMON;
				mvecState = D3D12_RESOURCE_STATE_COMMON;
				guideWidth = published.guideWidth;
				guideHeight = published.guideHeight;
				depthFormat = published.depthFormat;
				mvecFormat = published.mvecFormat;
				guidesReady = true;
			}
			VrPassTimer::Mark(cmd, "VrDb::SubmitGap");
			if (FAILED(cmd->Close()))
				return fail("copy list Close");
			if (!openedSharedFence)
				return fail("queue wait D3D11 publish fence");
			ID3D12CommandList* lists[]{ cmd };
			HRESULT waitHr = S_OK;
			HRESULT signalHr = S_OK;
			const UINT64 signaled = fenceValue + 1;
			VrSharedDevice::WithQueue([&] {
				waitHr = queue->Wait(openedSharedFence.Get(), published.fenceValue);
				if (SUCCEEDED(waitHr)) {
					queue->ExecuteCommandLists(1, lists);
					signalHr = queue->Signal(fence.Get(), signaled);
				}
			});
			if (FAILED(waitHr))
				return fail("queue wait D3D11 publish fence");
			if (FAILED(signalHr))
				return fail("copy signal");
			fenceValue = signaled;
			lastConsumed = published.fenceValue;
			lastConsumedFence.store(lastConsumed, std::memory_order_relaxed);
			if (logNextSuccess) {
				logNextSuccess = false;
				logger::info("[VrAriseTail] CopyImports resumed consumed={} queueFence={} gen={} guides={}",
					lastConsumed, fenceValue, openedGeneration, guidesReady ? 1 : 0);
			}
			timing = false;
			QueryPerformanceCounter(&spanCopyEnd);
			return true;
		}

		// A free list, or the older one after it finishes. Two lists keep the next record off the readback wait.
		int PickGraphList()
		{
			auto free = [&](int index) {
				return graphBusy[index] == 0 || (fence && fence->GetCompletedValue() >= graphBusy[index]);
			};
			LARGE_INTEGER waitStart{};
			QueryPerformanceCounter(&waitStart);
			int index = 0;
			if (!free(0)) {
				if (free(1))
					index = 1;
				else {
					index = graphBusy[0] <= graphBusy[1] ? 0 : 1;
					WaitFenceTimeout(graphBusy[index]);
				}
			}
			LARGE_INTEGER waitEnd{};
			QueryPerformanceCounter(&waitEnd);
			LARGE_INTEGER freq{};
			QueryPerformanceFrequency(&freq);
			spanWaitMs = freq.QuadPart ?
				static_cast<float>(static_cast<double>(waitEnd.QuadPart - waitStart.QuadPart) * 1000.0 /
					static_cast<double>(freq.QuadPart)) :
				0.0f;
			return index;
		}

		void DrainQuery()
		{
			if (!pendingQueryFence || !fence || fence->GetCompletedValue() < pendingQueryFence)
				return;
			VrPassTimer::Report();
			pendingQueryFence = 0;
		}

		bool RecordDb(const SharedPublish& published)
		{
			auto fail = [&](const char* why) {
				VrPassTimer::Abort();
				lastFailReason = why;
				++failCount;
				return false;
			};
			const int graphSlot = PickGraphList();
			QueryPerformanceCounter(&spanRecordBegin);
			ID3D12CommandAllocator* graphAlloc = graphSlot ? graphAllocB.Get() : allocator.Get();
			ID3D12GraphicsCommandList* graphCmd = graphSlot ? graphListB.Get() : list.Get();
			if (FAILED(graphAlloc->Reset()) || FAILED(graphCmd->Reset(graphAlloc, nullptr)))
				return fail("allocator/list Reset");
			present = nullptr;
			stripColor = nullptr;
			stripDepth = nullptr;
			stripMotion = nullptr;
			strip = {};
			VrDx12Db::Output reconstructed{};
			VrDx12Db::Inputs dbIn{
				.list = graphCmd,
				.device = device.Get(),
				.queue = queue.Get(),
				.fence = fence.Get(),
				.fenceValue = fenceValue,
				.color = color.Get(),
				.colorState = &colorState,
				.depth = depth.Get(),
				.depthState = &depthState,
				.motion = mvec.Get(),
				.motionState = &mvecState,
				.guides = guidesReady,
				.colorOnly = published.colorOnly,
				.srcWidth = published.width,
				.srcHeight = published.height,
				.constants = published.constants,
				.constantsValid = published.constantsValid,
				.frameId = published.frameId,
				.prepared = prepReady,
				.backdrop = prepTd.Get(),
				.backdropState = &prepTdState,
			};
			if (prepReady) {
				for (int eye = 0; eye < 2; ++eye) {
					dbIn.eyeColor[eye] = prepEyeColor[eye].Get();
					dbIn.eyeDepth[eye] = prepEyeDepth[eye].Get();
					dbIn.eyeMotion[eye] = prepEyeMotion[eye].Get();
					dbIn.eyeDepthN[eye] = prepDepthN[eye].Get();
					dbIn.eyeMotionN[eye] = prepMotionN[eye].Get();
				}
			}
			const bool recorded = VrDx12Db::Record(dbIn, reconstructed);
			if (!recorded) {
				list->Close();
				return fail("record");
			}
			present = reconstructed.present;
			presentState = reconstructed.presentState;
			presentW = reconstructed.width;
			presentH = reconstructed.height;
			stripColor = reconstructed.stripColor;
			stripDepth = reconstructed.stripDepth;
			stripMotion = reconstructed.stripMotion;
			strip = reconstructed.strip;
			if (FAILED(graphCmd->Close()))
				return fail("list Close");
			QueryPerformanceCounter(&spanRecordEnd);
			ID3D12CommandList* lists[]{ graphCmd };
			HRESULT signalHr = S_OK;
			const UINT64 signaled = fenceValue + 1;
			VrSharedDevice::WithQueue([&] {
				queue->ExecuteCommandLists(1, lists);
				signalHr = queue->Signal(fence.Get(), signaled);
			});
			if (FAILED(signalHr))
				return fail("graph signal");
			fenceValue = signaled;
			prepareFenceValue = fenceValue;
			graphBusy[graphSlot] = signaled;
			if (pendingQueryFence == 0)
				pendingQueryFence = signaled;
			QueryPerformanceCounter(&spanSubmitEnd);
			return true;
		}
	};

	std::atomic<std::uint64_t> presentedPublish{ 0 };
	std::atomic<std::uint64_t> enginePublished{ 0 };
	std::atomic<std::uint64_t> rdbStarted{ 0 };
	std::mutex presentWaitMutex;
	std::condition_variable presentCv;

	void NotePresented(std::uint64_t publishFence)
	{
		// True Single's wait in ShouldSwallowEngineSubmit checks presentedPublish under this
		// same lock; without it, a notify racing a waiter's predicate check right before it
		// starts blocking can be missed (the store itself is still correctly ordered, only
		// the wakeup can be lost).
		std::lock_guard lock(presentWaitMutex);
		presentedPublish.store(publishFence, std::memory_order_release);
		presentCv.notify_all();
	}

	struct Doorbell
	{
		Dx12Path* dx12 = nullptr;
		ComPtr<ID3D12Resource> hudlessKeep[2];
		D3D12_RESOURCE_STATES keepState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		ComPtr<ID3D12Resource> keepDepth[2];
		ComPtr<ID3D12Resource> keepMvec[2];
		D3D12_RESOURCE_STATES keepDepthState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		D3D12_RESOURCE_STATES keepMvecState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		bool keepHasGuides[2]{};
		ComPtr<ID3D12Resource> keepBackdrop[2];
		D3D12_RESOURCE_STATES keepBackdropState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		ComPtr<ID3D12Resource> keepStrip[2];
		D3D12_RESOURCE_STATES keepStripState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		VrDx12Db::StripView keepView[2]{};
		UINT keepFullW = 0;
		UINT keepFullH = 0;
		ComPtr<ID3D12Resource> fullReal;
		ComPtr<ID3D12Resource> fullInterp;
		D3D12_RESOURCE_STATES fullRealState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES fullInterpState = D3D12_RESOURCE_STATE_COMMON;
		ComPtr<ID3D12CommandAllocator> paintAlloc;
		ComPtr<ID3D12GraphicsCommandList> paintList;
		ComPtr<ID3D12Fence> paintFence;
		UINT64 paintFenceValue = 0;
		HANDLE paintEvent = nullptr;
		VrPose::Body keepPose[2]{};
		ComPtr<ID3D12Resource> keepHud[2];
		D3D12_RESOURCE_STATES keepHudState[2]{
			D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON
		};
		bool keepHudValid[2]{};
		uint64_t keepPublishFence[2]{};
		uint64_t keepFenceValue[2]{};
		LARGE_INTEGER keepWallStart[2]{};
		std::uint64_t keepSerial[2]{};
		bool keepAsync[2]{};
		bool keepCamValid[2]{};
		float keepFrameMs[2]{ 11.11f, 11.11f };
		sl::Constants keepCam[2]{};
		UINT keepW = 0;
		bool keepColorOnly[2]{};
		UINT keepH = 0;
		UINT fgW = 0;
		UINT fgH = 0;
		UINT keepGuideW = 0;
		UINT keepGuideH = 0;
		DXGI_FORMAT keepDepthFmt = DXGI_FORMAT_R32_FLOAT;
		DXGI_FORMAT keepMvecFmt = DXGI_FORMAT_UNKNOWN;
		UINT publishedKeep = 0;
		std::atomic<uint32_t> kickSlot{ 1 };
		HANDLE snapshotWaitEvent = nullptr;
		std::thread worker;
		std::mutex kickMutex;
		std::condition_variable kickCv;
		std::atomic<bool> stop{ false };
		UINT reqW = 0;
		UINT reqH = 0;
		DXGI_FORMAT reqFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
		UINT width = 0;
		UINT height = 0;
		DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
		std::atomic<bool> ready{ false };
		bool failed = false;
		uint32_t kicks = 0;
		std::atomic<uint64_t> inputGen{ 0 };
		uint64_t takenGen = 0;
		bool gapNoted = false;
		const char* lastSnapshotFailReason = "";
		uint64_t snapshotFailCount = 0;

		~Doorbell()
		{
			stop.store(true, std::memory_order_release);
			kickCv.notify_all();
			if (worker.joinable())
				worker.join();
			if (snapshotWaitEvent) {
				CloseHandle(snapshotWaitEvent);
				snapshotWaitEvent = nullptr;
			}
			if (paintEvent) {
				CloseHandle(paintEvent);
				paintEvent = nullptr;
			}
		}

		bool EnsureSlot(ComPtr<ID3D12Resource>& tex, D3D12_RESOURCE_STATES& state, UINT w, UINT h,
			DXGI_FORMAT fmt, bool uav, const wchar_t* name)
		{
			if (tex) {
				const auto desc = tex->GetDesc();
				if (desc.Width == w && desc.Height == h && desc.Format == fmt)
					return true;
			}
			if (!CreateTex(tex, w, h, fmt, uav, name))
				return false;
			state = D3D12_RESOURCE_STATE_COMMON;
			return true;
		}

		bool CreateTex(ComPtr<ID3D12Resource>& out, UINT w, UINT h, DXGI_FORMAT fmt, bool uav,
			const wchar_t* name)
		{
			out.Reset();
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_DEFAULT;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			desc.Width = w;
			desc.Height = h;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.Format = fmt;
			desc.SampleDesc.Count = 1;
			if (uav)
				desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			if (FAILED(dx12->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&out))))
				return false;
			out->SetName(name);
			return true;
		}

		bool CreateKeep(UINT w, UINT h, DXGI_FORMAT fmt)
		{
			if (hudlessKeep[0] && keepW == w && keepH == h &&
				hudlessKeep[0]->GetDesc().Format == fmt)
				return true;
			for (int i = 0; i < 2; ++i) {
				if (!CreateTex(hudlessKeep[i], w, h, fmt, true, L"VrAriseTail::HudlessKeep"))
					return false;
				keepState[i] = D3D12_RESOURCE_STATE_COMMON;
				keepFenceValue[i] = 0;
			}
			keepW = w;
			keepH = h;
			return true;
		}

		bool CreateGuideKeep(UINT w, UINT h, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt)
		{
			if (keepDepth[0] && keepGuideW == w && keepGuideH == h &&
				keepDepthFmt == depthFmt && keepMvecFmt == mvecFmt)
				return true;
			for (int i = 0; i < 2; ++i) {
				if (!CreateTex(keepDepth[i], w, h, depthFmt, false, L"VrAriseTail::KeepDepth") ||
					!CreateTex(keepMvec[i], w, h, mvecFmt, false, L"VrAriseTail::KeepMvec"))
					return false;
				keepDepthState[i] = D3D12_RESOURCE_STATE_COMMON;
				keepMvecState[i] = D3D12_RESOURCE_STATE_COMMON;
				keepHasGuides[i] = false;
			}
			keepGuideW = w;
			keepGuideH = h;
			keepDepthFmt = depthFmt;
			keepMvecFmt = mvecFmt;
			return true;
		}

		bool Ensure(UINT sbsWidth, UINT sbsHeight, DXGI_FORMAT fmt)
		{
			if (ready.load(std::memory_order_acquire))
				return true;
			if (failed)
				return false;
			auto fail = [&](const char* why) {
				logger::warn("[VrAriseTail] X4 doorbell failed: {}", why);
				failed = true;
				return false;
			};
			if (!dx12 || !dx12->device || !dx12->queue || !dx12->adapter)
				return fail("no Tail D3D12");
			if (sbsWidth < 2 || sbsHeight == 0)
				return fail("SBS too small");

			if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
				fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)
				fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
			const DXGI_FORMAT engineFmt = fmt;
			fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

			VrBeat::BindGpu(dx12->device.Get(), dx12->queue.Get());
			if (!snapshotWaitEvent)
				snapshotWaitEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!snapshotWaitEvent)
				return fail("snapshot event");

			width = sbsWidth;
			height = sbsHeight;
			format = fmt;
			ready.store(true, std::memory_order_release);
			logger::info("[VrAriseTail] X4 SBS doorbell {}x{} engineFmt={} RGBA8",
				sbsWidth, sbsHeight, static_cast<unsigned>(engineFmt));
			return true;
		}

		bool EnsureGPath()
		{
			if (!dx12 || !dx12->device || !dx12->queue)
				return false;
			if (!globals::features::upscaling.fidelityFX.featureFSR3FG) {
				static bool logged = false;
				if (!logged) {
					logger::warn("[VrAriseTail] FSR-FG path: FidelityFX FG not bound");
					logged = true;
				}
				return false;
			}
			// Color-only loading copies the full shared slot and must not retarget FG.
			const UINT colorW = fgW ? fgW : (keepW ? keepW : width);
			const UINT colorH = fgH ? fgH : (keepH ? keepH : height);
			const UINT guideW = keepGuideW ? keepGuideW : colorW;
			const UINT guideH = keepGuideH ? keepGuideH : colorH;
			return VrFfxOp::Ensure(dx12->device.Get(), dx12->queue.Get(),
				colorW, colorH, guideW, guideH, format,
				DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT);
		}

		bool SnapshotStrip(ID3D12Resource* backdrop, UINT fullW, UINT fullH, DXGI_FORMAT fmt)
		{
			auto fail = [&](const char* why) {
				lastSnapshotFailReason = why;
				++snapshotFailCount;
				return false;
			};
			const auto& view = dx12->strip;
			if (!view.active || !dx12->stripColor || !dx12->stripDepth || !dx12->stripMotion || fmt != DXGI_FORMAT_R8G8B8A8_UNORM)
				return fail("strip missing");
			const UINT dest = 1u - kickSlot.load(std::memory_order_acquire);
			if (keepFenceValue[dest] && !dx12->WaitFenceTimeout(keepFenceValue[dest]))
				return fail("wait own prior-copy fence (Tail-side hitch)");
			if (!EnsureSlot(keepBackdrop[dest], keepBackdropState[dest], fullW, fullH, fmt, false, L"VrAriseTail::KeepBackdrop") ||
				!EnsureSlot(keepStrip[dest], keepStripState[dest], view.colorW, view.colorH, fmt, false, L"VrAriseTail::KeepStrip") ||
				!EnsureSlot(keepDepth[dest], keepDepthState[dest], view.guideW, view.guideH, dx12->depthFormat, false, L"VrAriseTail::KeepDepth") ||
				!EnsureSlot(keepMvec[dest], keepMvecState[dest], view.guideW, view.guideH, dx12->mvecFormat, false, L"VrAriseTail::KeepMvec"))
				return fail("strip keep");
			auto* alloc = dx12->snapAllocator[dest].Get();
			auto* cmd = dx12->snapList[dest].Get();
			if (FAILED(alloc->Reset()) || FAILED(cmd->Reset(alloc, nullptr)))
				return fail("allocator/list Reset");
			VrPassTimer::BeginKeep(dx12->device.Get(), cmd, dest);
			auto copyOne = [&](ID3D12Resource* src, ID3D12Resource* dst, D3D12_RESOURCE_STATES& dstState) {
				Transition(cmd, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
				if (dstState != D3D12_RESOURCE_STATE_COPY_DEST) {
					Transition(cmd, dst, dstState, D3D12_RESOURCE_STATE_COPY_DEST);
					dstState = D3D12_RESOURCE_STATE_COPY_DEST;
				}
				cmd->CopyResource(dst, src);
				Transition(cmd, src, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(cmd, dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				dstState = D3D12_RESOURCE_STATE_COMMON;
			};
			D3D12_RESOURCE_STATES& backdropState = (dx12->present && backdrop == dx12->present) ? dx12->presentState : dx12->colorState;
			if (backdropState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, backdrop, backdropState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				backdropState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (keepBackdropState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, keepBackdrop[dest].Get(), keepBackdropState[dest], D3D12_RESOURCE_STATE_COPY_DEST);
				keepBackdropState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			cmd->CopyResource(keepBackdrop[dest].Get(), backdrop);
			Transition(cmd, backdrop, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			backdropState = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, keepBackdrop[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			keepBackdropState[dest] = D3D12_RESOURCE_STATE_COMMON;
			copyOne(dx12->stripColor, keepStrip[dest].Get(), keepStripState[dest]);
			copyOne(dx12->stripDepth, keepDepth[dest].Get(), keepDepthState[dest]);
			copyOne(dx12->stripMotion, keepMvec[dest].Get(), keepMvecState[dest]);
			keepHasGuides[dest] = true;
			keepView[dest] = view;
			keepFullW = fullW;
			keepFullH = fullH;
			keepW = view.colorW;
			keepH = view.colorH;
			fgW = view.colorW;
			fgH = view.colorH;
			keepGuideW = view.guideW;
			keepGuideH = view.guideH;
			keepDepthFmt = dx12->depthFormat;
			keepMvecFmt = dx12->mvecFormat;
			CopyPairedHud(cmd, dest);
			VrPassTimer::FinishKeep(cmd, dest);
			if (FAILED(cmd->Close()))
				return fail("list Close");
			ID3D12CommandList* lists[]{ cmd };
			HRESULT signalHr = S_OK;
			const UINT64 signaled = dx12->fenceValue + 1;
			VrSharedDevice::WithQueue([&] {
				dx12->queue->ExecuteCommandLists(1, lists);
				signalHr = dx12->queue->Signal(dx12->fence.Get(), signaled);
			});
			if (FAILED(signalHr))
				return fail("snapshot signal");
			dx12->fenceValue = signaled;
			keepFenceValue[dest] = dx12->fenceValue;
			publishedKeep = dest;
			width = fullW;
			height = fullH;
			format = fmt;
			return true;
		}

		bool Snapshot(ID3D12Resource* color, UINT w, UINT h, DXGI_FORMAT fmt, bool colorOnly)
		{
			auto fail = [&](const char* why) {
				lastSnapshotFailReason = why;
				++snapshotFailCount;
				return false;
			};
			if (!color || !dx12 || !dx12->device)
				return fail("no color/device/guides yet");
			if (!colorOnly && (!dx12->guidesReady || !dx12->depth || !dx12->mvec))
				return fail("no color/device/guides yet");
			if (w < 2 || h == 0 || (!colorOnly && (dx12->guideWidth < 2 || dx12->guideHeight == 0)))
				return fail("size too small");
			if (!colorOnly && dx12->strip.active)
				return SnapshotStrip(color, w, h, fmt);
			constexpr DXGI_FORMAT kDoorbellFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
			D3D12_RESOURCE_STATES& colorTracked = (dx12->present && color == dx12->present) ? dx12->presentState : dx12->colorState;
			if (!CreateKeep(w, h, kDoorbellFmt) ||
				(!colorOnly && !CreateGuideKeep(dx12->guideWidth, dx12->guideHeight, dx12->depthFormat, dx12->mvecFormat)))
				return fail("CreateKeep/CreateGuideKeep");
			if (!colorOnly) {
				fgW = w;
				fgH = h;
			}
			if (fmt != kDoorbellFmt)
				return fail("format mismatch");
			const UINT dest = 1u - kickSlot.load(std::memory_order_acquire);
			const UINT64 destPriorFence = keepFenceValue[dest];
			if (!dx12->WaitFenceTimeout(destPriorFence))
				return fail("wait own prior-copy fence (Tail-side hitch)");
			ID3D12CommandAllocator* alloc = dx12->snapAllocator[dest].Get();
			ID3D12GraphicsCommandList* cmd = dx12->snapList[dest].Get();
			if (FAILED(alloc->Reset()) || FAILED(cmd->Reset(alloc, nullptr)))
				return fail("allocator/list Reset");
			VrPassTimer::BeginKeep(dx12->device.Get(), cmd, dest);
			if (colorTracked != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, color, colorTracked, D3D12_RESOURCE_STATE_COPY_SOURCE);
				colorTracked = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (keepState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, hudlessKeep[dest].Get(), keepState[dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			cmd->CopyResource(hudlessKeep[dest].Get(), color);
			keepHasGuides[dest] = false;
			keepView[dest] = {};
			if (colorOnly) {
				Transition(cmd, hudlessKeep[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				keepState[dest] = D3D12_RESOURCE_STATE_COMMON;
				Transition(cmd, color, colorTracked, D3D12_RESOURCE_STATE_COMMON);
				colorTracked = D3D12_RESOURCE_STATE_COMMON;
				CopyPairedHud(cmd, dest);
				VrPassTimer::FinishKeep(cmd, dest);
				if (FAILED(cmd->Close()))
					return fail("list Close");
				ID3D12CommandList* lists[]{ cmd };
				HRESULT signalHr = S_OK;
				const UINT64 signaled = dx12->fenceValue + 1;
				VrSharedDevice::WithQueue([&] {
					dx12->queue->ExecuteCommandLists(1, lists);
					signalHr = dx12->queue->Signal(dx12->fence.Get(), signaled);
				});
				if (FAILED(signalHr))
					return fail("snapshot signal");
				dx12->fenceValue = signaled;
				keepFenceValue[dest] = dx12->fenceValue;
				publishedKeep = dest;
				width = w;
				height = h;
				return true;
			}
			if (keepDepthState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, keepDepth[dest].Get(), keepDepthState[dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepDepthState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (keepMvecState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, keepMvec[dest].Get(), keepMvecState[dest],
					D3D12_RESOURCE_STATE_COPY_DEST);
				keepMvecState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			if (dx12->depthState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, dx12->depth.Get(), dx12->depthState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->depthState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (dx12->mvecState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, dx12->mvec.Get(), dx12->mvecState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->mvecState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			cmd->CopyResource(keepDepth[dest].Get(), dx12->depth.Get());
			cmd->CopyResource(keepMvec[dest].Get(), dx12->mvec.Get());
			Transition(cmd, hudlessKeep[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepState[dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, keepDepth[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepDepthState[dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, keepMvec[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST,
				D3D12_RESOURCE_STATE_COMMON);
			keepMvecState[dest] = D3D12_RESOURCE_STATE_COMMON;
			keepHasGuides[dest] = true;
			keepView[dest] = {};
			Transition(cmd, color, colorTracked, D3D12_RESOURCE_STATE_COMMON);
			colorTracked = D3D12_RESOURCE_STATE_COMMON;
			if (dx12->depthState != D3D12_RESOURCE_STATE_COMMON) {
				Transition(cmd, dx12->depth.Get(), dx12->depthState, D3D12_RESOURCE_STATE_COMMON);
				dx12->depthState = D3D12_RESOURCE_STATE_COMMON;
			}
			if (dx12->mvecState != D3D12_RESOURCE_STATE_COMMON) {
				Transition(cmd, dx12->mvec.Get(), dx12->mvecState, D3D12_RESOURCE_STATE_COMMON);
				dx12->mvecState = D3D12_RESOURCE_STATE_COMMON;
			}
			CopyPairedHud(cmd, dest);
			VrPassTimer::FinishKeep(cmd, dest);
			if (FAILED(cmd->Close()))
				return fail("list Close");
			ID3D12CommandList* lists[]{ cmd };
			HRESULT signalHr = S_OK;
			const UINT64 signaled = dx12->fenceValue + 1;
			VrSharedDevice::WithQueue([&] {
				dx12->queue->ExecuteCommandLists(1, lists);
				signalHr = dx12->queue->Signal(dx12->fence.Get(), signaled);
			});
			if (FAILED(signalHr))
				return fail("snapshot signal");
			dx12->fenceValue = signaled;
			keepFenceValue[dest] = dx12->fenceValue;
			publishedKeep = dest;
			width = w;
			height = h;
			return true;
		}

		/**
		 * Diagnostic-only: 09-19's crash left no crash-*.log/WER dump despite
		 * Crash Logger being installed, and Windows' own Event Log showed
		 * 0xc0000409 (fail-fast, not a normal AV) — consistent with an
		 * uncaught exception reaching std::terminate on one of Tail's threads.
		 * This cannot make that safe; it only names the exception first so
		 * the next occurrence doesn't require guessing again.
		 */
		void WorkerLoopImpl()
		{
			SetThreadDescription(GetCurrentThread(), L"VrAriseDoorbell");
			while (!stop.load(std::memory_order_acquire)) {
				MSG msg{};
				while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
					DispatchMessageW(&msg);
				if (!ready.load(std::memory_order_acquire) && !failed &&
					inputGen.load(std::memory_order_acquire) > 0)
					Ensure(reqW, reqH, reqFmt);
				VrSwitch::Tick();
				if (ready.load(std::memory_order_acquire) && VrSwitch::On())
					EnsureGPath();
				if (ready.load(std::memory_order_acquire)) {
					const uint64_t gen = inputGen.load(std::memory_order_acquire);
					// A jump means publishes were overwritten: one frame of motion vectors, several of pose.
					// Reset FG on the first jump only. Another reset per kick keeps the doorbell behind.
					if (gen != takenGen) {
						const bool skipped = takenGen != 0 && gen > takenGen + 1;
						if (skipped) {
							VrFfxOp::NoteDiscontinuity(!gapNoted);
							gapNoted = true;
						} else {
							gapNoted = false;
						}
						if (Kick())
							takenGen = gen;
						else {
							std::unique_lock lock(kickMutex);
							kickCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
								return stop.load(std::memory_order_acquire);
							});
						}
					} else {
						std::unique_lock lock(kickMutex);
						kickCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
							return stop.load(std::memory_order_acquire) ||
								inputGen.load(std::memory_order_acquire) != takenGen;
						});
					}
				} else {
					std::unique_lock lock(kickMutex);
					kickCv.wait_for(lock, std::chrono::milliseconds(2), [&] {
						return stop.load(std::memory_order_acquire) ||
							inputGen.load(std::memory_order_acquire) > 0;
					});
				}
			}
			VrFfxOp::Shutdown();
		}

		void WorkerLoop()
		{
			try {
				WorkerLoopImpl();
			} catch (const std::exception& e) {
				logger::critical("[VrAriseTail] Doorbell WorkerLoop uncaught exception: {}", e.what());
				throw;
			} catch (...) {
				logger::critical("[VrAriseTail] Doorbell WorkerLoop uncaught non-standard exception");
				throw;
			}
		}

		void CopyPairedHud(ID3D12GraphicsCommandList* cmd, UINT dest)
		{
			keepHudValid[dest] = false;
			if (!dx12 || !dx12->hudReady || !dx12->hudLatch)
				return;
			const D3D12_RESOURCE_DESC desc = dx12->hudLatch->GetDesc();
			const UINT latchedW = static_cast<UINT>(desc.Width);
			const UINT latchedH = desc.Height;
			if (!keepHud[dest] || keepHud[dest]->GetDesc().Width != latchedW ||
				keepHud[dest]->GetDesc().Height != latchedH || keepHud[dest]->GetDesc().Format != desc.Format) {
				keepHud[dest].Reset();
				D3D12_HEAP_PROPERTIES heap{};
				heap.Type = D3D12_HEAP_TYPE_DEFAULT;
				D3D12_RESOURCE_DESC created = desc;
				created.MipLevels = 1;
				created.SampleDesc.Count = 1;
				created.Flags = D3D12_RESOURCE_FLAG_NONE;
				if (FAILED(dx12->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &created,
						D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&keepHud[dest]))))
					return;
				keepHud[dest]->SetName(dest ? L"VrAriseTail::KeepHud1" : L"VrAriseTail::KeepHud0");
				keepHudState[dest] = D3D12_RESOURCE_STATE_COMMON;
			}
			if (dx12->hudLatchState != D3D12_RESOURCE_STATE_COPY_SOURCE) {
				Transition(cmd, dx12->hudLatch.Get(), dx12->hudLatchState, D3D12_RESOURCE_STATE_COPY_SOURCE);
				dx12->hudLatchState = D3D12_RESOURCE_STATE_COPY_SOURCE;
			}
			if (keepHudState[dest] != D3D12_RESOURCE_STATE_COPY_DEST) {
				Transition(cmd, keepHud[dest].Get(), keepHudState[dest], D3D12_RESOURCE_STATE_COPY_DEST);
				keepHudState[dest] = D3D12_RESOURCE_STATE_COPY_DEST;
			}
			cmd->CopyResource(keepHud[dest].Get(), dx12->hudLatch.Get());
			Transition(cmd, keepHud[dest].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			keepHudState[dest] = D3D12_RESOURCE_STATE_COMMON;
			Transition(cmd, dx12->hudLatch.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			dx12->hudLatchState = D3D12_RESOURCE_STATE_COMMON;
			keepHudValid[dest] = true;
		}

		bool OfferKick(ID3D12Resource* color, UINT w, UINT h, DXGI_FORMAT fmt, const VrPose::Body& pose, std::uint64_t publishFence, bool colorOnly, LARGE_INTEGER wallStart, std::uint64_t serial, bool asyncTail, const sl::Constants& cam, bool camValid, float frameMs)
		{
			reqW = w;
			reqH = h;
			reqFmt = fmt;
			if (!Snapshot(color, w, h, fmt, colorOnly)) {
				if (snapshotFailCount <= 8 || (snapshotFailCount % 60) == 0)
					logger::error("[VrAriseTail] Snapshot failed #{}: {}", snapshotFailCount, lastSnapshotFailReason);
				return false;
			}
			keepColorOnly[publishedKeep] = colorOnly;
			keepPose[publishedKeep] = pose;
			keepPublishFence[publishedKeep] = publishFence;
			keepWallStart[publishedKeep] = wallStart;
			keepSerial[publishedKeep] = serial;
			keepAsync[publishedKeep] = asyncTail;
			keepCam[publishedKeep] = cam;
			keepCamValid[publishedKeep] = camValid;
			keepFrameMs[publishedKeep] = frameMs;
			inputGen.fetch_add(1, std::memory_order_release);
			if (!asyncTail) {
				if (!ready.load(std::memory_order_acquire) && !Ensure(reqW, reqH, reqFmt))
					return false;
				VrSwitch::Tick();
				if (ready.load(std::memory_order_acquire) && VrSwitch::On())
					EnsureGPath();
				return Kick();
			}
			if (!worker.joinable()) {
				worker = std::thread([this] { WorkerLoop(); });
				logger::info("[VrAriseTail] async Snapshot+Kick; G or bridge at Kick");
			}
			kickCv.notify_one();
			return true;
		}

		// generated == nullptr skips the fullInterp half entirely (bridge/no-FG kick): only
		// fullReal gets built, from keepBackdrop+keepStrip, which never depended on FG's
		// output anyway.
		bool CompositePair(UINT slot, ID3D12Resource* generated)
		{
			const auto& view = keepView[slot];
			if (!view.active || !keepBackdrop[slot] || !keepStrip[slot] || !keepFullW || !keepFullH)
				return false;
			if (!EnsureSlot(fullReal, fullRealState, keepFullW, keepFullH, format, true, L"VrAriseTail::FullReal"))
				return false;
			if (generated && !EnsureSlot(fullInterp, fullInterpState, keepFullW, keepFullH, format, true, L"VrAriseTail::FullInterp"))
				return false;
			if (!paintAlloc) {
				if (FAILED(dx12->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&paintAlloc))) ||
					FAILED(dx12->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, paintAlloc.Get(), nullptr, IID_PPV_ARGS(&paintList))) ||
					FAILED(dx12->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&paintFence))))
					return false;
				paintList->Close();
				paintEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
				if (!paintEvent)
					return false;
			}
			if (paintFence->GetCompletedValue() < paintFenceValue) {
				if (FAILED(paintFence->SetEventOnCompletion(paintFenceValue, paintEvent)) ||
					WaitForSingleObject(paintEvent, INFINITE) != WAIT_OBJECT_0)
					return false;
			}
			if (FAILED(paintAlloc->Reset()) || FAILED(paintList->Reset(paintAlloc.Get(), nullptr)))
				return false;
			auto* cmd = paintList.Get();
			Transition(cmd, keepBackdrop[slot].Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(cmd, fullReal.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			if (generated)
				Transition(cmd, fullInterp.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			cmd->CopyResource(fullReal.Get(), keepBackdrop[slot].Get());
			if (generated)
				cmd->CopyResource(fullInterp.Get(), keepBackdrop[slot].Get());
			Transition(cmd, keepBackdrop[slot].Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(cmd, fullReal.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (generated)
				Transition(cmd, fullInterp.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			const UINT frameId = static_cast<UINT>(keepSerial[slot]);
			if (!VrDx12Db::PaintCenters(cmd, dx12->device.Get(), keepStrip[slot].Get(), fullReal.Get(), view, frameId, 0))
				return false;
			if (generated && !VrDx12Db::PaintCenters(cmd, dx12->device.Get(), generated, fullInterp.Get(), view, frameId, 4))
				return false;
			Transition(cmd, fullReal.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			if (generated)
				Transition(cmd, fullInterp.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			fullRealState = D3D12_RESOURCE_STATE_COMMON;
			if (generated)
				fullInterpState = D3D12_RESOURCE_STATE_COMMON;
			if (FAILED(cmd->Close()))
				return false;
			ID3D12CommandList* lists[]{ cmd };
			HRESULT signalHr = S_OK;
			const UINT64 signaled = paintFenceValue + 1;
			VrSharedDevice::WithQueue([&] {
				dx12->queue->ExecuteCommandLists(1, lists);
				signalHr = dx12->queue->Signal(paintFence.Get(), signaled);
			});
			if (FAILED(signalHr))
				return false;
			paintFenceValue = signaled;
			if (FAILED(paintFence->SetEventOnCompletion(paintFenceValue, paintEvent)) ||
				WaitForSingleObject(paintEvent, INFINITE) != WAIT_OBJECT_0)
				return false;
			return true;
		}

		bool Kick()
		{
			if (!ready.load(std::memory_order_acquire) || !dx12)
				return false;
			const UINT slot = publishedKeep;
			if (keepView[slot].active) {
				if (!keepStrip[slot] || !keepBackdrop[slot])
					return false;
			} else if (!hudlessKeep[slot])
				return false;
			// Claim before the fence wait so Snapshot writes the other keep
			// slot. Waiting first let the next Offer overwrite this slot.
			kickSlot.store(slot, std::memory_order_release);
			NotifyL2("OC_L2_NoteStarted");
			VrPassTimer::BindReportSerial(keepSerial[slot]);
			VrBeat::NoteSpan(keepWallStart[slot].QuadPart, keepSerial[slot], keepPublishFence[slot], !keepAsync[slot],
				VrSwitch::On(), keepFrameMs[slot]);
			if (keepFenceValue[slot] &&
				!WaitFenceCpu(dx12->fence.Get(), snapshotWaitEvent, keepFenceValue[slot])) {
				VrBeat::ClearSpan();
				return false;
			}
			VrPassTimer::ReportKeep(dx12->queue.Get(), slot);

			VrPoseAbs::LatchDraw(keepPose[slot]);
			VrFfxOp::NotePairedHud(keepHudValid[slot] ? keepHud[slot].Get() : nullptr);
			if (keepCamValid[slot]) {
				VrFfxOp::LatchedView view{};
				const sl::Constants& cam = keepCam[slot];
				view.right[0] = cam.cameraRight.x;
				view.right[1] = cam.cameraRight.y;
				view.right[2] = cam.cameraRight.z;
				view.up[0] = cam.cameraUp.x;
				view.up[1] = cam.cameraUp.y;
				view.up[2] = cam.cameraUp.z;
				view.fwd[0] = cam.cameraFwd.x;
				view.fwd[1] = cam.cameraFwd.y;
				view.fwd[2] = cam.cameraFwd.z;
				view.pos[0] = cam.cameraPos.x;
				view.pos[1] = cam.cameraPos.y;
				view.pos[2] = cam.cameraPos.z;
				view.jitterX = cam.jitterOffset.x;
				view.jitterY = cam.jitterOffset.y;
				view.nearZ = cam.cameraNear;
				view.farZ = cam.cameraFar;
				view.fov = cam.cameraFOV;
				view.frameMs = keepFrameMs[slot];
				VrFfxOp::NoteLatchedView(view);
			}
			// First Ensure() size is the engine SBS. A later TD keep is larger;
			// submitting the latched size CopyResource-crops the top-left into the HMD.
			const UINT colorW = keepW ? keepW : width;
			const UINT colorH = keepH ? keepH : height;
			static UINT loggedW = 0;
			static UINT loggedH = 0;
			if (colorW != loggedW || colorH != loggedH) {
				logger::info("[VrAriseTail] kick color {}x{} guide {}x{}",
					colorW, colorH, keepGuideW, keepGuideH);
				loggedW = colorW;
				loggedH = colorH;
			}
			bool ran = false;
			if (keepView[slot].active) {
				if (VrSwitch::On()) {
					ID3D12Resource* generated = nullptr;
					const auto& view = keepView[slot];
					ran = VrFfxOp::OnDbFinalColor(
						keepStrip[slot].Get(), keepDepth[slot].Get(), keepMvec[slot].Get(),
						view.colorW, view.colorH, view.guideW, view.guideH,
						format, keepDepthFmt, keepMvecFmt,
						view.mvecScaleX, view.mvecScaleY, false, &generated);
					if (ran && generated && CompositePair(slot, generated))
						ran = VrFfxOp::PresentComposited(fullReal.Get(), fullInterp.Get(), keepFullW, keepFullH, format);
					else
						ran = false;
				}
				// FG off, or the attempt above failed: the real frame alone still reaches
				// the beat. fullReal never depended on FG's output (it's built from
				// keepBackdrop+keepStrip), so CompositePair(slot, nullptr) just skips the
				// fullInterp half instead of needing a separate no-FG composite path.
				// Without this, turning vrG off left the metronome mailbox permanently
				// empty and the engine's first takeover wait never returned.
				if (!ran && CompositePair(slot, nullptr))
					ran = VrBridge::OnDbFinalColor(fullReal.Get(), keepFullW, keepFullH, format);
			} else if (VrSwitch::On()) {
				const bool colorOnly = keepColorOnly[slot];
				ran = VrFfxOp::OnDbFinalColor(
					hudlessKeep[slot].Get(),
					colorOnly ? nullptr : keepDepth[slot].Get(),
					colorOnly ? nullptr : keepMvec[slot].Get(),
					colorW, colorH,
					colorOnly ? 0 : keepGuideW,
					colorOnly ? 0 : keepGuideH,
					format,
					colorOnly ? DXGI_FORMAT_UNKNOWN : keepDepthFmt,
					colorOnly ? DXGI_FORMAT_UNKNOWN : keepMvecFmt);
			}
			if (!ran && !keepView[slot].active)
				ran = VrBridge::OnDbFinalColor(hudlessKeep[slot].Get(), colorW, colorH, format);
			if (!ran) {
				VrBeat::ClearSpan();
				return false;
			}
			++kicks;
			if (kicks <= 8 || (kicks % 120) == 0)
				logger::info("[VrAriseTail] {} kick={} slot={} {}x{}",
					VrSwitch::On() ? "FSR-FG" : "bridge", kicks, slot, colorW, colorH);
			return true;
		}
	};

	void ResolveSize(std::uint32_t& width, std::uint32_t& height)
	{
		width = 1512;
		height = 1512;
	}

	void TailLoopImpl()
	{
		SetThreadDescription(GetCurrentThread(), L"VrAriseTail");
		timeBeginPeriod(1);
		Dx12Path dx12;
		Doorbell doorbell;
		doorbell.dx12 = &dx12;
		bool haveDevice = false;
		bool loggedPath = false;
		bool beatStarted = false;
		bool modeLatched = false;
		bool asyncDoorbell = false;
		VrAriseTail::Status local{};
		local.active = true;
		local.path = "pending";

		while (running.load(std::memory_order_acquire) && !abandoned.load(std::memory_order_acquire)) {
			if (!haveDevice) {
				if (dx12.CreateDevice() && dx12.device) {
					haveDevice = true;
					local.path = "dx12";
					if (!modeLatched) {
						auto& foveated = globals::features::upscaling.foveatedRender;
						foveated.bootSnapshot.LatchIfNeeded(foveated.settings);
						asyncDoorbell = foveated.bootSnapshot.Boot(&FoveatedRender::Settings::ariseModel3);
						modeLatched = true;
						logger::info("[VrAriseTail] doorbell {}", asyncDoorbell ? "ARISE async" : "Tail-synchronous");
					}
				} else {
					Sleep(50);
					continue;
				}
			}

			if (!dx12.color) {
				std::uint32_t w = 0;
				std::uint32_t h = 0;
				ResolveSize(w, h);
				if (!dx12.EnsureColorFor(w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) {
					logger::warn("[VrAriseTail] color target {}x{} failed", w, h);
					Sleep(50);
					continue;
				}
				logger::info("[VrAriseTail] color target {}x{} path={}", w, h, local.path);
			}

			auto* compositor = RE::BSOpenVR::GetIVRCompositor();
			if (!compositor) {
				Sleep(100);
				continue;
			}
			SharedPublish published{};

			published = SnapshotShared();
			if (published.fenceValue > dx12.lastConsumed || published.generation != dx12.openedGeneration) {
				const bool copied = dx12.CopyImports(published);
				// Started means the copy is queued. The engine may already be drawing the next frame.
				rdbStarted.store(published.fenceValue, std::memory_order_release);
				presentCv.notify_all();
				if (copied) {
					if (!VrBeat::HasGpuMail())
						local.path = "dx12-ktemp";
					static bool loggedShared = false;
					if (!loggedShared) {
						logger::info("[VrAriseTail] X2 shared Submit {}x{} eye={}x{} fence={} slot={} guides={} {}x{}",
							published.width, published.height, published.width / 2, published.height,
							published.fenceValue, published.slot, dx12.guidesReady ? 1 : 0,
							dx12.guideWidth, dx12.guideHeight);
						loggedShared = true;
					}
					const bool graph = dx12.RecordDb(published);
					VrPassTimer::BindReportSerial(dx12.passSerial);
					if (dx12.spanSubmitEnd.QuadPart >= dx12.spanStart.QuadPart) {
						ReportQpc("VrArise::Copy", dx12.spanStart.QuadPart, dx12.spanCopyEnd.QuadPart);
						ReportQpc("VrArise::Record", dx12.spanRecordBegin.QuadPart, dx12.spanRecordEnd.QuadPart);
						ReportQpc("VrArise::Submit", dx12.spanRecordEnd.QuadPart, dx12.spanSubmitEnd.QuadPart);
						VrPassTimer::ReportSpan("VrArise::WaitGpu", dx12.spanWaitMs, false);
						VrPassTimer::ReportCpu("VrArise::WaitGpu", dx12.spanWaitMs, false);
					}
					bool kicked = false;
					if (graph && dx12.color && (dx12.guidesReady || published.colorOnly)) {
						ID3D12Resource* finalColor = dx12.present ? dx12.present : dx12.color.Get();
						std::uint32_t outW = dx12.present ? dx12.presentW : published.width;
						std::uint32_t outH = dx12.present ? dx12.presentH : published.height;
						DXGI_FORMAT outFormat = dx12.present ? DXGI_FORMAT_R8G8B8A8_UNORM : published.format;
						auto& perfMode = globals::features::upscaling.perfMode;
						const bool logicalEye = perfMode.IsLogicalEyeActive() &&
							perfMode.GetDisplayEyeWidth() && perfMode.GetDisplayEyeHeight();
						const UINT tdW = perfMode.GetDisplayEyeWidth() * 2;
						const UINT tdH = perfMode.GetDisplayEyeHeight();
						if (logicalEye && tdW && tdH && (outW != tdW || outH != tdH)) {
							static bool loggedKeep = false;
							if (!loggedKeep) {
								loggedKeep = true;
								logger::info("[VrAriseTail] keep TD {}x{}; ignoring {}x{}", tdW, tdH, outW, outH);
							}
						} else {
							kicked = doorbell.OfferKick(finalColor, outW, outH, outFormat, published.pose,
								published.fenceValue, published.colorOnly, dx12.spanStart, dx12.passSerial,
								asyncDoorbell, published.constants[0], published.constantsValid, published.frameMs);
						}
					}
					if (!kicked) {
						VrPassTimer::BindReportSerial(dx12.passSerial);
						VrPassTimer::ReportWallClock(TotalName(asyncDoorbell), dx12.spanStart.QuadPart, false);
					}
				} else {
					if (dx12.failCount <= 8 || (dx12.failCount % 60) == 0)
						logger::error("[VrAriseTail] X2 CopyImports failed #{}: {}",
							dx12.failCount, dx12.lastFailReason);
				}
			}
			if (!beatStarted) {
				VrBeat::Start();
				beatStarted = true;
			}

			if (!dx12.color) {
				Sleep(1);
				PublishStatus(local);
				continue;
			}

			const auto beatStatus = VrBeat::GetStatus();
			local.lastWaitMs = beatStatus.lastWaitMs;
			local.lastWaitErr = beatStatus.lastWaitErr;
			local.lastSubmitL = beatStatus.lastSubmitL;
			local.lastSubmitR = beatStatus.lastSubmitR;
			local.submits = beatStatus.submits;
			if (!loggedPath && beatStatus.firstSubmit) {
				logger::info("[VrAriseTail] metronome first Submit path={}", local.path);
				loggedPath = true;
			}
			PublishStatus(local);
			Sleep(1);
		}
		if (!VrBeat::HasFirstSubmit())
			AbandonTakeover();
		else
			VrBeat::Stop();
		timeEndPeriod(1);
	}

	/**
	 * Diagnostic-only wrapper: see WorkerLoopImpl's comment. Names the
	 * exception if this thread is where a future 0xc0000409 comes from.
	 */
	void TailLoop()
	{
		try {
			TailLoopImpl();
		} catch (const std::exception& e) {
			logger::critical("[VrAriseTail] TailLoop uncaught exception: {}", e.what());
			throw;
		} catch (...) {
			logger::critical("[VrAriseTail] TailLoop uncaught non-standard exception");
			throw;
		}
	}

	void StartWorker()
	{
		if (running.exchange(true))
			return;
		active.store(true, std::memory_order_release);
		VrAriseTail::Status pending{};
		pending.active = true;
		pending.path = "pending";
		PublishStatus(pending);
		logger::info("[VrAriseTail] Tail thread started");
		worker = std::thread(TailLoop);
		worker.detach();
	}
#endif
}

void VrAriseTail::Install()
{
#ifdef ENABLE_SKYRIM_VR
	if (!globals::game::isVR)
		return;
	if (!installed.exchange(true))
		InstallUpdatePosesHook();
#endif
}

bool VrAriseTail::IsActive()
{
#ifdef ENABLE_SKYRIM_VR
	return !abandoned.load(std::memory_order_acquire) &&
		(active.load(std::memory_order_acquire) || BootLatchedArise());
#else
	return false;
#endif
}

void VrAriseTail::NoteEngineSubmit()
{
#ifdef ENABLE_SKYRIM_VR
	if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire))
		return;
	if (vrReady.exchange(true, std::memory_order_acq_rel))
		return;
	logger::info("[VrAriseTail] vrReady on first engine Submit");
	StartWorker();
#endif
}

bool VrAriseTail::ShouldSwallowEngineSubmit()
{
#ifdef ENABLE_SKYRIM_VR
	if (!BootLatchedArise() || abandoned.load(std::memory_order_acquire))
		return false;
	NoteEngineSubmit();
	if (!VrBeat::HasFirstSubmit()) {
		while (!VrBeat::HasFirstSubmit() &&
			!abandoned.load(std::memory_order_acquire)) {
			if (!running.load(std::memory_order_acquire)) {
				logger::error("[VrAriseTail] Tail exited before first Submit; abandoning takeover");
				AbandonTakeover();
				return false;
			}
			if (!VrBeat::WaitForFirstSubmitMs(5000) && !VrBeat::HasFirstSubmit())
				logger::warn("[VrAriseTail] still waiting for metronome first Submit (OC session bind)");
		}
		if (!VrBeat::HasFirstSubmit() || abandoned.load(std::memory_order_acquire))
			return false;
	}
	// True Single: wait out this specific publish's full pipeline every frame instead of
	// only at session bind. enginePublished was just set by this frame's PublishKTemp,
	// ahead of this Submit call; presentedPublish catches up once Kick (or a failure path)
	// calls NotePresented for that same fence. Bounded retries, not an unconditional
	// INFINITE wait: a stuck doorbell must not hang the game forever.
	if (BootVrSingleBlock()) {
		const std::uint64_t fence = enginePublished.load(std::memory_order_acquire);
		std::unique_lock lock(presentWaitMutex);
		int attempt = 0;
		while (presentedPublish.load(std::memory_order_acquire) < fence &&
			!abandoned.load(std::memory_order_acquire) &&
			running.load(std::memory_order_acquire)) {
			const bool satisfied = presentCv.wait_for(lock, std::chrono::milliseconds(2000), [&] {
				return presentedPublish.load(std::memory_order_acquire) >= fence ||
					abandoned.load(std::memory_order_acquire) ||
					!running.load(std::memory_order_acquire);
			});
			if (!satisfied) {
				++attempt;
				logger::warn("[VrAriseTail] True Single: still waiting on publish {} after {} x 2s", fence, attempt);
				if (attempt >= 5) {
					logger::error("[VrAriseTail] True Single: giving up waiting on publish {}; releasing engine anyway", fence);
					break;
				}
			}
		}
	}
	return !abandoned.load(std::memory_order_acquire);
#else
	return false;
#endif
}

std::uint64_t VrAriseTail::LastPublishFence()
{
#ifdef ENABLE_SKYRIM_VR
	return lastPublishFence.load(std::memory_order_relaxed);
#else
	return 0;
#endif
}

std::uint64_t VrAriseTail::LastConsumedFence()
{
#ifdef ENABLE_SKYRIM_VR
	return lastConsumedFence.load(std::memory_order_relaxed);
#else
	return 0;
#endif
}

void VrAriseTail::WaitSpPresent()
{
	// L1 paces the engine at WaitGetPoses. Do not block the render hook.
}

void VrAriseTail::ReleaseEngine(std::uint64_t publishFence)
{
#ifdef ENABLE_SKYRIM_VR
	NotePresented(publishFence);
#else
	(void)publishFence;
#endif
}

bool VrAriseTail::HistoryResetPending()
{
#ifdef ENABLE_SKYRIM_VR
	return historyResetPending.load(std::memory_order_acquire);
#else
	return false;
#endif
}

void VrAriseTail::ClearHistoryReset()
{
#ifdef ENABLE_SKYRIM_VR
	historyResetPending.store(false, std::memory_order_release);
#endif
}

ID3D11Texture2D* ScaleLoadToTd(ID3D11Texture2D* src, UINT srcW, UINT srcH, UINT dstW, UINT dstH)
{
	static ComPtr<ID3D11Texture2D> dst;
	static ComPtr<ID3D11ShaderResourceView> srv;
	static ComPtr<ID3D11UnorderedAccessView> uav;
	static ID3D11Texture2D* lastSrc = nullptr;
	static UINT cachedW = 0;
	static UINT cachedH = 0;
	if (!src || !globals::d3d::device || srcW < 2 || !srcH || dstW < 2 || !dstH)
		return nullptr;
	if (srcW == dstW && srcH == dstH)
		return src;
	if (!dst || cachedW != dstW || cachedH != dstH) {
		D3D11_TEXTURE2D_DESC desc{};
		src->GetDesc(&desc);
		desc.Width = dstW;
		desc.Height = dstH;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		dst.Reset();
		uav.Reset();
		if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, dst.ReleaseAndGetAddressOf())))
			return nullptr;
		Util::SetResourceName(dst.Get(), "VrAriseTail::LoadTd");
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		if (FAILED(globals::d3d::device->CreateUnorderedAccessView(dst.Get(), &uavDesc, uav.ReleaseAndGetAddressOf())))
			return nullptr;
		cachedW = dstW;
		cachedH = dstH;
	}
	if (lastSrc != src) {
		D3D11_TEXTURE2D_DESC srcDesc{};
		src->GetDesc(&srcDesc);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = srcDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srv.Reset();
		if (FAILED(globals::d3d::device->CreateShaderResourceView(src, &srvDesc, srv.ReleaseAndGetAddressOf())))
			return nullptr;
		lastSrc = src;
	}
	FoveatedRenderImpl::Ops::StretchDRSBothEyes(uav.Get(), dstW / 2, dstH, srcW / 2, srcH, srcW, srcH, srv.Get(), 2, 1.0f, false);
	return dst.Get();
}

void VrAriseTail::PublishComposite()
{
#ifdef ENABLE_SKYRIM_VR
	auto* renderer = globals::game::renderer;
	if (!renderer)
		return;
	auto& targets = renderer->GetRuntimeData().renderTargets;
	const RE::RENDER_TARGET preferred[] = {
		RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY,
		RE::RENDER_TARGETS::kIMAGESPACE_TEMP_COPY2,
	};
	for (const auto slot : preferred) {
		auto* tex = Util::AsReal(targets[slot].texture);
		if (!tex)
			continue;
		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc(&desc);
		const bool unorm8 = desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
							desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
							desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
							desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
		if (desc.Width < 2 || !desc.Height || !unorm8)
			continue;
		PublishKTemp(tex);
		return;
	}
#endif
}

void VrAriseTail::PublishKTemp(ID3D11Texture2D* kTemp)
{
#ifdef ENABLE_SKYRIM_VR
	if (!kTemp || !BootLatchedArise() || abandoned.load(std::memory_order_acquire))
		return;
	bool colorOnly = false;
	ID3D11Texture2D* colorSrc = kTemp;
	const bool loading = globals::state && globals::state->isLoadingMenuOpen;
	if (loading) {
		if (!publishPausedForLoad.exchange(true))
			logger::info("[VrAriseTail] X4 publish paused (loading/no world)");
		publishResumeQpc.store(0, std::memory_order_release);
		colorOnly = true;
	}
	auto& perfMode = globals::features::upscaling.perfMode;
	D3D11_TEXTURE2D_DESC srcDesc{};
	kTemp->GetDesc(&srcDesc);
	// The shared slot is the engine input. True display is produced on DX12.
	// Resizing the slot resets its fence, so a mismatched source is scaled in.
	UINT dstW = srcDesc.Width;
	UINT dstH = srcDesc.Height;
	if (sharedWidth && sharedHeight) {
		dstW = sharedWidth;
		dstH = sharedHeight;
	} else if (perfMode.IsLogicalEyeActive() && perfMode.GetRenderEyeWidth() && perfMode.GetRenderEyeHeight()) {
		dstW = perfMode.GetRenderEyeWidth() * 2;
		dstH = perfMode.GetRenderEyeHeight();
	}
	if (srcDesc.Width != dstW || srcDesc.Height != dstH) {
		ID3D11Texture2D* scaled = ScaleLoadToTd(kTemp, srcDesc.Width, srcDesc.Height, dstW, dstH);
		if (!scaled) {
			NoteNoNewFrame(loading ? "loading" : "not slot");
			return;
		}
		static bool loggedScale = false;
		if (!loggedScale) {
			loggedScale = true;
			logger::info("[VrAriseTail] input {}x{} -> slot {}x{}",
				srcDesc.Width, srcDesc.Height, dstW, dstH);
		}
		colorSrc = scaled;
	}
	D3D11_TEXTURE2D_DESC desc{};
	colorSrc->GetDesc(&desc);
	if (!desc.Width || !desc.Height) {
		NoteNoNewFrame("empty size");
		return;
	}

	ComPtr<ID3D11Device5> device5;
	ComPtr<ID3D11DeviceContext4> context4;
	if (!globals::d3d::device || !globals::d3d::context ||
		FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(&device5))) ||
		FAILED(globals::d3d::context->QueryInterface(IID_PPV_ARGS(&context4)))) {
		if (!loggedPublishFail.exchange(true))
			logger::warn("[VrAriseTail] X2 D3D11 fence interfaces unavailable");
		return;
	}

	CS_GPU_PASS("VrAriseTail::PublishKTemp");

	UINT guideW = 0;
	UINT guideH = 0;
	DXGI_FORMAT mvecFmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D* mvecSrc = nullptr;
	ID3D11ShaderResourceView* depthSrv = nullptr;
	if (globals::game::renderer) {
		auto& depth = globals::game::renderer->GetDepthStencilData()
						  .depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& motion = globals::game::renderer->GetRuntimeData()
						   .renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		ComPtr<ID3D11Texture2D> depthTex;
		ComPtr<ID3D11Texture2D> mvecTex;
		if (depth.texture)
			Util::AsReal(depth.texture)->QueryInterface(IID_PPV_ARGS(&depthTex));
		if (motion.texture)
			Util::AsReal(motion.texture)->QueryInterface(IID_PPV_ARGS(&mvecTex));
		D3D11_TEXTURE2D_DESC depthDesc{};
		D3D11_TEXTURE2D_DESC mvecDesc{};
		if (depthTex && mvecTex && depth.depthSRV) {
			depthTex->GetDesc(&depthDesc);
			mvecTex->GetDesc(&mvecDesc);
			if (depthDesc.Width && depthDesc.Height &&
				depthDesc.Width == mvecDesc.Width && depthDesc.Height == mvecDesc.Height) {
				guideW = depthDesc.Width;
				guideH = depthDesc.Height;
				mvecFmt = mvecDesc.Format;
				mvecSrc = mvecTex.Get();
				depthSrv = Util::AsReal(depth.depthSRV);
			}
		}
	}
	if (!colorOnly && (!guideW || !guideH || !mvecSrc || !depthSrv)) {
		// Before the pause latch clears, so a guide miss cannot look like a resume.
		NoteNoNewFrame("guides");
		return;
	}
	const bool resumeReset = colorOnly ? false : publishPausedForLoad.exchange(false);
	if (resumeReset || publishResumeQpc.load(std::memory_order_acquire) == 0) {
		LARGE_INTEGER now{};
		QueryPerformanceCounter(&now);
		publishResumeQpc.store(now.QuadPart, std::memory_order_release);
	}
	if (resumeReset)
		logger::info("[VrAriseTail] X4 publish resumed");
	const UINT64 frame = globals::state ? globals::state->frameCount : 0;
	if (frame && publishedThisFrame.exchange(frame) == frame)
		return;
	const UINT64 prevFrame = lastPublishedFrame.load(std::memory_order_acquire);
	// Serial, not UpdatePoses. A stopped-then-resumed publish does not show up as count != 1.
	// Color-only loading leaves the pending flag set so the first world frame resets once.
	const bool serialGap = !colorOnly && (historyResetPending.exchange(false, std::memory_order_acq_rel) ||
		(prevFrame && frame && frame != prevFrame + 1));
	const bool historyReset = !colorOnly && (resumeReset || serialGap);
	if (historyReset)
		VrFfxOp::NoteDiscontinuity();
	sl::Constants constants[2]{};
	for (uint32_t eye = 0; eye < 2 && !colorOnly; ++eye) {
		if (!globals::features::upscaling.streamlineDX12.FillConstants(constants[eye], eye, historyReset, false)) {
			if (!loggedPublishFail.exchange(true))
				logger::error("[VrAriseTail] X4 publish refused: FillConstants eye={}", eye);
			NoteNoNewFrame("constants");
			return;
		}
		constants[eye].mvecScale = { 1.0f, 1.0f };
	}
	if (globals::state && globals::state->screenSize.y > 0.0f)
		constants[0].cameraAspectRatio = globals::state->screenSize.x / globals::state->screenSize.y;
	static bool loggedGConstants = false;
	if (!loggedGConstants) {
		loggedGConstants = true;
		logger::info("[VrAriseTail] X4 SBS G constants jitter=({:.3f},{:.3f}) color={}x{} aspect={:.3f} fmt={} ({}) guide={}x{}",
			constants[0].jitterOffset.x, constants[0].jitterOffset.y,
			desc.Width, desc.Height, constants[0].cameraAspectRatio,
			static_cast<unsigned>(desc.Format),
			desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ? "RGBA8" :
				desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? "RGB10A2" : "other",
			guideW, guideH);
	}

	std::unique_lock lock(sharedMutex);
	const UINT ensureGuideW = (colorOnly && sharedHasGuides) ? sharedGuideWidth : (colorOnly ? 0u : guideW);
	const UINT ensureGuideH = (colorOnly && sharedHasGuides) ? sharedGuideHeight : (colorOnly ? 0u : guideH);
	const DXGI_FORMAT ensureMvec = (colorOnly && sharedHasGuides) ? sharedMvecFormat : (colorOnly ? DXGI_FORMAT_UNKNOWN : mvecFmt);
	if (!EnsureSharedSlots(device5.Get(), desc.Width, desc.Height, desc.Format,
			ensureGuideW, ensureGuideH, ensureMvec)) {
		if (!loggedPublishFail.exchange(true))
			logger::warn("[VrAriseTail] X2 shared slot create failed {}x{} fmt={}",
				desc.Width, desc.Height, static_cast<unsigned>(desc.Format));
		return;
	}
	const UINT slot = static_cast<UINT>(sharedFenceValue % kSharedSlots);
	if (!colorOnly && (!sharedHasGuides || !sharedDepthRtv[slot] || !sharedMvec11[slot])) {
		if (!loggedPublishFail.exchange(true))
			logger::error("[VrAriseTail] X4 publish refused: shared guides not created");
		return;
	}
	lock.unlock();
	auto* hudSrc = globals::game::renderer ?
					   Util::AsReal(globals::game::renderer->GetRuntimeData()
							.renderTargets[RE::RENDER_TARGETS::kMENUBG]
							.texture) :
					   nullptr;
	if (hudSrc) {
		D3D11_TEXTURE2D_DESC menuDesc{};
		hudSrc->GetDesc(&menuDesc);
		if (menuDesc.Width >= 2 && menuDesc.Height &&
			(!sharedHud11[slot] || sharedHudW != menuDesc.Width || sharedHudH != menuDesc.Height ||
				sharedHudFmt != menuDesc.Format)) {
			for (UINT i = 0; i < kSharedSlots; ++i) {
				if (sharedHudHandle[i]) {
					CloseHandle(sharedHudHandle[i]);
					sharedHudHandle[i] = nullptr;
				}
				sharedHud11[i].Reset();
				if (!CreateSharedTex(device5.Get(), menuDesc.Width, menuDesc.Height, menuDesc.Format, 0,
						sharedHud11[i], sharedHudHandle[i], i ? "VrAriseTail::HudSlot1" : "VrAriseTail::HudSlot0"))
					break;
			}
			sharedHudW = menuDesc.Width;
			sharedHudH = menuDesc.Height;
			sharedHudFmt = menuDesc.Format;
		}
		if (sharedHud11[slot])
			context4->CopyResource(sharedHud11[slot].Get(), hudSrc);
	}
	if (!colorOnly)
		context4->CopyResource(sharedMvec11[slot].Get(), mvecSrc);
	auto& upscaling = globals::features::upscaling;
	auto* vs = upscaling.GetUpscaleVS();
	auto* ps = upscaling.copyDepthToSharedBufferPS.Get(
		L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0");
	if (!colorOnly && (!vs || !ps)) {
		if (!loggedPublishFail.exchange(true))
			logger::error("[VrAriseTail] X4 publish refused: depth blit shaders missing");
		return;
	}
	if (!colorOnly) {
		Util::FullscreenPassScope stateScope(globals::d3d::context);
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(guideW);
		viewport.Height = static_cast<float>(guideH);
		viewport.MaxDepth = 1.0f;
		globals::d3d::context->RSSetViewports(1, &viewport);
		globals::d3d::context->IASetInputLayout(nullptr);
		globals::d3d::context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		globals::d3d::context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		globals::d3d::context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		globals::d3d::context->VSSetShader(vs, nullptr, 0);
		globals::d3d::context->PSSetShader(ps, nullptr, 0);
		if (upscaling.upscaleRasterizerState)
			globals::d3d::context->RSSetState(upscaling.upscaleRasterizerState.get());
		if (upscaling.upscaleBlendState)
			globals::d3d::context->OMSetBlendState(upscaling.upscaleBlendState.get(), nullptr, 0xffffffff);
		ID3D11ShaderResourceView* views[]{ depthSrv };
		globals::d3d::context->PSSetShaderResources(0, 1, views);
		ID3D11RenderTargetView* rtvs[]{ sharedDepthRtv[slot].Get() };
		globals::d3d::context->OMSetRenderTargets(1, rtvs, nullptr);
		globals::d3d::context->Draw(3, 0);
	}
	const bool prepared = !colorOnly && PrepareEngineEdge(globals::d3d::device, slot, colorSrc,
		desc.Width, desc.Height, desc.Format, guideW, guideH, mvecFmt);
	if (!prepared)
		context4->CopyResource(sharedTex11[slot].Get(), colorSrc);
	lock.lock();
	++sharedFenceValue;
	if (FAILED(context4->Signal(sharedFence11.Get(), sharedFenceValue))) {
		if (!loggedPublishFail.exchange(true))
			logger::warn("[VrAriseTail] X2 fence Signal failed");
		return;
	}
	lastPublishedFrame.store(frame, std::memory_order_release);
	lastPublishFence.store(sharedFenceValue, std::memory_order_relaxed);
	enginePublished.store(sharedFenceValue, std::memory_order_release);
	NotifyL2("OC_L2_NoteProduced");
	EndNoNewFrame();
	sharedPublished.fenceValue = sharedFenceValue;
	sharedPublished.slot = slot;
	sharedPublished.generation = sharedGeneration;
	sharedPublished.width = desc.Width;
	sharedPublished.height = desc.Height;
	sharedPublished.format = desc.Format;
	sharedPublished.hasGuides = !colorOnly;
	sharedPublished.colorOnly = colorOnly;
	sharedPublished.guideWidth = guideW;
	sharedPublished.guideHeight = guideH;
	sharedPublished.depthFormat = DXGI_FORMAT_R32_FLOAT;
	sharedPublished.mvecFormat = mvecFmt;
	sharedPublished.pose.valid = VrBeat::CopyEngineDrawPose(sharedPublished.pose.m);
	{
		float frameMs = 11.11f;
		if (globals::game::deltaTime && std::isfinite(*globals::game::deltaTime))
			frameMs = std::clamp(*globals::game::deltaTime * 1000.0f, 1.0f, 100.0f);
		sharedPublished.frameMs = frameMs;
	}
	sharedPublished.hudHandle = sharedHud11[slot] ? sharedHudHandle[slot] : nullptr;
	sharedConstants[0] = constants[0];
	sharedConstants[1] = constants[1];
	sharedConstantsValid = true;
	sharedPublished.constants[0] = constants[0];
	sharedPublished.constants[1] = constants[1];
	sharedPublished.constantsValid = !colorOnly;
	sharedPublished.frameId = frame;
	sharedPublished.prepared = prepared;
	sharedPublished.prepGeneration = prepGeneration;
	sharedPublished.prepTd = nullptr;
	for (int eye = 0; eye < 2; ++eye) {
		sharedPublished.prepEyeColor[eye] = nullptr;
		sharedPublished.prepEyeDepth[eye] = nullptr;
		sharedPublished.prepEyeMotion[eye] = nullptr;
		sharedPublished.prepDepthN[eye] = nullptr;
		sharedPublished.prepMotionN[eye] = nullptr;
	}
	if (prepared) {
		const auto& prep = prepSlots[slot];
		sharedPublished.prepTd = prep.td.handle;
		for (int eye = 0; eye < 2; ++eye) {
			sharedPublished.prepEyeColor[eye] = prep.eyeColor[eye].handle;
			sharedPublished.prepEyeDepth[eye] = prep.eyeDepth[eye].handle;
			sharedPublished.prepEyeMotion[eye] = prep.eyeMotion[eye].handle;
			sharedPublished.prepDepthN[eye] = prep.depthN[eye].handle;
			sharedPublished.prepMotionN[eye] = prep.motionN[eye].handle;
		}
	}
	if (!loggedPublish.exchange(true))
		logger::info("[VrAriseTail] X4 published kTEMP {}x{} + guides {}x{} mvecFmt={} slot={}",
			desc.Width, desc.Height, guideW, guideH, static_cast<unsigned>(mvecFmt), slot);
#else
	(void)kTemp;
#endif
}

VrAriseTail::Status VrAriseTail::GetStatus()
{
	std::scoped_lock lock(statusMutex);
	auto copy = status;
	copy.active = IsActive();
	if (!copy.active)
		copy.path = "off";
	return copy;
}
