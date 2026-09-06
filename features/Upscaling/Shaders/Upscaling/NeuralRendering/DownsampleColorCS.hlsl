Texture2D<float4> SourceColor : register(t0);
RWTexture2D<float4> DestinationColor : register(u0);

cbuffer DownsampleConstants : register(b0)
{
	uint SourceWidth;
	uint SourceHeight;
	uint DestinationWidth;
	uint DestinationHeight;
};

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
			int2 samplePixel = clamp(sourceBase + int2(x, y), int2(0, 0), int2(SourceWidth - 1, SourceHeight - 1));
			float2 distance = (float2(sourceBase + int2(x, y)) - sourcePosition) / max(scale, 1.0);
			float weight = Lanczos2(distance.x) * Lanczos2(distance.y);
			color += SourceColor.Load(int3(samplePixel, 0)) * weight;
			weightSum += weight;
		}
	}
	DestinationColor[dispatchThreadID.xy] = color / max(weightSum, 1e-5);
}