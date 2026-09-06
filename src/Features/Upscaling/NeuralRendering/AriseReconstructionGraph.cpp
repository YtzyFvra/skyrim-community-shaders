#include "AriseReconstructionGraph.h"

#include "Renderer.h"

#include <d3dcompiler.h>
#include <directx/d3dx12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <string>
#include <vector>

namespace NeuralRendering
{
	namespace
	{
		using Microsoft::WRL::ComPtr;

		constexpr DXGI_FORMAT kColorFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		constexpr UINT kDescriptorsPerSlot = 40;
		constexpr UINT kRootConstantDwords = 20;

		constexpr char kResampleShader[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceSize; uint2 DestinationSize; };
float Lanczos2(float x) { x=abs(x); if(x<1e-5)return 1; if(x>=2)return 0; float p=3.14159265359*x; return sin(p)*sin(p*.5)/(p*p*.5); }
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=DestinationSize))return; float2 scale=float2(SourceSize)/float2(DestinationSize);
 float2 position=(float2(id.xy)+.5)*scale-.5; int2 base=int2(floor(position)); float4 value=0; float sum=0;
 [unroll] for(int y=-1;y<=2;y++) [unroll] for(int x=-1;x<=2;x++) { int2 sample=clamp(base+int2(x,y),0,int2(SourceSize)-1);
  float2 distance=(float2(base+int2(x,y))-position)/max(scale,1); float weight=Lanczos2(distance.x)*Lanczos2(distance.y);
  value+=Source.Load(int3(sample,0))*weight; sum+=weight; }
 Destination[id.xy]=value/max(sum,1e-5);
})";

		constexpr char kResizeGuidesShader[] = R"(
Texture2D<float> SourceDepth : register(t0); Texture2D<float2> SourceMotion : register(t1);
RWTexture2D<float> DestinationDepth : register(u0); RWTexture2D<float2> DestinationMotion : register(u1);
cbuffer Constants : register(b0) { uint2 SourceSize; uint2 DestinationSize; float MotionVectorMultiplier; float3 Padding; };
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=DestinationSize))return;
 if(all(SourceSize==DestinationSize)){DestinationDepth[id.xy]=SourceDepth.Load(int3(id.xy,0));DestinationMotion[id.xy]=SourceMotion.Load(int3(id.xy,0))*MotionVectorMultiplier;return;}
 float2 sourcePosition=(float2(id.xy)+.5)*float2(SourceSize)/float2(DestinationSize)-.5;int2 base=int2(floor(sourcePosition));int2 sourceMax=int2(SourceSize)-1;
 int2 candidates[4]={clamp(base,0,sourceMax),clamp(base+int2(1,0),0,sourceMax),clamp(base+int2(0,1),0,sourceMax),clamp(base+int2(1,1),0,sourceMax)};
 float selectedDepth=1;float2 selectedMotion=0;
 [unroll]for(uint index=0;index<4;++index){float depth=SourceDepth.Load(int3(candidates[index],0));if(depth<selectedDepth){selectedDepth=depth;selectedMotion=SourceMotion.Load(int3(candidates[index],0));}}
 DestinationDepth[id.xy]=selectedDepth;DestinationMotion[id.xy]=selectedMotion*MotionVectorMultiplier;
})";

		constexpr char kFusionShader[] = R"(
Texture2D<float4> OriginalColor:register(t0); Texture2D<float4> NeuralColor:register(t1); Texture2D<float> DepthGuide:register(t2);
RWTexture2D<float4> OutputColor:register(u0);
cbuffer Constants:register(b0){uint2 FullSize;uint2 GuidanceSize;float LumaStrength;float ChromaStrength;float EdgeProtection;float ResidualLimit;
 uint Direction;uint DenoiseMode;float DenoiseStrength;float Softness;uint2 DepthSize;float FrequencyGuidance;float DepthGuidance;
 uint EnhancedGuidance;float ColorGuidance;float2 Padding;}; static const float3 LW=float3(.2126,.7152,.0722);
float4 OG(int2 p){return OriginalColor.Load(int3(clamp(p,0,int2(GuidanceSize)-1),0));}
float4 O(int2 p){float2 q=(float2(clamp(p,0,int2(FullSize)-1))+.5)*float2(GuidanceSize)/float2(FullSize)-.5;int2 b=int2(floor(q));float2 f=frac(q);
 return lerp(lerp(OG(b),OG(b+int2(1,0)),f.x),lerp(OG(b+int2(0,1)),OG(b+int2(1,1)),f.x),f.y);}
