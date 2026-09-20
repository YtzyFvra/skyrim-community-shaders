#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include "../Streamline.h"

#	include <cstdint>
#	include <d3d12.h>

/**
 * Simulated-compositor gate. G only sees this ready set.
 * Absolute pose marks before Copy. P counts immediately before Present.
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

	/** @brief Bind GPU and create the one ready set. */
	bool Ensure(ID3D12Device* device, ID3D12CommandQueue* queue,
		UINT colorW, UINT colorH, UINT guideW, UINT guideH,
		DXGI_FORMAT colorFmt, DXGI_FORMAT depthFmt, DXGI_FORMAT mvecFmt);

	/**
	 * @brief DB finished final color: copy into ready, tag the token, Present.
	 */
	bool OnDbFinalColor(const DbFrame& frame);

	void Shutdown();
}

#endif
