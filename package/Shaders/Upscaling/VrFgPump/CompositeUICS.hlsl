Texture2D<float4> UiTex : register(t0);
RWTexture2D<float4> Dest : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
	uint width = 0;
	uint height = 0;
	Dest.GetDimensions(width, height);
	if (id.x >= width || id.y >= height)
		return;
	const float4 ui = UiTex[id.xy];
	const float4 scene = Dest[id.xy];
	Dest[id.xy] = float4(scene.rgb * (1.0 - ui.a) + ui.rgb, 1.0);
}