float4 N(int2 p){return NeuralColor.Load(int3(clamp(p,0,int2(FullSize)-1),0));}
float D(int2 p){return DepthGuide.Load(int3(clamp(p,0,int2(DepthSize)-1),0));}
float DepthConfidence(int2 p){if(!EnhancedGuidance||DepthGuidance<=0)return 1; int2 q=int2(round((float2(p)+.5)*float2(DepthSize)/float2(FullSize)-.5));
 float c=D(q),mn=c,mx=c;int2 r=max(1,int2(round(2*float2(DepthSize)/float2(FullSize))));float s[4]={D(q+int2(-r.x,0)),D(q+int2(r.x,0)),D(q+int2(0,-r.y)),D(q+int2(0,r.y))};
 [unroll]for(uint i=0;i<4;i++){mn=min(mn,s[i]);mx=max(mx,s[i]);}return lerp(1,lerp(1,.45,smoothstep(.002,.025,mx-mn)),saturate(DepthGuidance));}
float3 FilterN(int2 p){float3 c=N(p).rgb;if(!DenoiseMode||DenoiseStrength<=0)return c;float3 f=0;float wsum=0;float l=dot(c,LW);
 [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){float3 s=N(p+int2(x,y)).rgb;float sw=(x==0&&y==0)?1:((x==0||y==0)?.72:.48);
 float rw=DenoiseMode==2?1:exp2(-abs(dot(s,LW)-l)*(DenoiseMode==4?18:(DenoiseMode==1?42:28)));f+=s*sw*rw;wsum+=sw*rw;}f/=max(wsum,1e-5);
 if(DenoiseMode==3)f+=(l-dot(f,LW))*LW/dot(LW,LW);return lerp(c,f,saturate(DenoiseStrength*(DenoiseMode==4?1.25:1)));}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){if(any(id.xy>=FullSize))return;int2 p=id.xy;float4 o=O(p);float3 n=FilterN(p);
 float3 l=O(p+int2(-1,0)).rgb,r=O(p+int2(1,0)).rgb,u=O(p+int2(0,-1)).rgb,d=O(p+int2(0,1)).rgb;
 float3 base=o.rgb*.25+(l+r+u+d)*.125+(O(p+int2(-1,-1)).rgb+O(p+int2(1,-1)).rgb+O(p+int2(-1,1)).rgb+O(p+int2(1,1)).rgb)*.0625;
 float ol=dot(o.rgb,LW),bl=dot(base,LW),nl=dot(n,LW),gradient=abs(dot(r-l,LW))+abs(dot(d-u,LW));float confidence=1;
 if(EnhancedGuidance){float lumaMin=ol,lumaMax=ol;float3 blockColors[4]={O(p+int2(-2,0)).rgb,O(p+int2(2,0)).rgb,O(p+int2(0,-2)).rgb,O(p+int2(0,2)).rgb};
  [unroll]for(uint index=0;index<4;++index){float sampleLuma=dot(blockColors[index],LW);lumaMin=min(lumaMin,sampleLuma);lumaMax=max(lumaMax,sampleLuma);}
  float blockFrequency=abs(ol-bl)*4+(lumaMax-lumaMin)+gradient*.5;float fc=lerp(1,lerp(1,.45,smoothstep(.08,.45,blockFrequency)),saturate(FrequencyGuidance));
  float scale=max(ol,.08), disagreement=(abs(nl-ol)/scale)*.65+(length((n-nl)-(o.rgb-ol))/scale)*.35;
  confidence=max(.35,fc*lerp(1,lerp(1,.35,smoothstep(.08,.55,disagreement)),saturate(ColorGuidance))*DepthConfidence(p));}
 float guard=saturate(1-EdgeProtection*saturate(gradient*4))*confidence;float lr=clamp(nl-bl,-ResidualLimit,ResidualLimit);
 float rr=clamp((n.r-nl)-(base.r-bl),-ResidualLimit,ResidualLimit),br=clamp((n.b-nl)-(base.b-bl),-ResidualLimit,ResidualLimit);float3 result;
 if(Direction==0){float fl=ol+guard*LumaStrength*lr,fr=fl+(o.r-ol)+guard*ChromaStrength*rr,fb=fl+(o.b-ol)+guard*ChromaStrength*br;
  result=float3(fr,(fl-LW.r*fr-LW.b*fb)/LW.g,fb);}else{float3 detail=clamp(o.rgb-base,-ResidualLimit,ResidualLimit);float dl=dot(detail,LW);
  result=lerp(o.rgb,n+guard*saturate(1-Softness*.7)*(LumaStrength*dl+ChromaStrength*(detail-dl)),confidence);}
 result=lerp(result,lerp(base,n,Direction==0?.65:.85),saturate(Softness)*.45*confidence);OutputColor[id.xy]=float4(saturate(result),o.a);})";

		constexpr char kRcasShader[] = R"(
