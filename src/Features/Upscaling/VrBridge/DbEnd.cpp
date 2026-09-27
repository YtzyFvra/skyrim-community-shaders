#include "PCH.h"
#include "DbEnd.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../VrBeat/Metronome.h"
#	include "../VrPose/Absolute.h"

bool VrBridge::OnDbFinalColor(ID3D12Resource* color, UINT width, UINT height, DXGI_FORMAT format)
{
	if (!color || !width || !height)
		return false;
	VrPoseAbs::StampReady();
	const VrPose::Body pose = VrPoseAbs::Ready();
	if (!VrBeat::CopySbs(color, width, height, format, pose.m, pose.valid, VrBeat::NextPair()))
		return false;
	return true;
}

#endif
