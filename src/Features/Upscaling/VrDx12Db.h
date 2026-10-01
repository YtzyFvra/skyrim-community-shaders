#pragma once

#include <d3d12.h>
#include <sl.h>

struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace VrDx12Db
{
	/**
	 * @brief Crop, true-display, and NR sizes shared by the engine prep and the doorbell.
	 * `srReady` is the geometry only; the doorbell still requires guides and constants.
	 */
	struct EdgePlan
	{
		UINT srcWidth = 0;
		UINT srcHeight = 0;
		UINT eyeW = 0;
		UINT eyeH = 0;
		UINT tdEyeW = 0;
		UINT tdEyeH = 0;
		UINT tdW = 0;
		UINT tdH = 0;
		UINT cropX[2]{};
		UINT cropY[2]{};
		UINT cropW = 0;
		UINT cropH = 0;
		UINT subInW = 0;
		UINT subInH = 0;
		UINT nrW = 0;
		UINT nrH = 0;
		UINT act1W = 0;
		UINT act1H = 0;
		UINT displayW = 0;
		UINT displayH = 0;
		UINT depthFilter = 0;
		float uvX[2]{};
		float uvY[2]{};
		float uvW[2]{};
		float uvH[2]{};
		// Padded crop in eye pixels. Nasal side grows by 5% when the eye has room.
		UINT srcX[2]{};
		UINT srcW[2]{};
		bool srReady = false;
	};

	/** @brief Where the stitched center splits back onto the true-display backdrop. */
	struct StripView
	{
		bool active = false;
		UINT srcOffsetX[2]{};
		UINT dstX[2]{};
		UINT dstY[2]{};
		UINT boxW = 0;
		UINT boxH = 0;
		UINT colorW = 0;
		UINT colorH = 0;
		UINT guideW = 0;
		UINT guideH = 0;
		float mvecScaleX = 0.0f;
		float mvecScaleY = 0.0f;
	};

	/** @brief Fills `out` from the live subrect and the logical-eye display size. */
	bool ComputeEdgePlan(UINT srcWidth, UINT srcHeight, EdgePlan& out);

	struct Inputs
	{
		ID3D12GraphicsCommandList* list = nullptr;
		ID3D12Device* device = nullptr;
		ID3D12CommandQueue* queue = nullptr;
		ID3D12Fence* fence = nullptr;
		UINT64 fenceValue = 0;
		ID3D12Resource* color = nullptr;
		D3D12_RESOURCE_STATES* colorState = nullptr;
		ID3D12Resource* depth = nullptr;
		D3D12_RESOURCE_STATES* depthState = nullptr;
		ID3D12Resource* motion = nullptr;
		D3D12_RESOURCE_STATES* motionState = nullptr;
		bool guides = false;
		bool colorOnly = false;
		UINT srcWidth = 0;
		UINT srcHeight = 0;
		const sl::Constants* constants = nullptr;
		bool constantsValid = false;
		UINT64 frameId = 0;
		// Engine already cropped, stretched the backdrop, and filtered NR guides.
		bool prepared = false;
		ID3D12Resource* backdrop = nullptr;
		D3D12_RESOURCE_STATES* backdropState = nullptr;
		ID3D12Resource* eyeColor[2]{};
		ID3D12Resource* eyeDepth[2]{};
		ID3D12Resource* eyeMotion[2]{};
		ID3D12Resource* eyeDepthN[2]{};
		ID3D12Resource* eyeMotionN[2]{};
	};

	struct Output
	{
		ID3D12Resource* present = nullptr;
		D3D12_RESOURCE_STATES presentState = D3D12_RESOURCE_STATE_COMMON;
		UINT width = 0;
		UINT height = 0;
		// RGBA8 center strip after the single reconstruction. Depth and motion stay at engine size.
		ID3D12Resource* stripColor = nullptr;
		ID3D12Resource* stripDepth = nullptr;
		ID3D12Resource* stripMotion = nullptr;
		StripView strip{};
	};

	/**
	 * @brief Records DX12 DLSS SR, neural rendering, the full-eye stretch, and the
	 * center embed on the caller's open command list. The engine image stays where
	 * it was copied; the true-display result is `Output::present`.
	 */
	bool Record(const Inputs& in, Output& out);

	/**
	 * @brief Writes the two original eye boxes from a stitched center onto a backdrop.
	 * The nasal pad stays in the source and is not written. `dest` must already be UAV.
	 */
	bool PaintCenters(ID3D12GraphicsCommandList* list, ID3D12Device* device,
		ID3D12Resource* source, ID3D12Resource* dest, const StripView& strip, uint32_t frameId,
		UINT tableSlot = 0);
}
