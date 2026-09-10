// Affine-guided lifting. a is already lerp(prior, a_fit, ρ²) from the fit pass.
//   D = RC + a * (G_D - Lo) + (ToneGain - 1) * E_lf + (DetailGain - 1) * E_hf
Texture2D<float4> OriginalColor : register(t0);
Texture2D<float4> NeuralColor : register(t1);
Texture2D<float4> NeuralGain : register(t2);
Texture2D<float4> GuideLow : register(t3);
Texture2D<float4> EditLow : register(t4);
Texture2D<float> DepthGuide : register(t5);
RWTexture2D<float4> OutputColor : register(u0);

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

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);
static const float SkyRaw = 1.0 - 64.0 / 16777216.0;
static const float MaskRaw = 1e-5;
static const float SkyLinear = 1.0e7;

float3 LoadGuidance(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(GuidanceWidth, GuidanceHeight) - 1);
	return OriginalColor.Load(int3(pixel, 0)).rgb;
}

float4 LoadGain(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(NeuralWidth, NeuralHeight) - 1);
	return NeuralGain.Load(int3(pixel, 0));
}

float3 LoadEditLow(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(NeuralWidth, NeuralHeight) - 1);
	return EditLow.Load(int3(pixel, 0)).rgb;
}

float3 GuidanceAtDisplay(int2 destPixel)
{
	float2 ratio = float2(GuidanceWidth, GuidanceHeight) / float2(FullWidth, FullHeight);
	float2 position = (float2(destPixel) + 0.5) * ratio - 0.5;
	if (all(abs(ratio - 1.0) < 1e-3))
		return LoadGuidance(int2(round(position)));
	int2 base = int2(floor(position));
	float3 acc = 0.0;
	float weightSum = 0.0;
	[unroll]
	for (int y = -1; y <= 2; ++y) {
		[unroll]
		for (int x = -1; x <= 2; ++x) {
			float2 delta = abs(float2(base + int2(x, y)) - position) / max(ratio, 1.0);
			float weight = saturate(1.0 - delta.x) * saturate(1.0 - delta.y);
			if (weight <= 0.0)
				continue;
			acc += LoadGuidance(base + int2(x, y)) * weight;
			weightSum += weight;
		}
	}
	return acc / max(weightSum, 1e-5);
}

float2 NeuralPosition(int2 destPixel)
{
	if (EdgeBlockMode != 0)
		return float2(destPixel);
	return (float2(destPixel) + 0.5) * float2(NeuralWidth, NeuralHeight) / float2(FullWidth, FullHeight) - 0.5;
}

float4 GainAtDisplay(float2 position)
{
	int2 base = int2(floor(position));
	float2 fraction = frac(position);
	return lerp(
		lerp(LoadGain(base), LoadGain(base + int2(1, 0)), fraction.x),
		lerp(LoadGain(base + int2(0, 1)), LoadGain(base + int2(1, 1)), fraction.x),
		fraction.y);
}

float3 EditLowAtDisplay(float2 position)
{
	int2 base = int2(floor(position));
	float2 fraction = frac(position);
	return lerp(
		lerp(LoadEditLow(base), LoadEditLow(base + int2(1, 0)), fraction.x),
		lerp(LoadEditLow(base + int2(0, 1)), LoadEditLow(base + int2(1, 1)), fraction.x),
		fraction.y);
}

float3 CubeScale(float3 base, float3 inject)
{
	float alpha = 1.0;
	[unroll]
	for (int c = 0; c < 3; ++c) {
		if (inject[c] > 1e-6)
			alpha = min(alpha, (1.0 - base[c]) / inject[c]);
		else if (inject[c] < -1e-6)
			alpha = min(alpha, (0.0 - base[c]) / inject[c]);
	}
	return base + saturate(alpha) * inject;
}

