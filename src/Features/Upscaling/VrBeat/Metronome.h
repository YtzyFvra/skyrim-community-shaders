#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include <openvr.h>

#	include <cstdint>
#	include <d3d12.h>

/**
 * Producer thread: copies SBS into a mailbox and enqueues via OpenVR Submit.
 * OC L3 owns Wait/Begin/End. Does not own G, Kick, or pairing.
 */
namespace VrBeat
{
	/** Latest picture. One GPU buffer; writers overwrite that same buffer. */
	struct Frame
	{
		ID3D12Resource* left12 = nullptr;
		ID3D12Resource* right12 = nullptr;
		ID3D12CommandQueue* queue12 = nullptr;
		bool sbs = false;
		std::uint64_t pair = 0;
	};

	struct Status
	{
		bool running = false;
		bool ownsWait = false;
		bool firstSubmit = false;
		double lastWaitMs = 0.0;
		std::uint64_t submits = 0;
		std::uint32_t waits = 0;
		int lastWaitErr = 0;
		int lastSubmitL = 0;
		int lastSubmitR = 0;
	};

	/** @brief Start the metronome thread. Idempotent. */
	void Start();

	/** @brief Stop the beat and drop Wait ownership. */
	void Stop();

	/** @brief Bind the D3D12 queue used to copy into the metronome buffer. */
	void BindGpu(ID3D12Device* device, ID3D12CommandQueue* queue);

	/** @brief New Kick id so an interpolated offer only pairs with its own real. */
	std::uint64_t NextPair();

	/**
	 * @brief Overwrite the one buffer and the owned pose together.
	 */
	bool CopySbs(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
		const vr::HmdMatrix34_t& pose, bool poseValid, std::uint64_t pair);

	/** @brief True after at least one CopySbs. */
	bool HasGpuMail();

	/** @brief Last copy-fence wait on the doorbell queue, in milliseconds. */
	double LastCopyFenceMs();

	/**
	 * @brief Copy the interpolated SBS for this Kick pair. Beat enqueues it
	 * before the real only when the pair ids match.
	 */
	bool OfferGInterpolated(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
		const vr::HmdMatrix34_t& pose, std::uint64_t pair);

	/** @brief Drop a pending interpolated offer (real CopySbs failed). */
	void CancelInterp();

	/** @brief True after the first owned Submit (OC session bind). */
	bool HasFirstSubmit();

	/** @brief True while this thread owns WaitGetPoses. */
	bool OwnsWait();

	/**
	 * @brief Hook IVRCompositor::WaitGetPoses/GetLastPoses as a passthrough snoop.
	 * Always calls the original; never substitutes a different query. Off the
	 * beat thread, also copies the HMD pose OpenVR just handed the engine.
	 * Idempotent; safe to call every UpdatePoses.
	 */
	void HookCompositorPoses();

	/** @brief HMD pose OpenVR handed the engine on its own last Wait/GetLastPoses. */
	bool CopyEngineDrawPose(vr::HmdMatrix34_t& out);

	/** @brief Block until first Submit or timeout. */
	bool WaitForFirstSubmitMs(unsigned ms);

	Status GetStatus();
}

#endif
