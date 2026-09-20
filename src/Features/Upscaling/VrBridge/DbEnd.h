#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include <d3d12.h>

/**
 * Bridge: the other DB end. Connects the absolute pose controller
 * to the metronome. G chain stays off.
 */
namespace VrBridge
{
	/**
	 * @brief DB finished this draw. Absolute pose marks first, then
	 * copy color + that pose into the metronome together.
	 */
	bool OnDbFinalColor(ID3D12Resource* color, UINT width, UINT height, DXGI_FORMAT format);
}

#endif
