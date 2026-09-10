#pragma once

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include <directx/d3dx12.h>

#include "NeuralRendering/Runtime.h"
#include "NeuralRendering/AriseReconstructionGraph.h"

class WrappedResource
{
public:
	// a_name follows Util::SetResourceName's "Feature::Name" convention; views get " SRV"/
	// " UAV"/" RTV" suffixes. Pass empty to leave resources unnamed.
	WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device, const std::string& a_name = {});
	~WrappedResource();

	ID3D11Texture2D* resource11 = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	winrt::com_ptr<ID3D12Resource> resource;
};

struct SharedFence
{
	static constexpr DWORD kRemovalPollMs = 100;
	winrt::com_ptr<ID3D12Fence> fence12;
	winrt::com_ptr<ID3D11Fence> fence11;
	uint64_t value = 0;

	/** @brief Returns the next value to signal, advancing the monotonic counter. */
	uint64_t Next() { return ++value; }
	/** @brief Creates and names the fence; throws on failure without leaking the NT handle. */
	void Create(ID3D12Device* a_device12, ID3D11Device5* a_device11, const char* a_name);
	/** @brief Waits on the CPU up to a_timeoutMs, polling device removal via fence12's own device.
	 *  Trivially true when the fence is unset or a_value is 0 (nothing to wait for). */
	bool CpuWait(uint64_t a_value, DWORD a_timeoutMs) const;
};

struct DXGISwapChainProxy : IDXGISwapChain4
{
public:
	DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);

	IDXGISwapChain4* swapChain;

	/****IUnknown****/
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
	virtual ULONG STDMETHODCALLTYPE AddRef() override;
	virtual ULONG STDMETHODCALLTYPE Release() override;

	/****IDXGIObject****/
	virtual HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown) override;
	virtual HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent) override;

	/****IDXGIDeviceSubObject****/
	virtual HRESULT STDMETHODCALLTYPE GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice) override;

	/****IDXGISwapChain****/
	virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface) override;
	virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget) override;
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget) override;
	virtual HRESULT STDMETHODCALLTYPE GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc) override;
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) override;
	virtual HRESULT STDMETHODCALLTYPE ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters) override;
	virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput) override;
	virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats) override;
	virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(_Out_ UINT* pLastPresentCount) override;

	/****IDXGISwapChain1****/
	virtual HRESULT STDMETHODCALLTYPE GetDesc1(_Out_ DXGI_SWAP_CHAIN_DESC1* pDesc) override;
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenDesc(_Out_ DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
	virtual HRESULT STDMETHODCALLTYPE GetHwnd(_Out_ HWND* pHwnd) override;
	virtual HRESULT STDMETHODCALLTYPE GetCoreWindow(_In_ REFIID refiid, _COM_Outptr_ void** ppUnk) override;
	virtual HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, _In_ const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
	virtual BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
	virtual HRESULT STDMETHODCALLTYPE GetRestrictToOutput(_COM_Outptr_ IDXGIOutput** ppRestrictToOutput) override;
	virtual HRESULT STDMETHODCALLTYPE SetBackgroundColor(_In_ const DXGI_RGBA* pColor) override;
	virtual HRESULT STDMETHODCALLTYPE GetBackgroundColor(_Out_ DXGI_RGBA* pColor) override;
	virtual HRESULT STDMETHODCALLTYPE SetRotation(_In_ DXGI_MODE_ROTATION Rotation) override;
	virtual HRESULT STDMETHODCALLTYPE GetRotation(_Out_ DXGI_MODE_ROTATION* pRotation) override;

	/****IDXGISwapChain2****/
	virtual HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
	virtual HRESULT STDMETHODCALLTYPE GetSourceSize(_Out_ UINT* pWidth, _Out_ UINT* pHeight) override;
	virtual HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
	virtual HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(_Out_ UINT* pMaxLatency) override;
	virtual HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
	virtual HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
	virtual HRESULT STDMETHODCALLTYPE GetMatrixTransform(_Out_ DXGI_MATRIX_3X2_F* pMatrix) override;

	/****IDXGISwapChain3****/
	virtual UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
	virtual HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(_In_ DXGI_COLOR_SPACE_TYPE ColorSpace, _Out_ UINT* pColorSpaceSupport) override;
	virtual HRESULT STDMETHODCALLTYPE SetColorSpace1(_In_ DXGI_COLOR_SPACE_TYPE ColorSpace) override;
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format, UINT SwapChainFlags, _In_reads_(BufferCount) const UINT* pCreationNodeMask, _In_reads_(BufferCount) IUnknown* const* ppPresentQueue) override;

	/****IDXGISwapChain4****/
	virtual HRESULT STDMETHODCALLTYPE SetHDRMetaData(_In_ DXGI_HDR_METADATA_TYPE Type, UINT Size, _In_reads_bytes_opt_(Size) void* pMetaData) override;
};

