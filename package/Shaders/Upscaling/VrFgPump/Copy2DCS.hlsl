Texture2D<float4> Src : register(t0);
RWTexture2D<float4> Dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width = 0;
	uint height = 0;
	Dest.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	Dest[id.xy] = Src[id.xy];
}
