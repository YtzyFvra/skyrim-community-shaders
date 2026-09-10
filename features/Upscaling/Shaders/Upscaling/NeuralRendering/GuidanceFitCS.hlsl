// Fits NR as a locally affine operator on its own input/output pair at N.
// u0.rgb: a = lerp(prior, a_fit, ρ²). u0.a: ρ² (how affine I→N is in the window).
// u1: same-side weighted 5x5 mean of N - I.
Texture2D<float4> NeuralInput : register(t0);
Texture2D<float4> NeuralColor : register(t1);
Texture2D<float> DepthGuide : register(t2);
RWTexture2D<float4> OutputGain : register(u0);
RWTexture2D<float4> OutputEditLow : register(u1);

cbuffer FusionConstants : register(b0)
{
	uint FullWidth;
	uint FullHeight;
	uint GuidanceWidth;
	uint GuidanceHeight;
	float DetailLuma;
	float DetailChroma;
	float RdEdgeFloor;
	float Regularization;
	uint CompareMode;
	uint DebugView;
	float GainPrior;
	float FitWindow;
	uint DepthWidth;
	uint DepthHeight;
	float MaxGain;
	float DepthGuidance;
	uint EdgeBlockMode;
	float EdgeBlockGThresh;
	uint NeuralWidth;
	uint NeuralHeight;
	float CameraFar;
	float CameraNear;
	float CameraRange;
	float CameraProduct;
	float DepthDilate;
	float DetailGain;
	float ToneGainLuma;
	float ToneGainChroma;
	float DitherStrength;
	float EdgeBlockSkip;
	float EdgeBlockDilateD;
	float EdgeBlockMix;
};

static const float SkyRaw = 1.0 - 64.0 / 16777216.0;
static const float MaskRaw = 1e-5;
static const float SkyLinear = 1.0e7;

float3 LoadInput(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(NeuralWidth, NeuralHeight) - 1);
	return NeuralInput.Load(int3(pixel, 0)).rgb;
}

float3 LoadNeural(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(NeuralWidth, NeuralHeight) - 1);
	return NeuralColor.Load(int3(pixel, 0)).rgb;
}

float LoadRawDepth(int2 pixel)
{
	if (DepthWidth == 0 || DepthHeight == 0)
		return 1.0;
	pixel = clamp(pixel, int2(0, 0), int2(DepthWidth, DepthHeight) - 1);
	return DepthGuide.Load(int3(pixel, 0));
}

float LinearizeDepth(float raw)
{
	if (raw < MaskRaw)
		return CameraNear;
	if (raw >= SkyRaw || CameraRange <= 0.0 || CameraProduct <= 0.0)
		return SkyLinear;
	return CameraProduct / max(-raw * CameraRange + CameraFar, 1e-5);
}

bool RdPairCliff(int2 a, int2 b)
{
	float rawA = LoadRawDepth(a);
	float rawB = LoadRawDepth(b);
	if (rawA < MaskRaw || rawB < MaskRaw)
		return false;
	bool skyA = rawA >= SkyRaw;
	bool skyB = rawB >= SkyRaw;
	if (skyA != skyB)
		return true;
	if (skyA || skyB)
		return false;
	float linearA = LinearizeDepth(rawA);
	float linearB = LinearizeDepth(rawB);
	return abs(linearA - linearB) / max(min(linearA, linearB), 1.0) >
		lerp(0.18, 0.028, saturate(DepthGuidance));
}

int2 OwnerR(int2 neuralPixel)
{
	float2 position = (float2(neuralPixel) + 0.5) *
		float2(DepthWidth, DepthHeight) / float2(NeuralWidth, NeuralHeight) - 0.5;
	return clamp(int2(round(position)), int2(0, 0), int2(DepthWidth, DepthHeight) - 1);
}

bool SameDepthSide(int2 center, int2 sample)
{
	if (DepthWidth == 0 || DepthHeight == 0)
		return true;
	return !RdPairCliff(OwnerR(center), OwnerR(sample));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= NeuralWidth || dispatchThreadID.y >= NeuralHeight)
		return;

	int2 pixel = int2(dispatchThreadID.xy);
	int radius = FitWindow >= 0.5 ? 2 : 1;
	float3 centerI = LoadInput(pixel);
	float centerY = dot(centerI, float3(0.2126, 0.7152, 0.0722));
	float sigma = clamp(EdgeBlockSkip, 0.02, 0.5);
	float invTwoSigma2 = 0.5 / (sigma * sigma);
	float3 sumI = 0.0;
	float3 sumN = 0.0;
	float3 sumII = 0.0;
	float3 sumIN = 0.0;
	float3 sumNN = 0.0;
	float3 sumEdit = 0.0;
	float weightSum = 0.0;
	float editWeight = 0.0;
	[unroll]
	for (int y = -2; y <= 2; ++y) {
		[unroll]
		for (int x = -2; x <= 2; ++x) {
			int2 samplePixel = pixel + int2(x, y);
			float3 i = LoadInput(samplePixel);
			float3 n = LoadNeural(samplePixel);
			if (!SameDepthSide(pixel, samplePixel))
				continue;
			float sampleY = dot(i, float3(0.2126, 0.7152, 0.0722));
			float weight = exp(-(sampleY - centerY) * (sampleY - centerY) * invTwoSigma2);
			sumEdit += (n - i) * weight;
			editWeight += weight;
			if (abs(x) > radius || abs(y) > radius)
				continue;
			sumI += i * weight;
			sumN += n * weight;
			sumII += i * i * weight;
			sumIN += i * n * weight;
			sumNN += n * n * weight;
			weightSum += weight;
		}
	}
	if (weightSum < 1e-4) {
		sumI = centerI;
		sumN = LoadNeural(pixel);
		sumII = centerI * centerI;
		sumIN = centerI * sumN;
		sumNN = sumN * sumN;
		weightSum = 1.0;
	}
	if (editWeight < 1e-4) {
		sumEdit = LoadNeural(pixel) - centerI;
		editWeight = 1.0;
	}
	float3 meanI = sumI / weightSum;
	float3 meanN = sumN / weightSum;
	float3 varI = max(sumII / weightSum - meanI * meanI, 0.0);
	float3 varN = max(sumNN / weightSum - meanN * meanN, 0.0);
	float3 covIN = sumIN / weightSum - meanI * meanN;
	float3 sumDI = max(sumII - meanI * meanI * weightSum, 0.0);
	float3 sumDN = max(sumNN - meanN * meanN * weightSum, 0.0);
	float3 sumDIDN = sumIN - meanI * meanN * weightSum;

	float eps = Regularization * Regularization * 0.05;
	float gainMax = lerp(1.0, 3.0, saturate(MaxGain));
	float3 fitted = clamp((covIN + eps * GainPrior) / (varI + eps), 0.0, gainMax);
	float3 rho2 = saturate((sumDIDN * sumDIDN) / (sumDI * sumDN + eps * eps));
	float3 gain = lerp(GainPrior, fitted, rho2);
	OutputGain[dispatchThreadID.xy] = float4(gain, dot(rho2, float3(0.2126, 0.7152, 0.0722)));
	OutputEditLow[dispatchThreadID.xy] = float4(sumEdit / editWeight, 0.0);
}
