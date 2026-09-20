#pragma once

#ifdef ENABLE_SKYRIM_VR

#	include <openvr.h>

#	include <cstdint>
#	include <d3d12.h>

/**
 * Our Wait+Submit metronome. Reads Tex+matrix from its own mailbox.
 * Does not own G, Kick, or pairing.
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

	/** @brief Overwrite the mailbox picture. Pose is submitPose, not this. */
	void Publish(const Frame& frame);

	/** @brief Bind the D3D12 queue used to copy into the metronome buffer. */
	void BindGpu(ID3D12Device* device, ID3D12CommandQueue* queue);

	/**
	 * @brief Overwrite the one buffer. Pose unchanged.
	 * Always the same resource; no ping-pong.
	 */
	bool CopySbs(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format);

	/**
	 * @brief Overwrite the one buffer and the owned pose together.
	 */
	bool CopySbs(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
		const vr::HmdMatrix34_t& pose, bool poseValid);

	/** @brief True after at least one CopySbs. */
	bool HasGpuMail();

	/**
	 * @brief G's interpolated frame, delivered on its own dedicated
	 * compositor period instead of sharing the reactive mailbox the real
	 * frame uses. Without this, the interpolated frame's color is always
	 * overwritten by the real frame that follows it moments later on the
	 * same Kick before the (purely reactive, once-per-engine-Wait) beat
	 * loop ever gets a chance to Submit it -- G pays its own per-Kick GPU
	 * cost but the interpolated frame never reaches the compositor, so the
	 * headset never sees genuine 2x (09-19 22:52 measurement: VDXR fps
	 * actually *dropped* with G on, pure overhead with no realized frame).
	 * Copies into a separate buffer immediately (safe to call from
	 * whichever thread G's native Present hook fires on); the beat thread
	 * picks it up on its next wake and gives it its own WaitGetPoses+Submit,
	 * deferred behind the engine's real-frame period if both are pending in
	 * the same beat-loop iteration (engine's Wait takes priority).
	 */
	bool OfferGInterpolated(ID3D12Resource* src, UINT width, UINT height, DXGI_FORMAT format,
		const vr::HmdMatrix34_t& pose);

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

	/**
	 * @brief Engine UpdatePoses(true) is about to WaitGetPoses.
	 * Serializes with the beat thread so both never Wait at once. The beat
	 * thread — never this one — Submits to close the period the engine's
	 * Wait just opened; Submit must never run on the engine thread.
	 */
	class EngineWaitClose
	{
	public:
		EngineWaitClose();
		~EngineWaitClose();
		EngineWaitClose(const EngineWaitClose&) = delete;
		EngineWaitClose& operator=(const EngineWaitClose&) = delete;
	};

	/** @brief Block until first Submit or timeout. */
	bool WaitForFirstSubmitMs(unsigned ms);

	Status GetStatus();
}

#endif
