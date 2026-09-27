#pragma once

#include <mutex>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct IDXGIAdapter;

/**
 * The one D3D12 device and the one DIRECT queue for the whole VR doorbell,
 * including frame generation. The engine only shares resources in; it does
 * not draw the doorbell.
 */
namespace VrSharedDevice
{
	ID3D12Device* Get(IDXGIAdapter* adapter);
	/** @brief The single doorbell queue. Null if the device was not created. */
	ID3D12CommandQueue* Queue(IDXGIAdapter* adapter);
	/** @brief Serializes queue calls. Do not hold it across a CPU fence wait. */
	std::mutex& Mutex();

	/** @brief Runs one queue call under Mutex(). The lock is not held across a CPU wait. */
	template <class Fn>
	decltype(auto) WithQueue(Fn&& fn)
	{
		std::lock_guard lock(Mutex());
		return fn();
	}
}
