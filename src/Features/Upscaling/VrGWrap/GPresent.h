#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include "../VrPose/Body.h"

#	include <cstdint>
#	include <d3d12.h>
#	include <dxgi1_4.h>

/**
 * Dead DLSS-G Present wrapper. Ensure always returns false.
 */
namespace VrGWrap
{
	bool Ensure(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGIAdapter* adapter,
		UINT width, UINT height, DXGI_FORMAT format);

	IDXGISwapChain3* SwapChain();
	ID3D12Resource* BackBuffer(UINT index);
	bool Ready();
	void Pump();
	void Shutdown();
	void BeginKickPresent(const VrPose::Body& pose);
}

#endif