Texture2D<float4> Source:register(t0);RWTexture2D<float4> Destination:register(u0);cbuffer Constants:register(b0){float Sharpness;uint2 Size;float Padding;};
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){if(any(id.xy>=Size))return;int2 p=id.xy;float3 b=Source.Load(int3(clamp(p+int2(0,-1),0,int2(Size)-1),0)).rgb;
float3 d=Source.Load(int3(clamp(p+int2(-1,0),0,int2(Size)-1),0)).rgb,e=Source.Load(int3(p,0)).rgb;
float3 f=Source.Load(int3(clamp(p+int2(1,0),0,int2(Size)-1),0)).rgb,h=Source.Load(int3(clamp(p+int2(0,1),0,int2(Size)-1),0)).rgb;
float3 mn=min(min(b,d),min(f,h)),mx=max(max(b,d),max(f,h));float3 hitMin=min(mn,e)/max(4*mx,1e-5);
float3 hitMax=(1-max(mx,e))/min(4*mn-4,-1e-5);float lobe=max(-.1875,min(max(max(-hitMin.r,hitMax.r),max(-hitMin.g,hitMax.g)),max(-hitMin.b,hitMax.b)))*Sharpness;
float bl=b.b*.5+b.r*.5+b.g,dl=d.b*.5+d.r*.5+d.g,el=e.b*.5+e.r*.5+e.g,fl=f.b*.5+f.r*.5+f.g,hl=h.b*.5+h.r*.5+h.g;
float maximumLuma=max(max(max(bl,dl),max(el,fl)),hl),minimumLuma=min(min(min(bl,dl),min(el,fl)),hl);
float lumaRange=max(maximumLuma-minimumLuma,1e-5);
float noise=1-.5*saturate(abs(.25*(bl+dl+fl+hl)-el)/lumaRange);lobe*=noise;
Destination[p]=float4((lobe*(b+d+f+h)+e)/(4*lobe+1),1);})";

		ComPtr<ID3DBlob> Compile(const char* source, const char* name)
		{
			ComPtr<ID3DBlob> shader;
			ComPtr<ID3DBlob> errors;
			const HRESULT result = D3DCompile(source, std::strlen(source), name, nullptr, nullptr, "main", "cs_5_1",
				D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
			if (FAILED(result)) {
				logger::error("[ARISE Graph] {} compilation failed: {}", name,
					errors ? static_cast<const char*>(errors->GetBufferPointer()) : "unknown error");
				return nullptr;
			}
			return shader;
		}

		DXGI_FORMAT SrvFormat(DXGI_FORMAT format)
		{
			switch (format) {
			case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
			default: return format;
			}
		}
	}

	class AriseReconstructionGraph::State
	{
	public:
		struct Slot
		{
			ComPtr<ID3D12DescriptorHeap> heap;
			ComPtr<ID3D12Resource> colorN;
			ComPtr<ID3D12Resource> act1Dlss;
			ComPtr<ID3D12Resource> depthN;
			ComPtr<ID3D12Resource> motionN;
			ComPtr<ID3D12Resource> neuralN;
			ComPtr<ID3D12Resource> referenceD;
			ComPtr<ID3D12Resource> displayA;
			ComPtr<ID3D12Resource> displayB;
			ComPtr<ID3D12Resource> act2Dlss;
			std::uint32_t displayWidth = 0;
			std::uint32_t displayHeight = 0;
			std::uint32_t sceneInputWidth = 0;
			std::uint32_t sceneInputHeight = 0;
			std::uint32_t engineWidth = 0;
			std::uint32_t engineHeight = 0;
			std::uint32_t act1OutputWidth = 0;
			std::uint32_t act1OutputHeight = 0;
			std::uint32_t nrWidth = 0;
			std::uint32_t nrHeight = 0;
			DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT depthFormat = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
			UINT descriptorCursor = 0;
		};

		bool Initialize(ID3D12Device* value, DXGI_FORMAT format, std::size_t count, Streamline* streamline)
		{
			Shutdown();
			if (!value || count == 0)
				return false;
			device = value;
			if (!SupportsTypedUavStore(kColorFormat)) {
				logger::error("[ARISE Graph] R16G16B16A16_FLOAT typed UAV stores are unsupported");
				Shutdown();
				return false;
			}
			displayFormat = format;
			dlss.Initialize(streamline, value);
			slots.resize(count);
			D3D12_DESCRIPTOR_RANGE ranges[2]{};
			ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0 };
			ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3 };
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants = { 0, 0, kRootConstantDwords };
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable = { 2, ranges };
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			D3D12_ROOT_SIGNATURE_DESC desc{ 2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
			ComPtr<ID3DBlob> serialized;
			ComPtr<ID3DBlob> errors;
			if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
				FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
					IID_PPV_ARGS(&rootSignature))))
				return false;
			return CreatePipeline(kResampleShader, "AriseLanczos", resample) &&
				CreatePipeline(kResizeGuidesShader, "AriseResizeGuides", resizeGuides) &&
				CreatePipeline(kFusionShader, "AriseFusion", fusion) && CreatePipeline(kRcasShader, "AriseRCAS", rcas);
		}

		void Shutdown()
		{
			dlss.Shutdown();
			slots.clear();
			resample.Reset();
			resizeGuides.Reset();
			fusion.Reset();
			rcas.Reset();
			rootSignature.Reset();
			device.Reset();
			activePath.store(0, std::memory_order_relaxed);
			forceReset = true;
		}

		void Reset()
		{
			forceReset = true;
			dlss.Reset();
		}

		bool Ensure(std::size_t index, const Inputs& inputs, const Plan& plan)
		{
			if (!device || index >= slots.size() || !inputs.scene || !inputs.depth || !inputs.motionVectors ||
				!plan.sceneInputWidth || !plan.sceneInputHeight || !plan.engineWidth || !plan.engineHeight ||
				!plan.displayWidth || !plan.displayHeight ||
				!plan.act1OutputWidth || !plan.act1OutputHeight || !plan.nrWidth || !plan.nrHeight)
				return false;
			auto& slot = slots[index];
			const auto sceneDesc = inputs.scene->GetDesc();
			const auto depthDesc = inputs.depth->GetDesc();
			const auto motionDesc = inputs.motionVectors->GetDesc();
			if (sceneDesc.Width != plan.sceneInputWidth || sceneDesc.Height != plan.sceneInputHeight ||
				sceneDesc.Format != displayFormat ||
				depthDesc.Width != plan.sceneInputWidth || depthDesc.Height != plan.sceneInputHeight ||
				motionDesc.Width != plan.sceneInputWidth || motionDesc.Height != plan.sceneInputHeight)
				return false;
			if (slot.displayWidth == plan.displayWidth && slot.displayHeight == plan.displayHeight &&
				slot.sceneInputWidth == plan.sceneInputWidth && slot.sceneInputHeight == plan.sceneInputHeight &&
				slot.engineWidth == plan.engineWidth && slot.engineHeight == plan.engineHeight &&
				slot.act1OutputWidth == plan.act1OutputWidth && slot.act1OutputHeight == plan.act1OutputHeight &&
				slot.nrWidth == plan.nrWidth && slot.nrHeight == plan.nrHeight &&
				slot.sceneFormat == sceneDesc.Format &&
				slot.depthFormat == depthDesc.Format &&
				slot.motionFormat == motionDesc.Format)
				return true;
			DXGI_FORMAT motionOutputFormat = DXGI_FORMAT_UNKNOWN;
			for (const DXGI_FORMAT candidate : { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32G32_FLOAT }) {
				if (SupportsTypedUavStore(candidate)) {
					motionOutputFormat = candidate;
					break;
				}
			}
			if (!SupportsTypedUavStore(DXGI_FORMAT_R32_FLOAT) || motionOutputFormat == DXGI_FORMAT_UNKNOWN) {
				logger::error("[ARISE Graph] Required typed UAV stores are unsupported (depth R32_FLOAT, motion float2)");
				return false;
			}
			slot = {};
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				kDescriptorsPerSlot, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&slot.heap))) ||
				!CreateTexture(slot.colorN, plan.nrWidth, plan.nrHeight, kColorFormat, L"ARISE Graph Color N") ||
				!CreateTexture(slot.act1Dlss, plan.act1OutputWidth, plan.act1OutputHeight, kColorFormat, L"ARISE Graph ACT1 DLSS") ||
				!CreateTexture(slot.depthN, plan.nrWidth, plan.nrHeight, DXGI_FORMAT_R32_FLOAT, L"ARISE Graph Depth N") ||
				!CreateTexture(slot.motionN, plan.nrWidth, plan.nrHeight, motionOutputFormat, L"ARISE Graph Motion N") ||
				!CreateTexture(slot.neuralN, plan.nrWidth, plan.nrHeight, kColorFormat, L"ARISE Graph Neural N") ||
				!CreateTexture(slot.referenceD, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Reference D") ||
				!CreateTexture(slot.displayA, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Display A") ||
				!CreateTexture(slot.displayB, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Display B") ||
				!CreateTexture(slot.act2Dlss, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph ACT2 DLSS"))
				return false;
			slot.displayWidth = plan.displayWidth;
			slot.displayHeight = plan.displayHeight;
			slot.sceneInputWidth = plan.sceneInputWidth;
			slot.sceneInputHeight = plan.sceneInputHeight;
			slot.engineWidth = plan.engineWidth;
			slot.engineHeight = plan.engineHeight;
			slot.act1OutputWidth = plan.act1OutputWidth;
			slot.act1OutputHeight = plan.act1OutputHeight;
			slot.nrWidth = plan.nrWidth;
			slot.nrHeight = plan.nrHeight;
			slot.sceneFormat = sceneDesc.Format;
			slot.depthFormat = depthDesc.Format;
			slot.motionFormat = motionDesc.Format;
			forceReset = true;
			return true;
		}

		Result Record(ID3D12GraphicsCommandList* list, std::size_t index, const Inputs& inputs, const Plan& plan)
		{
			Result result{ inputs.scene, D3D12_RESOURCE_STATE_COMMON, false };
			if (!list)
				return result;
			if (!plan.eligible) {
				activePath.store(0, std::memory_order_relaxed);
				if (plan.requested) {
					++unsupported;
					++fallback;
				}
				return result;
			}
			if (!Ensure(index, inputs, plan)) {
				++failed;
				return result;
			}
			auto& slot = slots[index];
			slot.descriptorCursor = 0;
			ID3D12Resource* act1Color = inputs.scene;
			std::uint32_t act1Width = plan.sceneInputWidth;
			std::uint32_t act1Height = plan.sceneInputHeight;
			bool act1DlssSucceeded = false;
			if (plan.renderDocReplacement && plan.act1 == Act1Backend::kDLSS) {
				act1DlssSucceeded = DispatchResample(list, slot, inputs.scene, slot.act1Dlss.Get(),
					plan.sceneInputWidth, plan.sceneInputHeight, plan.act1OutputWidth, plan.act1OutputHeight,
					SrvFormat(inputs.scene->GetDesc().Format), kColorFormat);
			} else if (plan.act1 == Act1Backend::kDLSS) {
				act1DlssSucceeded = dlss.Record(list, {
					.frameId = plan.frameId, .viewport = AriseDlssProvider::kAct1Viewport,
					.inputWidth = plan.sceneInputWidth, .inputHeight = plan.sceneInputHeight,
					.outputWidth = plan.act1OutputWidth, .outputHeight = plan.act1OutputHeight,
					.quality = plan.act1Quality, .preset = plan.act1Preset, .reset = plan.reset,
					.camera = plan.act1Camera,
					.color = inputs.scene, .output = slot.act1Dlss.Get(), .depth = inputs.depth,
					.motionVectors = inputs.motionVectors });
			}
			if (act1DlssSucceeded) {
				act1Color = slot.act1Dlss.Get();
				act1Width = plan.act1OutputWidth;
				act1Height = plan.act1OutputHeight;
			} else if (plan.act1 != Act1Backend::kDLSS) {
				++act1Bypass;
			} else {
				// A failed ACT1 uses this frame's sealed scene; retaining an older DLSS output would freeze temporal content.
				++fallback;
			}
			const DXGI_FORMAT act1Format = act1Color == slot.act1Dlss.Get() ? kColorFormat :
				SrvFormat(inputs.scene->GetDesc().Format);
			if (!DispatchResample(list, slot, act1Color, slot.colorN.Get(), act1Width, act1Height,
					plan.nrWidth, plan.nrHeight, act1Format, kColorFormat) ||
				!DispatchResizeGuides(list, slot, inputs.depth, inputs.motionVectors, slot.depthN.Get(), slot.motionN.Get(),
					plan.sceneInputWidth, plan.sceneInputHeight, plan.nrWidth, plan.nrHeight)) {
				++failed;
				return result;
			}
			++lanczos;
			bool nrSucceeded = false;
			if (plan.renderDocReplacement) {
				nrSucceeded = DispatchResample(list, slot, slot.colorN.Get(), slot.neuralN.Get(),
					plan.nrWidth, plan.nrHeight, plan.nrWidth, plan.nrHeight, kColorFormat, kColorFormat);
			} else {
				Transition(list, slot.colorN.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				Transition(list, slot.depthN.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				Transition(list, slot.motionN.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				Transition(list, slot.neuralN.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				nrSucceeded = Renderer::Instance().ExecuteModel3(list, slot.colorN.Get(), slot.depthN.Get(),
					slot.motionN.Get(), slot.neuralN.Get(), plan.nrWidth, plan.nrHeight,
					plan.motionVectorScaleX, plan.motionVectorScaleY, plan.tuning, plan.reset || forceReset);
				Transition(list, slot.colorN.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.depthN.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.motionN.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.neuralN.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			}
			forceReset = !nrSucceeded;
			ID3D12Resource* neural = nrSucceeded ? slot.neuralN.Get() : slot.colorN.Get();
			if (nrSucceeded)
				++neuralRendering;
			else {
				++failed;
				++fallback;
			}
			if (!DispatchResample(list, slot, neural, slot.displayA.Get(), plan.nrWidth, plan.nrHeight,
					plan.displayWidth, plan.displayHeight, kColorFormat, kColorFormat)) {
				++failed;
				return result;
			}
			++lanczos;
			ID3D12Resource* finalColor = slot.displayA.Get();
			if (plan.act2Sr == Act2SrBackend::kDLSS) {
				if (plan.renderDocReplacement && DispatchResample(list, slot, neural, slot.act2Dlss.Get(),
						plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight,
						kColorFormat, kColorFormat))
					finalColor = slot.act2Dlss.Get();
				else if (!plan.renderDocReplacement && dlss.Record(list, {
					.frameId = plan.frameId, .viewport = AriseDlssProvider::kAct2Viewport,
					.inputWidth = plan.nrWidth, .inputHeight = plan.nrHeight,
					.outputWidth = plan.displayWidth, .outputHeight = plan.displayHeight,
					.quality = plan.act2Quality, .preset = plan.act2Preset, .reset = plan.reset,
					.camera = plan.act2Camera,
					.color = neural, .output = slot.act2Dlss.Get(), .depth = slot.depthN.Get(),
					.motionVectors = slot.motionN.Get() }))
					finalColor = slot.act2Dlss.Get();
				else {
					// displayA is this frame's NR result upscaled with Lanczos, never a cached prior-frame DLSS output.
					++fallback;
				}
			}
			bool fusionSucceeded = false;
			if (plan.fusionEnabled) {
				ID3D12Resource* referenceG = act1DlssSucceeded ? slot.act1Dlss.Get() : slot.referenceD.Get();
				const std::uint32_t referenceWidth = act1DlssSucceeded ? plan.act1OutputWidth : plan.displayWidth;
				const std::uint32_t referenceHeight = act1DlssSucceeded ? plan.act1OutputHeight : plan.displayHeight;
				const bool referenceReady = act1DlssSucceeded || DispatchResample(list, slot, inputs.scene,
					slot.referenceD.Get(), plan.sceneInputWidth, plan.sceneInputHeight,
					plan.displayWidth, plan.displayHeight, SrvFormat(inputs.scene->GetDesc().Format), kColorFormat);
				if (referenceReady && DispatchFusion(list, slot, referenceG, finalColor, inputs.depth,
					slot.displayB.Get(), referenceWidth, referenceHeight, plan)) {
					finalColor = slot.displayB.Get();
					fusionSucceeded = true;
					++fusionCount;
				} else {
					++failed;
					++fallback;
				}
			}
			bool rcasSucceeded = false;
			if (plan.sharpen) {
				ID3D12Resource* target = finalColor == slot.displayA.Get() ? slot.displayB.Get() : slot.displayA.Get();
				if (DispatchRcas(list, slot, finalColor, target, plan)) {
					finalColor = target;
					rcasSucceeded = true;
					++rcasCount;
				} else {
					++failed;
					++fallback;
				}
			}
			activePath.store(fusionSucceeded ? (rcasSucceeded ? 4u : 3u) :
				(rcasSucceeded ? 2u : 1u), std::memory_order_relaxed);
			result = { finalColor, D3D12_RESOURCE_STATE_COMMON, nrSucceeded };
			return result;
		}

		Statistics Stats() const
		{
			return { act1Bypass.load(), dlss.GetStatistics(), lanczos.load(), neuralRendering.load(), fusionCount.load(), rcasCount.load(),
				unsupported.load(), fallback.load(), failed.load() };
		}

		bool IsDlssAvailable() const { return dlss.IsAvailable(); }

		const char* Path() const
		{
			switch (activePath.load(std::memory_order_relaxed)) {
			case 1: return "Bypass > N > NR > Lanczos";
			case 2: return "Bypass > N > NR > Lanczos > RCAS";
			case 3: return "Bypass > N > NR > Fusion";
			case 4: return "Bypass > N > NR > Fusion > RCAS";
			default: return "passthrough";
			}
		}

	private:
		bool SupportsTypedUavStore(DXGI_FORMAT format) const
		{
			D3D12_FEATURE_DATA_FORMAT_SUPPORT support{ format };
			return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
				(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
		}

		bool CreatePipeline(const char* source, const char* name, ComPtr<ID3D12PipelineState>& output)
		{
			auto shader = Compile(source, name);
			if (!shader)
				return false;
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
			desc.pRootSignature = rootSignature.Get();
			desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
			return SUCCEEDED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&output)));
		}

		bool CreateTexture(ComPtr<ID3D12Resource>& output, std::uint32_t width, std::uint32_t height,
			DXGI_FORMAT format, const wchar_t* name)
		{
			D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
			auto desc = CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0,
				D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
			if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
					D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&output))))
				return false;
			output->SetName(name);
			return true;
		}

		void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
			D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
		{
			if (before == after)
				return;
			auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, before, after);
			list->ResourceBarrier(1, &barrier);
		}

		D3D12_GPU_DESCRIPTOR_HANDLE BindViews(Slot& slot, ID3D12Resource* source0, DXGI_FORMAT source0Format,
			ID3D12Resource* destination, DXGI_FORMAT destinationFormat,
			ID3D12Resource* source1 = nullptr, DXGI_FORMAT source1Format = DXGI_FORMAT_UNKNOWN,
			ID3D12Resource* source2 = nullptr, DXGI_FORMAT source2Format = DXGI_FORMAT_UNKNOWN)
		{
			if (!slot.heap || slot.descriptorCursor > kDescriptorsPerSlot - 5)
				return {};
			const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto handle = slot.heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(slot.descriptorCursor) * stride;
			auto gpuHandle = slot.heap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<UINT64>(slot.descriptorCursor) * stride;
			for (auto [resource, format] : std::array<std::pair<ID3D12Resource*, DXGI_FORMAT>, 3>{ {
				{ source0, source0Format }, { source1, source1Format }, { source2, source2Format } } }) {
				D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
				desc.Format = resource ? format : kColorFormat;
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				desc.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(resource, &desc, handle);
				handle.ptr += stride;
			}
			D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
			uav.Format = destinationFormat;
			uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device->CreateUnorderedAccessView(destination, nullptr, &uav, handle);
			handle.ptr += stride;
			uav.Format = kColorFormat;
			device->CreateUnorderedAccessView(nullptr, nullptr, &uav, handle);
			slot.descriptorCursor += 5;
			return gpuHandle;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE BindGuideViews(Slot& slot, ID3D12Resource* depth, DXGI_FORMAT depthFormat,
			ID3D12Resource* motion, DXGI_FORMAT motionFormat, ID3D12Resource* destinationDepth,
			ID3D12Resource* destinationMotion)
		{
			if (!slot.heap || slot.descriptorCursor > kDescriptorsPerSlot - 5)
				return {};
			const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto handle = slot.heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(slot.descriptorCursor) * stride;
			auto gpuHandle = slot.heap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<UINT64>(slot.descriptorCursor) * stride;
			for (auto [resource, format] : std::array<std::pair<ID3D12Resource*, DXGI_FORMAT>, 3>{ {
				{ depth, depthFormat }, { motion, motionFormat }, { nullptr, kColorFormat } } }) {
				D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
				desc.Format = format;
				desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				desc.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(resource, &desc, handle);
				handle.ptr += stride;
			}
			for (ID3D12Resource* destination : { destinationDepth, destinationMotion }) {
				D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
				desc.Format = destination->GetDesc().Format;
				desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(destination, nullptr, &desc, handle);
				handle.ptr += stride;
			}
			slot.descriptorCursor += 5;
			return gpuHandle;
		}

		bool BeginDispatch(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12PipelineState* pipeline,
			const void* constants, UINT constantCount, D3D12_GPU_DESCRIPTOR_HANDLE views)
		{
			if (!pipeline || !views.ptr || constantCount > kRootConstantDwords)
				return false;
			ID3D12DescriptorHeap* heaps[]{ slot.heap.Get() };
			list->SetDescriptorHeaps(1, heaps);
			list->SetComputeRootSignature(rootSignature.Get());
			list->SetPipelineState(pipeline);
			list->SetComputeRoot32BitConstants(0, constantCount, constants, 0);
			list->SetComputeRootDescriptorTable(1, views);
			return true;
		}

		bool DispatchResample(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* source,
			ID3D12Resource* destination, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight,
			DXGI_FORMAT sourceFormat, DXGI_FORMAT destinationFormat)
		{
			if (!source || !destination)
				return false;
			const std::uint32_t constants[]{ sourceWidth, sourceHeight, destinationWidth, destinationHeight };
			const auto views = BindViews(slot, source, sourceFormat, destination, destinationFormat);
			if (!views.ptr)
				return false;
			Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, resample.Get(), constants, 4, views))
				return false;
			list->Dispatch((destinationWidth + 7) / 8, (destinationHeight + 7) / 8, 1);
			Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		bool DispatchResizeGuides(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* depth,
			ID3D12Resource* motion, ID3D12Resource* destinationDepth, ID3D12Resource* destinationMotion,
			std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight)
		{
			if (!depth || !motion || !destinationDepth || !destinationMotion)
				return false;
			struct Constants { std::uint32_t source[2], destination[2]; float multiplier, padding[3]; } constants{
				{ sourceWidth, sourceHeight }, { destinationWidth, destinationHeight }, 1.0f, {} };
			const auto views = BindGuideViews(slot, depth, SrvFormat(depth->GetDesc().Format),
				motion, SrvFormat(motion->GetDesc().Format), destinationDepth, destinationMotion);
			if (!views.ptr)
				return false;
			for (auto* resource : { depth, motion })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			for (auto* resource : { destinationDepth, destinationMotion })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, resizeGuides.Get(), &constants, sizeof(constants) / sizeof(std::uint32_t), views))
				return false;
			list->Dispatch((destinationWidth + 7) / 8, (destinationHeight + 7) / 8, 1);
			for (auto* resource : { depth, motion })
				Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			for (auto* resource : { destinationDepth, destinationMotion })
				Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		bool DispatchFusion(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* original,
			ID3D12Resource* neural, ID3D12Resource* depth, ID3D12Resource* destination,
			std::uint32_t originalWidth, std::uint32_t originalHeight, const Plan& plan)
		{
			struct Constants
			{
				std::uint32_t full[2], neural[2];
				float luma, chroma, edge, residual;
				std::uint32_t direction, denoise;
				float denoiseStrength, softness;
				std::uint32_t depth[2];
				float frequency, depthGuidance;
				std::uint32_t enhanced;
				float color, padding[2];
			} constants{ { plan.displayWidth, plan.displayHeight }, { originalWidth, originalHeight },
				plan.fusion.luma, plan.fusion.chroma, plan.fusion.edgeProtection, plan.fusion.residualLimit,
				plan.fusion.direction, plan.fusion.denoise, plan.fusion.denoiseStrength, plan.fusion.softness,
				{ plan.sceneInputWidth, plan.sceneInputHeight }, plan.fusion.frequencyGuidance, plan.fusion.depthGuidance,
				plan.fusion.enhancedGuidance, plan.fusion.colorGuidance, {} };
			static_assert(sizeof(Constants) == kRootConstantDwords * sizeof(std::uint32_t));
			const auto views = BindViews(slot, original, SrvFormat(original->GetDesc().Format), destination, kColorFormat,
				neural, kColorFormat, depth, SrvFormat(depth->GetDesc().Format));
			if (!views.ptr)
				return false;
			for (auto* resource : { original, neural, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, fusion.Get(), &constants, sizeof(constants) / sizeof(std::uint32_t), views))
				return false;
			list->Dispatch((plan.displayWidth + 7) / 8, (plan.displayHeight + 7) / 8, 1);
			for (auto* resource : { original, neural, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		bool DispatchRcas(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* source,
			ID3D12Resource* destination, const Plan& plan)
		{
			struct Constants { float sharpness; std::uint32_t width, height; float padding; } constants{
				plan.sharpness, plan.displayWidth, plan.displayHeight, 0.0f };
			const auto views = BindViews(slot, source, kColorFormat, destination, kColorFormat);
			if (!views.ptr)
				return false;
			Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, rcas.Get(), &constants, 4, views))
				return false;
			list->Dispatch((plan.displayWidth + 7) / 8, (plan.displayHeight + 7) / 8, 1);
			Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		ComPtr<ID3D12Device> device;
		AriseDlssProvider dlss;
		DXGI_FORMAT displayFormat = DXGI_FORMAT_UNKNOWN;
		ComPtr<ID3D12RootSignature> rootSignature;
		ComPtr<ID3D12PipelineState> resample;
		ComPtr<ID3D12PipelineState> resizeGuides;
		ComPtr<ID3D12PipelineState> fusion;
		ComPtr<ID3D12PipelineState> rcas;
		std::vector<Slot> slots;
		std::atomic_uint64_t act1Bypass = 0;
		std::atomic_uint64_t lanczos = 0;
		std::atomic_uint64_t neuralRendering = 0;
		std::atomic_uint64_t fusionCount = 0;
		std::atomic_uint64_t rcasCount = 0;
		std::atomic_uint64_t unsupported = 0;
		std::atomic_uint64_t fallback = 0;
		std::atomic_uint64_t failed = 0;
		std::atomic_uint32_t activePath = 0;
		bool forceReset = true;
	};

	AriseReconstructionGraph::AriseReconstructionGraph() : state_(std::make_unique<State>()) {}
	AriseReconstructionGraph::~AriseReconstructionGraph() = default;
	bool AriseReconstructionGraph::Initialize(ID3D12Device* device, DXGI_FORMAT format, std::size_t count,
		Streamline* streamline)
	{
		return state_->Initialize(device, format, count, streamline);
	}
	void AriseReconstructionGraph::Shutdown() { state_->Shutdown(); }
	void AriseReconstructionGraph::Reset() { state_->Reset(); }
	bool AriseReconstructionGraph::EnsureSlotResources(std::size_t index, const Inputs& inputs, const Plan& plan)
	{
		return state_->Ensure(index, inputs, plan);
	}
	AriseReconstructionGraph::Result AriseReconstructionGraph::Record(ID3D12GraphicsCommandList* list,
		std::size_t index, const Inputs& inputs, const Plan& plan)
	{
		return state_->Record(list, index, inputs, plan);
	}
	AriseReconstructionGraph::Statistics AriseReconstructionGraph::GetStatistics() const { return state_->Stats(); }
	bool AriseReconstructionGraph::IsDlssAvailable() const { return state_->IsDlssAvailable(); }
	const char* AriseReconstructionGraph::ActivePath() const { return state_->Path(); }
}