class DX12SwapChain
{
public:
	struct AriseSceneCapture
	{
		std::uint64_t frameId = 0;
		std::uint32_t guideWidth = 0;
		std::uint32_t guideHeight = 0;
		NeuralRendering::AriseReconstructionGraph::Plan plan{};
	};

	struct AriseStatistics
	{
		std::uint64_t logicalPresents = 0;
		std::uint64_t physicalPresents = 0;
		std::uint64_t captured = 0;
		std::uint64_t reconstructed = 0;
		std::uint64_t fallback = 0;
		std::uint64_t uiComposites = 0;
		std::uint64_t backpressure = 0;
		std::uint64_t failures = 0;
		HRESULT lastPresentResult = S_OK;
		bool tailNrAvailable = false;
		bool dlssAvailable = false;
		bool renderDocCompatibility = false;
		bool asyncTail = false;
		NeuralRendering::AriseReconstructionGraph::Statistics graph{};
		std::string graphPath;
	};

	winrt::com_ptr<ID3D12Device> d3d12Device;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;

	// 4x DLSS-G generates 3 interpolated frames per real frame; the SL pacer needs those
	// plus 2 slots of its own slack to avoid oversubscribing the flip-model chain.
	static constexpr UINT kMaxBackBuffers = 5;

	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[kMaxBackBuffers];
	winrt::com_ptr<ID3D12GraphicsCommandList4> commandLists[kMaxBackBuffers];

	IDXGISwapChain4* swapChain;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;
	DXGI_SWAP_CHAIN_DESC1 logicalSwapChainDesc;

	WrappedResource* swapChainBufferWrapped;
	WrappedResource* uiBufferWrapped;

	// D3D12 interop resources for frame generation
	WrappedResource* depthBufferShared12 = nullptr;
	WrappedResource* motionVectorBufferShared12 = nullptr;

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	SharedFence interopFence;

	winrt::com_ptr<ID3D12Resource> swapChainBuffers[kMaxBackBuffers];

	UINT frameIndex = 0;

	UINT64 frameFenceValues[kMaxBackBuffers] = {};

	LARGE_INTEGER qpf;

	double refreshRate = 0;

	DXGISwapChainProxy* swapChainProxy = nullptr;

	bool useDLSSG = false;
	bool useArise = false;

	// Actual buffer count the live swap chain was created/resized with (<= kMaxBackBuffers).
	UINT backBufferCount = 2;

