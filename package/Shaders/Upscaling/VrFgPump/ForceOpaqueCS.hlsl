RWTexture2D<float4> Dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width = 0;
	uint height = 0;
	Dest.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	float4 color = Dest[id.xy];
	Dest[id.xy] = float4(color.rgb, 1.0);
}
