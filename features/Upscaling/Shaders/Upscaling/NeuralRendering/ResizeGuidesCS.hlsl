// N centers match color ACT1 DS. Tent/Lanczos walk the G/PX grid; Z is fetched from R.
Texture2D<float> SourceDepth : register(t0);
Texture2D<float2> SourceMotion : register(t1);
RWTexture2D<float> DestinationDepth : register(u0);
RWTexture2D<float2> DestinationMotion : register(u1);

cbuffer ResizeGuidesConstants : register(b0)
{
	uint SourceWidth;
	uint SourceHeight;
	uint TargetWidth;
	uint TargetHeight;
	uint GuidanceWidth;
	uint GuidanceHeight;
	uint Filter;
	float MotionVectorMultiplier;
};

static const float MaskRaw = 1e-5;

float Lanczos2(float x)
{
	x = abs(x);
	if (x < 1e-5)
		return 1.0;
	if (x >= 2.0)
		return 0.0;
	float pix = 3.14159265359 * x;
	return sin(pix) * sin(pix * 0.5) / (pix * pix * 0.5);
}

float Tent(float x)
{
	return saturate(1.0 - abs(x));
}

int2 ClampSource(int2 pixel)
{
	return clamp(pixel, int2(0, 0), int2(SourceWidth - 1, SourceHeight - 1));
}

int2 GuideToSource(int2 guide)
{
	if (GuidanceWidth == 0 || GuidanceHeight == 0 ||
		(GuidanceWidth == SourceWidth && GuidanceHeight == SourceHeight))
		return ClampSource(guide);
	float2 sourcePosition = (float2(guide) + 0.5) *
		float2(SourceWidth, SourceHeight) / float2(GuidanceWidth, GuidanceHeight) - 0.5;
	return ClampSource(int2(round(sourcePosition)));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= TargetWidth || dispatchThreadID.y >= TargetHeight)
		return;
	if (SourceWidth == TargetWidth && SourceHeight == TargetHeight) {
		DestinationDepth[dispatchThreadID.xy] = SourceDepth.Load(int3(dispatchThreadID.xy, 0));
		DestinationMotion[dispatchThreadID.xy] =
			SourceMotion.Load(int3(dispatchThreadID.xy, 0)) * MotionVectorMultiplier;
		return;
	}

	int2 sourceMax = int2(SourceWidth - 1, SourceHeight - 1);
	if (Filter == 0) {
		float2 sourcePosition = (float2(dispatchThreadID.xy) + 0.5) *
			float2(SourceWidth, SourceHeight) / float2(TargetWidth, TargetHeight) - 0.5;
		int2 base = int2(floor(sourcePosition));
		int2 candidates[4] = {
			clamp(base, int2(0, 0), sourceMax),
			clamp(base + int2(1, 0), int2(0, 0), sourceMax),
			clamp(base + int2(0, 1), int2(0, 0), sourceMax),
			clamp(base + int2(1, 1), int2(0, 0), sourceMax)
		};
		float selectedDepth = 1.0;
		float2 selectedMotion = 0.0;
		bool foundGeometry = false;
		[unroll]
		for (uint index = 0; index < 4; ++index) {
			float depth = SourceDepth.Load(int3(candidates[index], 0));
			if (depth < MaskRaw)
				continue;
			if (!foundGeometry || depth < selectedDepth) {
				selectedDepth = depth;
				selectedMotion = SourceMotion.Load(int3(candidates[index], 0));
				foundGeometry = true;
			}
		}
		DestinationDepth[dispatchThreadID.xy] = selectedDepth;
		DestinationMotion[dispatchThreadID.xy] = selectedMotion * MotionVectorMultiplier;
		return;
	}

	uint gridWidth = GuidanceWidth > 0 ? GuidanceWidth : SourceWidth;
	uint gridHeight = GuidanceHeight > 0 ? GuidanceHeight : SourceHeight;
	float2 scale = float2(gridWidth, gridHeight) / float2(TargetWidth, TargetHeight);
	float2 guidePosition = (float2(dispatchThreadID.xy) + 0.5) * scale - 0.5;
	int2 guideBase = int2(floor(guidePosition));
	int2 guideMax = int2(gridWidth - 1, gridHeight - 1);
	float depthAcc = 0.0;
	float2 motionAcc = 0.0;
	float weightSum = 0.0;
	[unroll]
	for (int y = -1; y <= 2; ++y) {
		[unroll]
		for (int x = -1; x <= 2; ++x) {
			int2 guide = clamp(guideBase + int2(x, y), int2(0, 0), guideMax);
			int2 source = GuideToSource(guide);
			float depth = SourceDepth.Load(int3(source, 0));
			if (depth < MaskRaw)
				continue;
			float2 distance = (float2(guideBase + int2(x, y)) - guidePosition) / max(scale, 1.0);
			float weight = Filter == 1 ?
				Tent(distance.x) * Tent(distance.y) :
				Lanczos2(distance.x) * Lanczos2(distance.y);
			depthAcc += depth * weight;
			motionAcc += SourceMotion.Load(int3(source, 0)) * weight;
			weightSum += weight;
		}
	}
	if (weightSum < 1e-5) {
		DestinationDepth[dispatchThreadID.xy] = 1.0;
		DestinationMotion[dispatchThreadID.xy] = 0.0;
		return;
	}
	DestinationDepth[dispatchThreadID.xy] = depthAcc / weightSum;
	DestinationMotion[dispatchThreadID.xy] = motionAcc / weightSum * MotionVectorMultiplier;
}
