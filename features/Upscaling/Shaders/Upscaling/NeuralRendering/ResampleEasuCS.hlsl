Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> DestinationColor : register(u0);

cbuffer DownsampleConstants : register(b0)
{
	uint SourceWidth;
	uint SourceHeight;
	uint DestinationWidth;
	uint DestinationHeight;
};

static const float3 LumaWeights = float3(0.2126, 0.7152, 0.0722);

float3 LoadSource(int2 pixel)
{
	pixel = clamp(pixel, int2(0, 0), int2(SourceWidth - 1, SourceHeight - 1));
	return SourceColor.Load(int3(pixel, 0)).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= DestinationWidth || dispatchThreadID.y >= DestinationHeight)
		return;

	float2 scale = float2(SourceWidth, SourceHeight) / float2(DestinationWidth, DestinationHeight);
	float2 sourcePosition = (float2(dispatchThreadID.xy) + 0.5) * scale - 0.5;
	int2 sourceBase = int2(floor(sourcePosition));
	float2 fraction = frac(sourcePosition);

	static const int2 offsets[12] = {
		int2(0, -1), int2(1, -1),
		int2(-1, 0), int2(0, 0), int2(1, 0), int2(2, 0),
		int2(-1, 1), int2(0, 1), int2(1, 1), int2(2, 1),
		int2(0, 2), int2(1, 2)
	};
	float3 samples[12];
	float luma[12];
	[unroll]
	for (uint tap = 0; tap < 12; ++tap) {
		samples[tap] = LoadSource(sourceBase + offsets[tap]);
		luma[tap] = dot(samples[tap], LumaWeights);
	}

	float2 direction = float2(
		(luma[4] + luma[8]) - (luma[3] + luma[7]),
		(luma[7] + luma[8]) - (luma[3] + luma[4]));
	float directionLength = length(direction);
	direction = directionLength > 1e-5 ? direction / directionLength : float2(1.0, 0.0);
	float anisotropy = saturate(directionLength * 4.0);
	float2 orthogonal = float2(-direction.y, direction.x);

	float3 accumulated = 0.0;
	float weightSum = 0.0;
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
	DestinationColor[dispatchThreadID.xy] = float4(clamp(result, localMin, localMax), 1.0);
}
