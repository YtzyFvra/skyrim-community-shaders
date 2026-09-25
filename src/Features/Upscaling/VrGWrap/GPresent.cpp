#include "PCH.h"
#include "GPresent.h"

#ifdef ENABLE_SKYRIM_VR

bool VrGWrap::Ensure(ID3D12Device*, ID3D12CommandQueue*, IDXGIAdapter*,
	UINT, UINT, DXGI_FORMAT)
{
	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::warn("[VrGWrap] VR DLSS-G gate shorted");
	}
	return false;
}

IDXGISwapChain3* VrGWrap::SwapChain()
{
	return nullptr;
}

ID3D12Resource* VrGWrap::BackBuffer(UINT)
{
	return nullptr;
}

bool VrGWrap::Ready()
{
	return false;
}

void VrGWrap::BeginKickPresent(const VrPose::Body&)
{
}

void VrGWrap::Pump()
{
}

void VrGWrap::Shutdown()
{
}

#endif
