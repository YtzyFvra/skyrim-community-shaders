#pragma once

#include <cstdint>
#include <d3d11.h>

namespace NeuralRendering
{
	struct Act2BackendStats
	{
		std::uint64_t nisSucceeded = 0;
		std::uint64_t dlssSucceeded = 0;
		std::uint64_t fsrSucceeded = 0;
	};

	/** VR: Post kTEMP 8-pass then owned Submit. Flat: PostInject pipeline. */
	bool ApplyFoveatedLdr();
	/** Render eye from kTEMP, display eye from OpenVR recommended or kTOTAL. */
	bool GetVrOwnedEyeSizes(uint32_t& renderW, uint32_t& renderH, uint32_t& displayW, uint32_t& displayH);
	/** Private SBS after a successful owned submit this session; null before the first frame. */
	ID3D11Texture2D* GetVrOwnedComposite(ID3D11ShaderResourceView** outSrv = nullptr);
	Act2BackendStats GetAct2BackendStats();
	/** Dumps G / I / N / RC / Lo / Gain / EditLow / D of the next Guidance frame as raw float32 for offline band analysis. */
	void RequestBandCapture();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}