	// Returns the current frame time (in seconds) for accurate FPS calculation when frame generation is active
	float GetFrameTime() const;

	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	void CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateSwapChainDirect(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateSwapChainArise(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc,
		bool asyncTail, bool enableDLSSG);

	void CreateInterop();
	/** @brief (Re)creates the D3D11/D3D12-shared swap-chain and UI buffer textures at (re)size time. */
	void RecreateWrappedResources(const DXGI_SWAP_CHAIN_DESC1& desc);

	DXGISwapChainProxy* GetSwapChainProxy();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	/** @brief IDXGISwapChain::GetBuffer equivalent for the wrapped D3D11 swap-chain buffer. Only buffer index 0 is supported. */
	HRESULT GetBuffer(UINT buffer, REFIID riid, void** ppSurface);
	/** @brief IDXGISwapChain::ResizeBuffers equivalent; rejects any bufferCount differing from the chain's own backBufferCount. */
	HRESULT ResizeBuffers(UINT bufferCount, UINT width, UINT height, DXGI_FORMAT format, UINT flags);
	HRESULT Present(UINT SyncInterval, UINT Flags);
	/** @brief Publishes post-LDR guides for the current Model 3 frame before UI rendering begins. */
	void CaptureAriseScene(ID3D11Resource* depth, ID3D11Resource* motionVectors,
		const AriseSceneCapture& capture);
	/** @brief Drains the old Model 3 plan and rebuilds temporal runtime state before a new capture is published. */
	bool PrepareAriseGraphPlanChange();
	void ClearAriseSceneCapture();
	[[nodiscard]] AriseStatistics GetAriseStatistics() const;
	HRESULT GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice);
	HANDLE GetFrameLatencyWaitableObject();

	void SetColorSpace(bool enableHDR);

	// Resources needed by BackgroundBlur when D3D12 swap chain is active
	struct BlurResources
	{
		ID3D11Texture2D* backbufferTex = nullptr;
		ID3D11RenderTargetView* backbufferRTV = nullptr;
		ID3D11ShaderResourceView* backbufferSRV = nullptr;
		ID3D11ShaderResourceView* uiBufferSRV = nullptr;
		ID3D11RenderTargetView* uiBufferRTV = nullptr;
	};

	// Get all resources needed for background blur in one call
	BlurResources GetBlurResources() const;

	// D3D12 interop resource management
	void CreateSharedResources();
	/** @brief Waits until Model 3 tail workers and submitted GPU work are idle. */
	bool DrainModel3Tail();
	[[nodiscard]] UINT PhysicalWidth() const { return swapChainDesc.Width; }
	[[nodiscard]] UINT PhysicalHeight() const { return swapChainDesc.Height; }
	[[nodiscard]] UINT LogicalWidth() const { return logicalSwapChainDesc.Width; }
	[[nodiscard]] UINT LogicalHeight() const { return logicalSwapChainDesc.Height; }
	/** @brief Keep engine render extents on logical R. Window client size stays physical D. */
	void ApplyLogicalRenderExtents() const;
	[[nodiscard]] bool IsRenderDocCompatibility() const { return ariseRenderDocCompatibility; }
	[[nodiscard]] bool IsAriseAsyncTail() const { return ariseAsyncTail; }
	/**
	 * @brief Reconstructed Display D + UI after the Tail graph (not the DXGI backbuffer).
	 * @param holder Keeps the D3D11 texture alive for the caller.
	 * @param waitForIdle When true, drains in-flight Tail work before the D3D11 copy.
	 */
	ID3D11Texture2D* GetScreenshotComposite(winrt::com_ptr<ID3D11Texture2D>& holder, bool waitForIdle);

private:
	enum class AriseSlotState : std::uint8_t
	{
		kWritable,
		kQueued,
		kConsuming,
		kSubmitted,
	};

	struct ArisePresentSlot
	{
		AriseSlotState state = AriseSlotState::kWritable;
		std::unique_ptr<WrappedResource> sealedScene;
		std::unique_ptr<WrappedResource> sealedUI;
		std::unique_ptr<WrappedResource> displayUI;
		std::unique_ptr<WrappedResource> sealedDepth;
		std::unique_ptr<WrappedResource> sealedMotionVectors;
		AriseSceneCapture capture{};
		bool captureValid = false;
		winrt::com_ptr<ID3D12CommandAllocator> allocator;
		winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;
		UINT64 engineReady = 0;
		UINT64 tailDone = 0;
		UINT syncInterval = 0;
		UINT flags = 0;
		std::uint32_t frameId = 0;
	};

