#pragma once

#include <cstdint>
#include <dxgiformat.h>

struct ID3D11Texture2D;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12Resource;

namespace sl
{
	struct Constants;
}

/**
 * VR booster tail. The game thread seals kTEMP and returns. This thread records
 * ACT1, NR, and ACT2 on a DX12 command list, then hands the HMD-sized result to
 * FG or Submit. It does not take the desktop DXGI proxy.
 */
namespace VrBoosterTail
{
	bool UseAsyncTail();
	void NoteSealed(std::uint32_t frame);
	bool OwnsSubmit();
	void PreparePresent(std::uint32_t width, std::uint32_t height);
	ID3D11Texture2D* PresentTexture();

	ID3D12Resource* Reconstruct(ID3D12Device* device, ID3D12CommandQueue* queue,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* mvec,
		std::uint32_t width, std::uint32_t height,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		DXGI_FORMAT colorFormat, DXGI_FORMAT depthFormat, DXGI_FORMAT motionFormat,
		const sl::Constants* eyeConstants,
		std::uint32_t& outWidth, std::uint32_t& outHeight, DXGI_FORMAT& outFormat);
}
