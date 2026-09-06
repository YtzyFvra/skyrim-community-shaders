Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> NeuralColor : register(t1);
RWTexture2D<float4> OutputColor : register(u0);

cbuffer FusionConstants : register(b0)
{
	uint FullWidth;
	uint FullHeight;
	uint LowWidth;
	uint LowHeight;
	float LumaStrength;
	float ChromaStrength;
	float EdgeProtection;
	float ResidualLimit;
	uint OutputMode;
	float3 Padding;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

float4 LoadOriginal(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(FullWidth - 1, FullHeight - 1));
	return OriginalColor.Load(int3(pixel, 0));
}

float4 LoadNeural(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(LowWidth - 1, LowHeight - 1));
	return NeuralColor.Load(int3(pixel, 0));
}

float4 SampleNeuralEasu(float2 pixel)
{
	int2 base = int2(floor(pixel));
	float2 fraction = frac(pixel);
	float3 samples[12] = {
		LoadNeural(base + int2(0, -1)).rgb,
		LoadNeural(base + int2(1, -1)).rgb,
		LoadNeural(base + int2(-1, 0)).rgb,
		LoadNeural(base + int2(0, 0)).rgb,
		LoadNeural(base + int2(1, 0)).rgb,
		LoadNeural(base + int2(2, 0)).rgb,
		LoadNeural(base + int2(-1, 1)).rgb,
		LoadNeural(base + int2(0, 1)).rgb,
		LoadNeural(base + int2(1, 1)).rgb,
		LoadNeural(base + int2(2, 1)).rgb,
		LoadNeural(base + int2(0, 2)).rgb,
		LoadNeural(base + int2(1, 2)).rgb,
	};
	float luma[12];
	[unroll]
	for (uint index = 0; index < 12; ++index)
		luma[index] = dot(samples[index], LumaWeights);

	float2 direction = float2(
		(luma[4] + luma[8]) - (luma[3] + luma[7]),
		(luma[7] + luma[8]) - (luma[3] + luma[4]));
	float directionLength = length(direction);
	direction = directionLength > 1e-5 ? direction / directionLength : float2(1.0, 0.0);
	float anisotropy = saturate(directionLength * 4.0);
	float2 orthogonal = float2(-direction.y, direction.x);
	float3 accumulated = 0.0;
	float weightSum = 0.0;
	static const int2 offsets[12] = {
		int2(0, -1), int2(1, -1), int2(-1, 0), int2(0, 0), int2(1, 0), int2(2, 0),
		int2(-1, 1), int2(0, 1), int2(1, 1), int2(2, 1), int2(0, 2), int2(1, 2)
	};
	[unroll]
	for (uint tap = 0; tap < 12; ++tap) {
		float2 delta = float2(offsets[tap]) - fraction;
		float along = dot(delta, direction);
		float across = dot(delta, orthogonal);
		float distanceSquared = along * along * lerp(1.0, 0.55, anisotropy) +
			across * across * lerp(1.0, 1.8, anisotropy);
		float weight = saturate(1.0 - distanceSquared * 0.25);
		weight = weight * weight * (3.0 - 2.0 * weight);
		accumulated += samples[tap] * weight;
		weightSum += weight;
	}
	float3 result = accumulated / max(weightSum, 1e-5);
	float3 localMin = min(min(samples[3], samples[4]), min(samples[7], samples[8]));
	float3 localMax = max(max(samples[3], samples[4]), max(samples[7], samples[8]));
	return float4(clamp(result, localMin, localMax), 1.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= FullWidth || dispatchThreadID.y >= FullHeight)
		return;

	int2 pixel = int2(dispatchThreadID.xy);
	float4 original = LoadOriginal(pixel);
	float3 neural = NeuralColor.Load(int3(pixel, 0)).rgb;

	float3 originalBase = (
		LoadOriginal(pixel + int2(-1, -1)).rgb + LoadOriginal(pixel + int2(1, -1)).rgb +
		LoadOriginal(pixel + int2(-1, 1)).rgb + LoadOriginal(pixel + int2(1, 1)).rgb) * 0.25;
	float originalLuma = dot(original.rgb, LumaWeights);
	float baseLuma = dot(originalBase, LumaWeights);
	float neuralLuma = dot(neural, LumaWeights);

	float leftLuma = dot(LoadOriginal(pixel + int2(-1, 0)).rgb, LumaWeights);
	float rightLuma = dot(LoadOriginal(pixel + int2(1, 0)).rgb, LumaWeights);
	float upLuma = dot(LoadOriginal(pixel + int2(0, -1)).rgb, LumaWeights);
	float downLuma = dot(LoadOriginal(pixel + int2(0, 1)).rgb, LumaWeights);
	float edge = saturate((abs(rightLuma - leftLuma) + abs(downLuma - upLuma)) * 4.0);
	float guard = saturate(1.0 - EdgeProtection * edge);

	float lumaResidual = clamp(neuralLuma - baseLuma, -ResidualLimit, ResidualLimit);
	float redResidual = clamp((neural.r - neuralLuma) - (originalBase.r - baseLuma), -ResidualLimit, ResidualLimit);
	float blueResidual = clamp((neural.b - neuralLuma) - (originalBase.b - baseLuma), -ResidualLimit, ResidualLimit);
	float fusedLuma = originalLuma + guard * LumaStrength * lumaResidual;
	float fusedRed = fusedLuma + (original.r - originalLuma) + guard * ChromaStrength * redResidual;
	float fusedBlue = fusedLuma + (original.b - originalLuma) + guard * ChromaStrength * blueResidual;
	float fusedGreen = (fusedLuma - LumaWeights.r * fusedRed - LumaWeights.b * fusedBlue) / LumaWeights.g;

	OutputColor[dispatchThreadID.xy] = float4(saturate(float3(fusedRed, fusedGreen, fusedBlue)), original.a);
}