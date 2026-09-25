#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include "../Streamline.h"

#	include <cstdint>
#	include <d3d12.h>

/**
 * Dead DLSS-G simulated-compositor gate. Ensure always returns false.
 */
namespace VrSimGate
{
	struct DbFrame
	{
		ID3D12Resource* color = nullptr;
		ID3D12Resource* depth = nullptr;
		ID3D12Resource* mvec = nullptr;
		ID3D12Resource* ui = nullptr;
		sl::Constants constants{};
		UINT colorW = 0;
		UINT colorH = 0;
		UINT guideW = 0;
		UINT guideH = 0;
		DXGI_FORMAT colorFmt = DXGI_FORMAT_R8G8B8A8_UNORM;
		DXGI_FORMAT depthFmt = DXGI_FORMAT_R32_FLOAT;
		DXGI_FORMAT mvecFmt = DXGI_FORMAT_UNKNOWN;
		bool enableG = false;
	};

	bool Ensure(ID3D12Device* device, ID3D12CommandQueue* queue,
		UINT colorW, UINT colorH, UINT guideW, UINT guideH,
		DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt);

	bool OnDbFinalColor(const DbFrame& frame);

	void Shutdown();
}

#endif
