#include "NISScaler.h"

#include "GpuPass.h"

#pragma warning(push)
#pragma warning(disable: 4324)
#include "../../../../extern/Streamline-DX12/source/plugins/sl.nis/NIS/NIS_Config.h"
#pragma warning(pop)
#include "../../../../extern/Streamline-DX12/source/plugins/sl.nis/NIS_shaders.h"

#include <d3d11.h>

namespace NeuralRendering
{
	bool NISScaler::Initialize(ID3D11Device* device)
	{
		if (shader_)
			return true;
		if (!device || FAILED(device->CreateComputeShader(
				NIS_Scaler_V0_H0_cs, NIS_Scaler_V0_H0_cs_len, nullptr, shader_.put())))
			return false;

		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.ByteWidth = sizeof(NISConfig);
		bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
		bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, constants_.put())))
			return false;

		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(device->CreateSamplerState(&samplerDesc, sampler_.put())))
			return false;

		D3D11_TEXTURE2D_DESC textureDesc{};
		textureDesc.Width = static_cast<UINT>(kFilterSize / 4);
		textureDesc.Height = static_cast<UINT>(kPhaseCount);
		textureDesc.MipLevels = 1;
		textureDesc.ArraySize = 1;
		textureDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
		textureDesc.SampleDesc.Count = 1;
		textureDesc.Usage = D3D11_USAGE_IMMUTABLE;
		textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA scalerData{ coef_scale, static_cast<UINT>(kFilterSize * sizeof(float)), 0 };
		D3D11_SUBRESOURCE_DATA usmData{ coef_usm, static_cast<UINT>(kFilterSize * sizeof(float)), 0 };
		if (FAILED(device->CreateTexture2D(&textureDesc, &scalerData, scalerCoefficients_.put())) ||
			FAILED(device->CreateTexture2D(&textureDesc, &usmData, usmCoefficients_.put())) ||
			FAILED(device->CreateShaderResourceView(scalerCoefficients_.get(), nullptr, scalerCoefficientsSRV_.put())) ||
			FAILED(device->CreateShaderResourceView(usmCoefficients_.get(), nullptr, usmCoefficientsSRV_.put())))
			return false;
		return true;
	}

	bool NISScaler::Apply(ID3D11Device* device, ID3D11DeviceContext* context,
		ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output,
		std::uint32_t inputWidth, std::uint32_t inputHeight,
		std::uint32_t outputWidth, std::uint32_t outputHeight, float sharpness)
	{
		if (!context || !input || !output || !Initialize(device))
			return false;
		NISConfig config{};
		if (!NVScalerUpdateConfig(config, sharpness,
				0, 0, inputWidth, inputHeight, inputWidth, inputHeight,
				0, 0, outputWidth, outputHeight, outputWidth, outputHeight, NISHDRMode::None))
			return false;

		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(constants_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return false;
		std::memcpy(mapped.pData, &config, sizeof(config));
		context->Unmap(constants_.get(), 0);

		CS_GPU_PASS("NeuralRendering::NISScaler");
		context->CSSetShader(shader_.get(), nullptr, 0);
		ID3D11Buffer* buffers[]{ constants_.get() };
		context->CSSetConstantBuffers(0, 1, buffers);
		ID3D11SamplerState* samplers[]{ sampler_.get() };
		context->CSSetSamplers(0, 1, samplers);
		ID3D11ShaderResourceView* srvs[]{ input, scalerCoefficientsSRV_.get(), usmCoefficientsSRV_.get() };
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
		context->Dispatch((outputWidth + 31) / 32, (outputHeight + 23) / 24, 1);

		ID3D11Buffer* nullBuffer[]{ nullptr };
		ID3D11SamplerState* nullSampler[]{ nullptr };
		ID3D11ShaderResourceView* nullSRVs[]{ nullptr, nullptr, nullptr };
		ID3D11UnorderedAccessView* nullUAV[]{ nullptr };
		context->CSSetConstantBuffers(0, 1, nullBuffer);
		context->CSSetSamplers(0, 1, nullSampler);
		context->CSSetShaderResources(0, 3, nullSRVs);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	void NISScaler::Reset()
	{
		shader_ = nullptr;
		constants_ = nullptr;
		sampler_ = nullptr;
		scalerCoefficients_ = nullptr;
		usmCoefficients_ = nullptr;
		scalerCoefficientsSRV_ = nullptr;
		usmCoefficientsSRV_ = nullptr;
	}
}