// Solid fill of a private owned-Submit SBS. BandWidth/BandHeight is the write rect.

cbuffer BandCB : register(b0)
{
	float4 BandColor;
	uint BandWidth;
	uint BandHeight;
	uint2 pad;
};

RWTexture2D<float4> Dst : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= BandWidth || tid.y >= BandHeight)
		return;
	Dst[tid.xy] = BandColor;
}
