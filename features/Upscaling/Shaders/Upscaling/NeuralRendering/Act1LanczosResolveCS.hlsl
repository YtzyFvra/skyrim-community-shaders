Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> DestinationColor : register(u0);

cbuffer ResolveConstants : register(b0)
{
	uint SourceWidth;
	uint SourceHeight;
	uint DestinationWidth;
	uint DestinationHeight;
};

float Sinc(float value)
{
	if (abs(value) < 1e-5)
		return 1.0;
	float angle = 3.14159265359 * value;
	return sin(angle) / angle;
}

float Lanczos2(float value)
{
	value = abs(value);
	return value < 2.0 ? Sinc(value) * Sinc(value * 0.5) : 0.0;
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID)
{
	if (dispatchThreadID.x >= DestinationWidth || dispatchThreadID.y >= DestinationHeight)
		return;

	float2 scale = float2(SourceWidth, SourceHeight) / float2(DestinationWidth, DestinationHeight);
	float2 sourcePosition = (float2(dispatchThreadID.xy) + 0.5) * scale - 0.5;
	int2 sourceBase = int2(floor(sourcePosition));
	float4 color = 0.0;
	float weightSum = 0.0;
	[unroll]
	for (int y = -1; y <= 2; ++y) {
		[unroll]
		for (int x = -1; x <= 2; ++x) {
			int2 samplePosition = clamp(sourceBase + int2(x, y), int2(0, 0), int2(SourceWidth - 1, SourceHeight - 1));
			float2 distance = float2(sourceBase + int2(x, y)) - sourcePosition;
			float weight = Lanczos2(distance.x) * Lanczos2(distance.y);
			color += SourceColor.Load(int3(samplePosition, 0)) * weight;
			weightSum += weight;
		}
	}
	DestinationColor[dispatchThreadID.xy] = color / max(weightSum, 1e-5);
}