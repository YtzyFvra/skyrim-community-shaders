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

	/** Flat: PostInject pipeline after engine Post. VR waits for `ApplyVrAfterTonemap`. */
	bool ApplyFoveatedLdr();
	/** VR: after ISHDR Tonemap, read kTEMP, run per-eye 8-pass, Submit ourselves. */
	bool ApplyVrAfterTonemap();
	/** Owned SBS: both eyes from kTEMP half-eye (TD = E on this cut). */
	bool GetVrOwnedEyeSizes(uint32_t& renderW, uint32_t& renderH, uint32_t& displayW, uint32_t& displayH);
	/** Private SBS packed after a successful owned Submit; null if that frame fell back. */
	ID3D11Texture2D* GetVrOwnedComposite(ID3D11ShaderResourceView** outSrv = nullptr);
	/** One E×E owned eye; null if that frame fell back to engine Submit. */
	ID3D11Texture2D* GetVrOwnedEye(uint32_t eye);
	Act2BackendStats GetAct2BackendStats();
	/** Dumps G / I / N / RC / Lo / Gain / EditLow / D of the next Guidance frame as raw float32 for offline band analysis. */
	void RequestBandCapture();

	/** Releases all runtime and shared-resource state. */
	void Reset();
}