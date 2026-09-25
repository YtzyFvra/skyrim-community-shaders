#include "PCH.h"
#include "DbEnd.h"

#ifdef ENABLE_SKYRIM_VR

#	include "../VrAriseTail.h"
#	include "../VrBeat/Metronome.h"
#	include "../VrPose/Absolute.h"

#	include <cmath>

bool VrBridge::OnDbFinalColor(ID3D12Resource* color, UINT width, UINT height, DXGI_FORMAT format)
{
	if (!color || !width || !height)
		return false;
	VrPoseAbs::StampReady();
	const VrPose::Body pose = VrPoseAbs::Ready();
	if (!VrBeat::CopySbs(color, width, height, format, pose.m, pose.valid, VrBeat::NextPair()))
		return false;
	static std::uint64_t n = 0;
	++n;
	// Timing-based diagnostics (fence waits, drops, slot alternation) came
	// back clean through a whole session that still juddered -- log the
	// actual declared numbers instead, dense enough (every 6th, ~15/s at
	// 90fps) to see a few seconds of frame-to-frame continuity around
	// whenever judder is noticed. Yaw uses OpenVR's column layout (col 2 =
	// -forward); a healthy sequence should read as smoothly changing, not
	// jumping/flipping between consecutive printed lines.
	if (n <= 8 || (n % 6) == 0) {
		const float yawDeg = std::atan2(pose.m.m[0][2], pose.m.m[2][2]) * 57.29578f;
		// Doorbell health folded into the same line so a before/after log diff around
		// a judder onset shows pose continuity and pipeline drops/fence stalls together
		// without needing a separate devbench poll timed to the moment it happens.
		const VrAriseTail::Status st = VrAriseTail::GetStatus();
		logger::info("[VrBridge] CopySbs n={} TwP={} pos=({:.3f},{:.3f},{:.3f}) yaw={:.1f}deg slot={} snapWait={:.2f} kickWait={:.2f} dropped={}",
			n, pose.valid ? 1 : 0, pose.m.m[0][3], pose.m.m[1][3], pose.m.m[2][3], yawDeg,
			st.lastKeepSlot, st.lastSnapshotWaitMs, st.lastKickWaitMs, st.droppedGens);
	}
	return true;
}

#endif
