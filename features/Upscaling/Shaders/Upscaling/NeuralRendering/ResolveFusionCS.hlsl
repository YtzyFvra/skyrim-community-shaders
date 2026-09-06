Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> NeuralColor : register(t1);
Texture2D<float> DepthGuide : register(t2);
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
	uint Direction;
	uint DenoiseMode;
	float DenoiseStrength;
	float Softness;
	uint DepthWidth;
	uint DepthHeight;
	float FrequencyGuidance;
	float DepthGuidance;
	uint EnhancedGuidance;
	float ColorGuidance;
	float2 GuidancePadding;
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

float LoadDepth(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(DepthWidth - 1, DepthHeight - 1));
	return DepthGuide.Load(int3(pixel, 0));
}

float DepthBlockConfidence(int2 pixel)
{
	if (EnhancedGuidance == 0 || DepthGuidance <= 0.0)
		return 1.0;
	float2 depthPosition = (float2(pixel) + 0.5) *
		float2(DepthWidth, DepthHeight) / float2(FullWidth, FullHeight) - 0.5;
	int2 depthPixel = int2(round(depthPosition));
	int2 radius = max(int2(1, 1), int2(round(2.0 * float2(DepthWidth, DepthHeight) / float2(FullWidth, FullHeight))));
	float center = LoadDepth(depthPixel);
	float minimumDepth = center;
	float maximumDepth = center;
	float samples[4] = {
		LoadDepth(depthPixel + int2(-radius.x, 0)), LoadDepth(depthPixel + int2(radius.x, 0)),
		LoadDepth(depthPixel + int2(0, -radius.y)), LoadDepth(depthPixel + int2(0, radius.y))
	};
	[unroll]
	for (uint index = 0; index < 4; ++index) {
		minimumDepth = min(minimumDepth, samples[index]);
		maximumDepth = max(maximumDepth, samples[index]);
	}
	float blockRange = maximumDepth - minimumDepth;
	float discontinuity = smoothstep(0.002, 0.025, blockRange);
	float confidence = lerp(1.0, 0.45, discontinuity);
	return lerp(1.0, confidence, saturate(DepthGuidance));
}

