// Copy a depth SRV (R24_UNORM read as float [0,1]) into an R32_FLOAT UAV with
// independent src/dst offsets. D3D11 forbids CopySubresourceRegion of a DS crop.

Texture2D<float> SrcDepth : register(t0);
RWTexture2D<float> DstDepth : register(u0);

cbuffer DepthCopyCB : register(b0)
{
	uint srcOffsetX;
	uint srcOffsetY;
	uint dstOffsetX;
	uint dstOffsetY;
	uint copyWidth;
	uint copyHeight;
	uint2 pad;
};

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
	if (tid.x >= copyWidth || tid.y >= copyHeight)
		return;

	DstDepth[uint2(dstOffsetX + tid.x, dstOffsetY + tid.y)] =
		SrcDepth[uint2(srcOffsetX + tid.x, srcOffsetY + tid.y)];
}
