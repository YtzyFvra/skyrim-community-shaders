#include "PCH.h"
#include "PresentGate.h"

#ifdef ENABLE_SKYRIM_VR

bool VrSimGate::Ensure(ID3D12Device*, ID3D12CommandQueue*,
	UINT, UINT, UINT, UINT,
	DXGI_FORMAT, DXGI_FORMAT, DXGI_FORMAT)
{
	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::warn("[VrSimGate] VR DLSS-G gate shorted");
	}
	return false;
}

bool VrSimGate::OnDbFinalColor(const DbFrame&)
{
	static bool logged = false;
	if (!logged) {
		logged = true;
		logger::warn("[VrSimGate] VR DLSS-G gate shorted");
	}
	return false;
}

void VrSimGate::Shutdown()
{
}

#endif
