#pragma once

#include <d3d12.h>
#include <sl.h>

struct ID3D12Device;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace VrDx12Db
{
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
	};

	struct Output
	{
		ID3D12Resource* present = nullptr;
		D3D12_RESOURCE_STATES presentState = D3D12_RESOURCE_STATE_COMMON;
		UINT width = 0;
		UINT height = 0;
	};

	/**
	 * @brief Records DX12 DLSS SR, neural rendering, the full-eye stretch, and the
	 * center embed on the caller's open command list. The engine image stays where
	 * it was copied; the true-display result is `Output::present`.
	 */
	bool Record(const Inputs& in, Output& out);
}