float Dither(int2 pixel)
{
	float3 p3 = frac(float3(pixel.xyx) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	float2 r = frac((p3.xx + p3.yz) * p3.zy);
	return (r.x + r.y - 1.0) / 255.0;
}

float3 SplitLumaChroma(float3 value, float lumaGain, float chromaGain)
{
	float luma = dot(value, LumaWeights);
	return lumaGain * luma + chromaGain * (value - luma);
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

int2 OwnerRFromDisplay(int2 destPixel)
{
	float2 position = (float2(destPixel) + 0.5) *
		float2(DepthWidth, DepthHeight) / float2(FullWidth, FullHeight) - 0.5;
	return clamp(int2(round(position)), int2(0, 0), int2(DepthWidth, DepthHeight) - 1);
}

int2 GuidanceFromDisplay(int2 destPixel)
{
	float2 position = (float2(destPixel) + 0.5) *
		float2(GuidanceWidth, GuidanceHeight) / float2(FullWidth, FullHeight) - 0.5;
	return clamp(int2(round(position)), int2(0, 0), int2(GuidanceWidth, GuidanceHeight) - 1);
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

bool RdCliffAtR(int2 engine)
{
	return RdPairCliff(engine, engine + int2(1, 0)) ||
		RdPairCliff(engine, engine + int2(-1, 0)) ||
		RdPairCliff(engine, engine + int2(0, 1)) ||
		RdPairCliff(engine, engine + int2(0, -1));
}

float2 GuidanceGradient(int2 destPixel)
{
	int2 g = GuidanceFromDisplay(destPixel);
	float gx = dot(LoadGuidance(g + int2(1, 0)), LumaWeights) - dot(LoadGuidance(g + int2(-1, 0)), LumaWeights);
	float gy = dot(LoadGuidance(g + int2(0, 1)), LumaWeights) - dot(LoadGuidance(g + int2(0, -1)), LumaWeights);
	return float2(gx, gy);
}

float LumaVar3(int2 destPixel, bool guidance)
{
	float sum = 0.0;
	float sumSq = 0.0;
	[unroll]
	for (int y = -1; y <= 1; ++y) {
		[unroll]
		for (int x = -1; x <= 1; ++x) {
			int2 dest = destPixel + int2(x, y);
			float luma = guidance ?
				dot(LoadGuidance(GuidanceFromDisplay(dest)), LumaWeights) :
				dot(GuideLow.Load(int3(clamp(dest, int2(0, 0), int2(FullWidth, FullHeight) - 1), 0)).rgb, LumaWeights);
			sum += luma;
			sumSq += luma * luma;
		}
	}
	return max(sumSq / 9.0 - (sum / 9.0) * (sum / 9.0), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	int2 local = int2(dispatchThreadID.xy);
	int2 cropOrigin = int2(int(EdgeBlockMode & 0xffff), int(EdgeBlockMode >> 16));
	const bool cropResolve = EdgeBlockMode != 0;
	if (cropResolve) {
		if (local.x >= NeuralWidth || local.y >= NeuralHeight)
			return;
	} else if (local.x >= FullWidth || local.y >= FullHeight)
		return;

	int2 neuralPixel = local;
	int2 displayPixel = cropResolve ? local + cropOrigin : local;
	int2 neuralMax = cropResolve ?
		int2(NeuralWidth, NeuralHeight) - 1 : int2(FullWidth, FullHeight) - 1;

	float4 rcSample = NeuralColor.Load(int3(neuralPixel, 0));
	float3 rc = rcSample.rgb;
	float3 lo = GuideLow.Load(int3(neuralPixel, 0)).rgb;
	float2 neuralPosition = NeuralPosition(neuralPixel);
	float4 gainSample = GainAtDisplay(neuralPosition);
	float3 gain = gainSample.rgb;

	bool rightHalf = displayPixel.x * 2 >= (int)FullWidth;
	if (CompareMode == 1 || (CompareMode == 2 && rightHalf) || (CompareMode == 3 && !rightHalf))
		gain = 1.0;

	float3 detail = GuidanceAtDisplay(displayPixel) - lo;
	float3 edit = rc - lo;
	float3 editLow5 = EditLowAtDisplay(neuralPosition);
	float3 editLow13 = 0.0;
	int2 nBase = int2(floor(neuralPosition));
	[unroll]
	for (int y = -1; y <= 1; ++y) {
		[unroll]
		for (int x = -1; x <= 1; ++x)
			editLow13 += LoadEditLow(nBase + int2(x, y));
	}
	editLow13 /= 9.0;
	float3 editMid = editLow5 - editLow13;
	float3 editHigh = edit - editLow5;

	if (EdgeBlockDilateD > 1e-4) {
		float2 g = GuidanceGradient(displayPixel);
		int2 step = int2(round(g / max(length(g), 1e-5)));
		if (all(step == 0))
			step = int2(1, 0);
		float3 ehPlus = NeuralColor.Load(int3(clamp(neuralPixel + step, int2(0, 0), neuralMax), 0)).rgb -
			GuideLow.Load(int3(clamp(neuralPixel + step, int2(0, 0), neuralMax), 0)).rgb -
			EditLowAtDisplay(NeuralPosition(neuralPixel + step));
		float3 ehMinus = NeuralColor.Load(int3(clamp(neuralPixel - step, int2(0, 0), neuralMax), 0)).rgb -
			GuideLow.Load(int3(clamp(neuralPixel - step, int2(0, 0), neuralMax), 0)).rgb -
			EditLowAtDisplay(NeuralPosition(neuralPixel - step));
		editHigh += EdgeBlockDilateD * (editHigh - 0.5 * (ehPlus + ehMinus));
	}

	float3 inject = gain * SplitLumaChroma(detail, DetailLuma, DetailChroma) +
		SplitLumaChroma(editLow13, ToneGainLuma - 1.0, ToneGainChroma - 1.0) +
		SplitLumaChroma(editMid, 0.5 * (ToneGainLuma - 1.0), 0.5 * (ToneGainChroma - 1.0)) +
		(DetailGain - 1.0) * editHigh;

	float3 result = CubeScale(rc, inject);
	float bandGainDelta = max(max(abs(ToneGainLuma - 1.0), abs(ToneGainChroma - 1.0)), abs(DetailGain - 1.0));
	float dither = DitherStrength * saturate(bandGainDelta * 2.0);
	if (dither > 0.0)
		result = saturate(result + Dither(displayPixel) * dither);

	if (DebugView == 1)
		result = gain * 0.5;
	else if (DebugView == 2)
		result = saturate(0.5 + detail * 4.0);
	else if (DebugView == 3)
		result = saturate(0.5 + edit * 4.0);
	else if (DebugView == 4)
		result = saturate(0.5 + editLow5 * 4.0);
	else if (DebugView == 5)
		result = saturate(0.5 + editHigh * 8.0);
	else if (DebugView == 6)
		result = lerp(dot(rc, LumaWeights).xxx * 0.5, float3(1.0, 0.2, 0.1),
			RdCliffAtR(OwnerRFromDisplay(displayPixel)) ? 1.0 : 0.0);
	else if (DebugView == 7) {
		float eps = max(Regularization * Regularization * 0.05, 1e-6);
		float varG = LumaVar3(displayPixel, true);
		float varLo = LumaVar3(displayPixel, false);
		float energyW = saturate(max(varG - varLo, 0.0) / (max(varG - varLo, 0.0) + varLo + eps));
		result = lerp(dot(rc, LumaWeights).xxx * 0.45, float3(1.0, 0.55, 0.15), energyW);
	} else if (DebugView == 9)
		result = lerp(dot(rc, LumaWeights).xxx * 0.45, float3(1.0, 0.85, 0.2), gainSample.a);

	OutputColor[dispatchThreadID.xy] = float4(result, rcSample.a);
}