	void CreateArisePresentResources();
	HRESULT CreateAriseCompositeResources();
	bool ReleaseArisePresentResources();
	HRESULT PresentArise(UINT syncInterval, UINT flags);
	HRESULT ExecuteArisePresent(std::size_t slotIndex);
	bool RecordAriseComposite(ArisePresentSlot& slot, ID3D12Resource* scene,
		ID3D12Resource* physicalBuffer, UINT backBufferIndex);
	bool SealAriseCapture(ArisePresentSlot& slot, std::uint64_t presentFrameId);
	void ResetScreenshotComposite();
	bool RecordScreenshotComposite(ID3D12GraphicsCommandList* commandList);
	bool WaitForAriseTail(UINT64 value);
	bool WaitForAriseIdle();
	void ReapAriseSlotsLocked();
	void LatchAriseFailure(HRESULT result, const char* operation);
	void EnterAriseSynchronousFallback(HRESULT result, const char* operation);
	void UpdateLogicalSwapChainDesc();

	static constexpr std::size_t kAriseSlotCount = 3;
	static constexpr std::chrono::milliseconds kAriseWaitTimeout{ 2000 };
	std::array<ArisePresentSlot, kAriseSlotCount> ariseSlots;
	NeuralRendering::AriseReconstructionGraph ariseReconstructionGraph;
	winrt::com_ptr<ID3D12RootSignature> ariseCompositeRootSignature;
	winrt::com_ptr<ID3D12PipelineState> ariseCompositePipelineState;
	winrt::com_ptr<ID3D12PipelineState> ariseUiScalePipelineState;
	winrt::com_ptr<ID3D12DescriptorHeap> ariseCompositeSrvHeap;
	winrt::com_ptr<ID3D12DescriptorHeap> ariseCompositeRtvHeap;
	winrt::com_ptr<ID3D11Fence> ariseEngineFence11;
	winrt::com_ptr<ID3D12Fence> ariseEngineFence12;
	winrt::com_ptr<ID3D12Fence> ariseTailFence12;
	winrt::com_ptr<ID3D11Fence> ariseTailFence11;
	std::unique_ptr<WrappedResource> ariseScreenshotComposite;
	mutable std::mutex ariseMutex;
	std::condition_variable ariseCondition;
	std::timed_mutex arisePhysicalPresentMutex;
	std::size_t ariseProducerCursor = 0;
	UINT64 ariseEngineValue = 0;
	UINT64 ariseTailValue = 0;
	std::atomic_bool ariseFatalFailure = false;
	std::atomic_bool ariseSynchronousFallback = false;
	std::atomic_bool ariseTailNrAvailable = false;
	std::atomic_bool ariseGraphAvailable = false;
	bool ariseRenderDocCompatibility = false;
	bool ariseAsyncTail = true;
	std::atomic_uint64_t ariseLogicalPresents = 0;
	std::atomic_uint64_t arisePhysicalPresents = 0;
	std::atomic_uint64_t ariseCaptured = 0;
	std::atomic_uint64_t ariseReconstructed = 0;
	std::atomic_uint64_t ariseFallback = 0;
	std::atomic_uint64_t ariseUiComposites = 0;
	std::atomic_uint64_t ariseBackpressure = 0;
	std::atomic_uint64_t ariseFailures = 0;
	std::atomic_long ariseLastPresentResult = S_OK;
	std::unique_ptr<WrappedResource> arisePendingDepth;
	std::unique_ptr<WrappedResource> arisePendingMotionVectors;
	AriseSceneCapture arisePendingCapture{};
	std::uint64_t arisePendingPresentSequence = 0;
	bool arisePendingCaptureValid = false;
};