float EnhancedStructureConfidence(int2 pixel, float3 original, float3 neural,
	float originalLuma, float baseLuma, float gradient)
{
	if (EnhancedGuidance == 0)
		return 1.0;

	float lumaMin = originalLuma;
	float lumaMax = originalLuma;
	float3 blockColors[4] = {
		LoadOriginal(pixel + int2(-2, 0)).rgb, LoadOriginal(pixel + int2(2, 0)).rgb,
		LoadOriginal(pixel + int2(0, -2)).rgb, LoadOriginal(pixel + int2(0, 2)).rgb
	};
	[unroll]
	for (uint index = 0; index < 4; ++index) {
		float sampleLuma = dot(blockColors[index], LumaWeights);
		lumaMin = min(lumaMin, sampleLuma);
		lumaMax = max(lumaMax, sampleLuma);
	}
	float blockFrequency = abs(originalLuma - baseLuma) * 4.0 + (lumaMax - lumaMin) + gradient * 0.5;
	float frequencyConfidence = lerp(1.0, lerp(1.0, 0.45, smoothstep(0.08, 0.45, blockFrequency)),
		saturate(FrequencyGuidance));

	float originalScale = max(originalLuma, 0.08);
	float lumaDisagreement = abs(dot(neural, LumaWeights) - originalLuma) / originalScale;
	float3 originalChroma = original - originalLuma;
	float neuralLuma = dot(neural, LumaWeights);
	float3 neuralChroma = neural - neuralLuma;
	float chromaDisagreement = length(neuralChroma - originalChroma) / originalScale;
	float colorDisagreement = lumaDisagreement * 0.65 + chromaDisagreement * 0.35;
	float colorConfidence = lerp(1.0, lerp(1.0, 0.35, smoothstep(0.08, 0.55, colorDisagreement)),
		saturate(ColorGuidance));
	return max(0.35, frequencyConfidence * colorConfidence * DepthBlockConfidence(pixel));
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

float3 FilterNeural(int2 pixel)
{
	float3 center = LoadNeural(pixel).rgb;
	if (DenoiseMode == 0 || DenoiseStrength <= 0.0)
		return center;

	float centerLuma = dot(center, LumaWeights);
	float3 filtered = 0.0;
	float weightSum = 0.0;
	[unroll]
	for (int y = -1; y <= 1; ++y) {
		[unroll]
		for (int x = -1; x <= 1; ++x) {
			float3 sampleColor = LoadNeural(pixel + int2(x, y)).rgb;
			float spatialWeight = (x == 0 && y == 0) ? 1.0 : ((x == 0 || y == 0) ? 0.72 : 0.48);
			float lumaDelta = abs(dot(sampleColor, LumaWeights) - centerLuma);
			float rangeScale = DenoiseMode == 4 ? 18.0 : (DenoiseMode == 1 ? 42.0 : 28.0);
			float rangeWeight = DenoiseMode == 2 ? 1.0 : exp2(-lumaDelta * rangeScale);
			float weight = spatialWeight * rangeWeight;
			filtered += sampleColor * weight;
			weightSum += weight;
		}
	}
	filtered /= max(weightSum, 1e-5);
	if (DenoiseMode == 3) {
		float filteredLuma = dot(filtered, LumaWeights);
		filtered += (centerLuma - filteredLuma) * LumaWeights / dot(LumaWeights, LumaWeights);
	}
	return lerp(center, filtered, saturate(DenoiseStrength * (DenoiseMode == 4 ? 1.25 : 1.0)));
}

float3 AnalyzeReference(int2 pixel, float3 center,
	out float leftLuma, out float rightLuma, out float upLuma, out float downLuma)
{
	float3 left = LoadOriginal(pixel + int2(-1, 0)).rgb;
	float3 right = LoadOriginal(pixel + int2(1, 0)).rgb;
	float3 up = LoadOriginal(pixel + int2(0, -1)).rgb;
	float3 down = LoadOriginal(pixel + int2(0, 1)).rgb;
	leftLuma = dot(left, LumaWeights);
	rightLuma = dot(right, LumaWeights);
	upLuma = dot(up, LumaWeights);
	downLuma = dot(down, LumaWeights);
	float3 diagonal = LoadOriginal(pixel + int2(-1, -1)).rgb + LoadOriginal(pixel + int2(1, -1)).rgb +
		LoadOriginal(pixel + int2(-1, 1)).rgb + LoadOriginal(pixel + int2(1, 1)).rgb;
	return center * 0.25 + (left + right + up + down) * 0.125 + diagonal * 0.0625;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= FullWidth || dispatchThreadID.y >= FullHeight)
		return;

	int2 pixel = int2(dispatchThreadID.xy);
	float4 original = LoadOriginal(pixel);
	float3 neural = FilterNeural(pixel);
	float leftLuma;
	float rightLuma;
	float upLuma;
	float downLuma;
	float3 originalBase = AnalyzeReference(pixel, original.rgb, leftLuma, rightLuma, upLuma, downLuma);
	float originalLuma = dot(original.rgb, LumaWeights);
	float baseLuma = dot(originalBase, LumaWeights);
	float neuralLuma = dot(neural, LumaWeights);

	float gradient = abs(rightLuma - leftLuma) + abs(downLuma - upLuma);
	float edge = saturate(gradient * 4.0);
	float structureConfidence = EnhancedStructureConfidence(
		pixel, original.rgb, neural, originalLuma, baseLuma, gradient);
	float guard = saturate(1.0 - EdgeProtection * edge) * structureConfidence;

	float lumaResidual = clamp(neuralLuma - baseLuma, -ResidualLimit, ResidualLimit);
	float redResidual = clamp((neural.r - neuralLuma) - (originalBase.r - baseLuma), -ResidualLimit, ResidualLimit);
	float blueResidual = clamp((neural.b - neuralLuma) - (originalBase.b - baseLuma), -ResidualLimit, ResidualLimit);
	float3 result;
	if (Direction == 0) {
		float fusedLuma = originalLuma + guard * LumaStrength * lumaResidual;
		float fusedRed = fusedLuma + (original.r - originalLuma) + guard * ChromaStrength * redResidual;
		float fusedBlue = fusedLuma + (original.b - originalLuma) + guard * ChromaStrength * blueResidual;
		float fusedGreen = (fusedLuma - LumaWeights.r * fusedRed - LumaWeights.b * fusedBlue) / LumaWeights.g;
		result = float3(fusedRed, fusedGreen, fusedBlue);
	} else {
		float3 detail = clamp(original.rgb - originalBase, -ResidualLimit, ResidualLimit);
		float detailLuma = dot(detail, LumaWeights);
		float3 detailChroma = detail - detailLuma;
		float detailConfidence = guard * saturate(1.0 - Softness * 0.7);
		float3 neuralWithDetail = neural + detailConfidence * (LumaStrength * detailLuma + ChromaStrength * detailChroma);
		result = lerp(original.rgb, neuralWithDetail, structureConfidence);
	}
	float3 softTarget = lerp(originalBase, neural, Direction == 0 ? 0.65 : 0.85);
	result = lerp(result, softTarget, saturate(Softness) * 0.45 * structureConfidence);
	OutputColor[dispatchThreadID.xy] = float4(saturate(result), original.a);
}