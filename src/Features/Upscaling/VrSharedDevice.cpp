#include "PCH.h"
#include "VrSharedDevice.h"

#include <mutex>

#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
	std::mutex deviceMutex;
	ComPtr<ID3D12Device> sharedDevice;
	ComPtr<ID3D12CommandQueue> sharedQueue;
	std::mutex queueMutex;
}

ID3D12Device* VrSharedDevice::Get(IDXGIAdapter* adapter)
{
	std::lock_guard lock(deviceMutex);
	if (sharedDevice)
		return sharedDevice.Get();
	if (!adapter)
		return nullptr;
	const HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&sharedDevice));
	if (FAILED(hr)) {
		logger::warn("[VrSharedDevice] D3D12CreateDevice failed hr={:08X}", static_cast<unsigned>(hr));
		return nullptr;
	}
	sharedDevice->SetName(L"VrSharedDevice");
	logger::info("[VrSharedDevice] one D3D12 device for the doorbell");
	return sharedDevice.Get();
}

ID3D12CommandQueue* VrSharedDevice::Queue(IDXGIAdapter* adapter)
{
	if (!Get(adapter))
		return nullptr;
	std::lock_guard lock(deviceMutex);
	if (sharedQueue)
		return sharedQueue.Get();
	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	if (FAILED(sharedDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&sharedQueue)))) {
		logger::warn("[VrSharedDevice] queue create failed");
		return nullptr;
	}
	sharedQueue->SetName(L"VrSharedDevice::Queue");
	logger::info("[VrSharedDevice] one queue for NR, composite inputs, and FSR");
	return sharedQueue.Get();
}

std::mutex& VrSharedDevice::Mutex()
{
	return queueMutex;
}
