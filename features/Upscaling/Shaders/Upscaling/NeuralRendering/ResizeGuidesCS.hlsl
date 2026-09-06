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
	float MotionVectorMultiplier;
	float3 Padding;
};

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

	float2 sourcePosition = (float2(dispatchThreadID.xy) + 0.5) *
		float2(SourceWidth, SourceHeight) / float2(TargetWidth, TargetHeight) - 0.5;
	int2 base = int2(floor(sourcePosition));
	int2 sourceMax = int2(SourceWidth - 1, SourceHeight - 1);
	int2 candidates[4] = {
		clamp(base, int2(0, 0), sourceMax),
		clamp(base + int2(1, 0), int2(0, 0), sourceMax),
		clamp(base + int2(0, 1), int2(0, 0), sourceMax),
		clamp(base + int2(1, 1), int2(0, 0), sourceMax)
	};

	float selectedDepth = 1.0;
	float2 selectedMotion = 0.0;
	[unroll]
	for (uint index = 0; index < 4; ++index) {
		float depth = SourceDepth.Load(int3(candidates[index], 0));
		if (depth < selectedDepth) {
			selectedDepth = depth;
			selectedMotion = SourceMotion.Load(int3(candidates[index], 0));
		}
	}
	DestinationDepth[dispatchThreadID.xy] = selectedDepth;
	DestinationMotion[dispatchThreadID.xy] = selectedMotion * MotionVectorMultiplier;
}