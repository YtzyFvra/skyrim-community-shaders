#pragma once

#include <cstdint>
#include <d3d11.h>

namespace VrFgPump
{
	/** @brief User wants VR DLSS-G and Streamline DX12 is loaded. */
	bool WantsPump();

	/** @brief True after the private SL Present pump is live. */
	bool IsActive();

	/** @brief Last breadcrumb and counters for the FG debug line / devbench. */
	struct DebugSnapshot
	{
		const char* lastStep = "";
		bool active = false;
		bool busy = false;
		bool windowVisible = false;
		uint32_t appPresents = 0;
		uint32_t hookSubmits = 0;
		uint32_t lastHr = 0;
		uint32_t busyMs = 0;
	};

	/** @brief Last breadcrumb step; for the FG debug line. */
	const char* LastStep();

	/** @brief Live pump counters; safe to call from the menu thread. */
	DebugSnapshot Snapshot();

	/**
	 * @brief Inject: copy guides and kick the G doorbell worker. Hook Submits
	 *        each G Present as-is. Returns false if the worker is still presenting
	 *        so the engine world Submit stays live.
	 * @return true if the pump owns this frame (swallow armed).
	 */
	bool PresentOwnedSbs(ID3D11Texture2D* hudLessSbs, std::uint32_t width, std::uint32_t height);
}
