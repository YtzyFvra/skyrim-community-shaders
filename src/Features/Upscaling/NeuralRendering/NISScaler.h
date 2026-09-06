#pragma once

#include <winrt/base.h>

struct ID3D11Buffer;
struct ID3D11ComputeShader;
struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11SamplerState;
struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11UnorderedAccessView;

namespace NeuralRendering
{
	class NISScaler
	{
	public:
		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context,
			ID3D11ShaderResourceView* input, ID3D11UnorderedAccessView* output,
			std::uint32_t inputWidth, std::uint32_t inputHeight,
			std::uint32_t outputWidth, std::uint32_t outputHeight, float sharpness);
		void Reset();

	private:
		bool Initialize(ID3D11Device* device);

		winrt::com_ptr<ID3D11ComputeShader> shader_;
		winrt::com_ptr<ID3D11Buffer> constants_;
		winrt::com_ptr<ID3D11SamplerState> sampler_;
		winrt::com_ptr<ID3D11Texture2D> scalerCoefficients_;
		winrt::com_ptr<ID3D11Texture2D> usmCoefficients_;
		winrt::com_ptr<ID3D11ShaderResourceView> scalerCoefficientsSRV_;
		winrt::com_ptr<ID3D11ShaderResourceView> usmCoefficientsSRV_;
	};
}