#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include <openvr.h>

namespace VrPose
{
	/** CPU HMD 3×4. Not a GPU CBV. */
	struct Body
	{
		vr::HmdMatrix34_t m{};
		bool valid = false;
	};
}

#endif
