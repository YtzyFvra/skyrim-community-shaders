#include "AriseReconstructionGraph.h"

#include "../VrPassTimer.h"

#include "Renderer.h"
#include "Utils/Game.h"

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
		constexpr UINT kDescriptorsPerSlot = 140;
		constexpr UINT kSrvsPerBind = 8;
		constexpr UINT kDescriptorsPerBind = kSrvsPerBind + 2;
		constexpr UINT kRootConstantDwords = 32;

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

		constexpr char kTentShader[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceSize; uint2 DestinationSize; };
float Tent(float x){return saturate(1-abs(x));}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=DestinationSize))return; float2 scale=float2(SourceSize)/float2(DestinationSize);
 float2 position=(float2(id.xy)+.5)*scale-.5; int2 base=int2(floor(position)); float4 value=0; float sum=0;
 [unroll] for(int y=-1;y<=2;y++) [unroll] for(int x=-1;x<=2;x++){ int2 sample=clamp(base+int2(x,y),0,int2(SourceSize)-1);
  float2 distance=(float2(base+int2(x,y))-position)/max(scale,1); float weight=Tent(distance.x)*Tent(distance.y);
  value+=Source.Load(int3(sample,0))*weight; sum+=weight; }
 Destination[id.xy]=value/max(sum,1e-5);
})";

		constexpr char kMitchellShader[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceSize; uint2 DestinationSize; };
float Mitchell(float x){x=abs(x);const float B=1./3.,C=1./3.;
 if(x<1)return((12-9*B-6*C)*x*x*x+(-18+12*B+6*C)*x*x+(6-2*B))/6;
 if(x<2)return((-B-6*C)*x*x*x+(6*B+30*C)*x*x+(-12*B-48*C)*x+(8*B+24*C))/6;return 0;}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=DestinationSize))return; float2 scale=float2(SourceSize)/float2(DestinationSize);
 float2 position=(float2(id.xy)+.5)*scale-.5; int2 base=int2(floor(position)); float4 value=0; float sum=0;
 [unroll] for(int y=-1;y<=2;y++) [unroll] for(int x=-1;x<=2;x++){ int2 sample=clamp(base+int2(x,y),0,int2(SourceSize)-1);
  float2 distance=float2(base+int2(x,y))-position; float weight=Mitchell(distance.x)*Mitchell(distance.y);
  value+=Source.Load(int3(sample,0))*weight; sum+=weight; }
 Destination[id.xy]=value/max(sum,1e-5);
})";

		constexpr char kEasuShader[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Constants : register(b0) { uint2 SourceSize; uint2 DestinationSize; };
static const float3 LW=float3(.2126,.7152,.0722);
float3 L(int2 p){return Source.Load(int3(clamp(p,0,int2(SourceSize)-1),0)).rgb;}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=DestinationSize))return; float2 scale=float2(SourceSize)/float2(DestinationSize);
 float2 position=(float2(id.xy)+.5)*scale-.5; int2 base=int2(floor(position)); float2 f=frac(position);
 static const int2 o[12]={int2(0,-1),int2(1,-1),int2(-1,0),int2(0,0),int2(1,0),int2(2,0),int2(-1,1),int2(0,1),int2(1,1),int2(2,1),int2(0,2),int2(1,2)};
 float3 s[12]; float luma[12];
 [unroll] for(uint i=0;i<12;i++){s[i]=L(base+o[i]);luma[i]=dot(s[i],LW);}
 float2 dir=float2((luma[4]+luma[8])-(luma[3]+luma[7]),(luma[7]+luma[8])-(luma[3]+luma[4]));
 float len=length(dir); dir=len>1e-5?dir/len:float2(1,0); float aniso=saturate(len*4); float2 orth=float2(-dir.y,dir.x);
 float3 acc=0; float sum=0;
 [unroll] for(uint i=0;i<12;i++){ float2 d=float2(o[i])-f; float along=dot(d,dir),across=dot(d,orth);
  float d2=along*along*lerp(1,.55,aniso)+across*across*lerp(1,1.8,aniso); float w=saturate(1-d2*.25); w=w*w*(3-2*w);
  acc+=s[i]*w; sum+=w; }
 float3 result=acc/max(sum,1e-5); float3 mn=min(min(s[3],s[4]),min(s[7],s[8])), mx=max(max(s[3],s[4]),max(s[7],s[8]));
 Destination[id.xy]=float4(clamp(result,mn,mx),1);
})";

		constexpr char kResizeGuidesShader[] = R"(
Texture2D<float> SourceDepth:register(t0);Texture2D<float2> SourceMotion:register(t1);
RWTexture2D<float> DestinationDepth:register(u0);RWTexture2D<float2> DestinationMotion:register(u1);
cbuffer Constants:register(b0){uint2 SourceSize;uint2 DestinationSize;uint2 GuidanceSize;uint Filter;float MotionVectorMultiplier;};
static const float MaskRaw=1e-5;
float Lanczos2(float x){x=abs(x);if(x<1e-5)return 1;if(x>=2)return 0;float p=3.14159265359*x;return sin(p)*sin(p*.5)/(p*p*.5);}
float Tent(float x){return saturate(1-abs(x));}
int2 CS(int2 p){return clamp(p,0,int2(SourceSize)-1);}
int2 GS(int2 g){if(any(GuidanceSize==0)||all(GuidanceSize==SourceSize))return CS(g);
 return CS(int2(round((float2(g)+.5)*float2(SourceSize)/float2(GuidanceSize)-.5)));}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=DestinationSize))return;
 if(all(SourceSize==DestinationSize)){DestinationDepth[id.xy]=SourceDepth.Load(int3(id.xy,0));DestinationMotion[id.xy]=SourceMotion.Load(int3(id.xy,0))*MotionVectorMultiplier;return;}
 if(Filter==0){float2 q=(float2(id.xy)+.5)*float2(SourceSize)/float2(DestinationSize)-.5;int2 b=int2(floor(q));int2 mx=int2(SourceSize)-1;
  int2 c[4]={clamp(b,0,mx),clamp(b+int2(1,0),0,mx),clamp(b+int2(0,1),0,mx),clamp(b+int2(1,1),0,mx)};
  float z=1;float2 m=0;bool fnd=false;
  [unroll]for(uint i=0;i<4;i++){float d=SourceDepth.Load(int3(c[i],0));if(d<MaskRaw)continue;if(!fnd||d<z){z=d;m=SourceMotion.Load(int3(c[i],0));fnd=true;}}
  DestinationDepth[id.xy]=z;DestinationMotion[id.xy]=m*MotionVectorMultiplier;return;}
 uint2 grid=any(GuidanceSize==0)?SourceSize:GuidanceSize;float2 scale=float2(grid)/float2(DestinationSize);
 float2 gp=(float2(id.xy)+.5)*scale-.5;int2 gb=int2(floor(gp));int2 gmx=int2(grid)-1;float za=0;float2 ma=0;float wsum=0;
 [unroll]for(int y=-1;y<=2;y++)[unroll]for(int x=-1;x<=2;x++){int2 g=clamp(gb+int2(x,y),0,gmx);int2 s=GS(g);float d=SourceDepth.Load(int3(s,0));if(d<MaskRaw)continue;
  float2 dist=(float2(gb+int2(x,y))-gp)/max(scale,1);float w=Filter==1?Tent(dist.x)*Tent(dist.y):Lanczos2(dist.x)*Lanczos2(dist.y);
  za+=d*w;ma+=SourceMotion.Load(int3(s,0))*w;wsum+=w;}
 if(wsum<1e-5){DestinationDepth[id.xy]=1;DestinationMotion[id.xy]=0;return;}
 DestinationDepth[id.xy]=za/wsum;DestinationMotion[id.xy]=ma/wsum*MotionVectorMultiplier;
})";

		constexpr char kGuidanceFitShader[] = R"(
Texture2D<float4> NeuralInput:register(t0);Texture2D<float4> NeuralColor:register(t1);Texture2D<float> DepthGuide:register(t2);
RWTexture2D<float4> OutputGain:register(u0);RWTexture2D<float4> OutputEditLow:register(u1);
cbuffer Constants:register(b0){uint FullWidth,FullHeight,GuidanceWidth,GuidanceHeight;float DetailLuma,DetailChroma,RdEdgeFloor,Regularization;
 uint CompareMode,DebugView;float GainPrior,FitWindow;uint DepthWidth,DepthHeight;float MaxGain,DepthGuidance;
 uint EdgeBlockMode;float EdgeBlockGThresh;uint NeuralWidth,NeuralHeight;float CameraFar,CameraNear,CameraRange,CameraProduct;
 float DepthDilate,DetailGain,ToneGainLuma,ToneGainChroma,DitherStrength,EdgeBlockSkip,EdgeBlockDilateD,EdgeBlockMix;};
static const float SkyRaw=1-64./16777216.,MaskRaw=1e-5,SkyLin=1e7;
float3 LI(int2 p){return NeuralInput.Load(int3(clamp(p,0,int2(NeuralWidth,NeuralHeight)-1),0)).rgb;}
float3 LN(int2 p){return NeuralColor.Load(int3(clamp(p,0,int2(NeuralWidth,NeuralHeight)-1),0)).rgb;}
float DR(int2 p){if(DepthWidth==0||DepthHeight==0)return 1;return DepthGuide.Load(int3(clamp(p,0,int2(DepthWidth,DepthHeight)-1),0));}
float Lin(float r){if(r<MaskRaw)return CameraNear;if(r>=SkyRaw||CameraRange<=0||CameraProduct<=0)return SkyLin;return CameraProduct/max(-r*CameraRange+CameraFar,1e-5);}
bool RP(int2 a,int2 b){float ra=DR(a),rb=DR(b);if(ra<MaskRaw||rb<MaskRaw)return false;bool sa=ra>=SkyRaw,sb=rb>=SkyRaw;if(sa!=sb)return true;if(sa||sb)return false;
 float za=Lin(ra),zb=Lin(rb);return abs(za-zb)/max(min(za,zb),1)>lerp(.18,.028,saturate(DepthGuidance));}
int2 OR(int2 n){return clamp(int2(round((float2(n)+.5)*float2(DepthWidth,DepthHeight)/float2(NeuralWidth,NeuralHeight)-.5)),0,int2(DepthWidth,DepthHeight)-1);}
bool Same(int2 c,int2 s){if(DepthWidth==0||DepthHeight==0)return true;return !RP(OR(c),OR(s));}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=NeuralWidth||id.y>=NeuralHeight)return;int2 p=id.xy;int r=FitWindow>=.5?2:1;float3 cI=LI(p);float cY=dot(cI,float3(.2126,.7152,.0722));
 float sig=clamp(EdgeBlockSkip,.02,.5),inv=0.5/(sig*sig);float3 sI=0,sN=0,sII=0,sIN=0,sNN=0,sE=0;float wsum=0,ew=0;
 [unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++){int2 q=p+int2(x,y);if(!Same(p,q))continue;float3 i=LI(q),n=LN(q);
  float dy=dot(i,float3(.2126,.7152,.0722))-cY;float w=exp(-dy*dy*inv);sE+=(n-i)*w;ew+=w;if(abs(x)>r||abs(y)>r)continue;
  sI+=i*w;sN+=n*w;sII+=i*i*w;sIN+=i*n*w;sNN+=n*n*w;wsum+=w;}
 if(wsum<1e-4){sI=cI;sN=LN(p);sII=cI*cI;sIN=cI*sN;sNN=sN*sN;wsum=1;}
 if(ew<1e-4){sE=LN(p)-cI;ew=1;}
 float3 mI=sI/wsum,mN=sN/wsum,vI=max(sII/wsum-mI*mI,0),cv=sIN/wsum-mI*mN;
 float3 sDI=max(sII-mI*mI*wsum,0),sDN=max(sNN-mN*mN*wsum,0),sDIDN=sIN-mI*mN*wsum;
 float eps=Regularization*Regularization*.05;float3 fit=clamp((cv+eps*GainPrior)/(vI+eps),0,lerp(1,3,saturate(MaxGain)));
 float3 rho=saturate((sDIDN*sDIDN)/(sDI*sDN+eps*eps));float3 g=lerp(GainPrior,fit,rho);
 OutputGain[id.xy]=float4(g,dot(rho,float3(.2126,.7152,.0722)));OutputEditLow[id.xy]=float4(sE/ew,0);})";

		constexpr char kFusionShader[] = R"(
Texture2D<float4> OriginalColor:register(t0);Texture2D<float4> NeuralColor:register(t1);Texture2D<float4> NeuralGain:register(t2);Texture2D<float4> GuideLow:register(t3);Texture2D<float4> EditLow:register(t4);
Texture2D<float> DepthGuide:register(t5);RWTexture2D<float4> OutputColor:register(u0);
cbuffer Constants:register(b0){uint FullWidth,FullHeight,GuidanceWidth,GuidanceHeight;float DetailLuma,DetailChroma,RdEdgeFloor,Regularization;
 uint CompareMode,DebugView;float GainPrior,FitWindow;uint DepthWidth,DepthHeight;float MaxGain,DepthGuidance;
 uint EdgeBlockMode;float EdgeBlockGThresh;uint NeuralWidth,NeuralHeight;float CameraFar,CameraNear,CameraRange,CameraProduct;
 float DepthDilate,DetailGain,ToneGainLuma,ToneGainChroma,DitherStrength,EdgeBlockSkip,EdgeBlockDilateD,EdgeBlockMix;};
static const float3 LW=float3(.2126,.7152,.0722);static const float SkyRaw=1-64./16777216.,MaskRaw=1e-5,SkyLin=1e7;
float3 OG(int2 p){return OriginalColor.Load(int3(clamp(p,0,int2(GuidanceWidth,GuidanceHeight)-1),0)).rgb;}
float4 LG(int2 p){return NeuralGain.Load(int3(clamp(p,0,int2(NeuralWidth,NeuralHeight)-1),0));}
float3 LE(int2 p){return EditLow.Load(int3(clamp(p,0,int2(NeuralWidth,NeuralHeight)-1),0)).rgb;}
float3 GD(int2 p){float2 ratio=float2(GuidanceWidth,GuidanceHeight)/float2(FullWidth,FullHeight);float2 q=(float2(p)+.5)*ratio-.5;
 if(all(abs(ratio-1)<1e-3))return OG(int2(round(q)));int2 b=int2(floor(q));float3 a=0;float ws=0;
 [unroll]for(int y=-1;y<=2;y++)[unroll]for(int x=-1;x<=2;x++){float2 d=abs(float2(b+int2(x,y))-q)/max(ratio,1);float w=saturate(1-d.x)*saturate(1-d.y);if(w<=0)continue;a+=OG(b+int2(x,y))*w;ws+=w;}
 return a/max(ws,1e-5);}
float2 NP(int2 p){if(EdgeBlockMode!=0)return float2(p);return (float2(p)+.5)*float2(NeuralWidth,NeuralHeight)/float2(FullWidth,FullHeight)-.5;}
float4 GA(float2 q){int2 b=int2(floor(q));float2 f=frac(q);return lerp(lerp(LG(b),LG(b+int2(1,0)),f.x),lerp(LG(b+int2(0,1)),LG(b+int2(1,1)),f.x),f.y);}
float3 EA(float2 q){int2 b=int2(floor(q));float2 f=frac(q);return lerp(lerp(LE(b),LE(b+int2(1,0)),f.x),lerp(LE(b+int2(0,1)),LE(b+int2(1,1)),f.x),f.y);}
float3 Cube(float3 b,float3 i){float a=1;[unroll]for(int c=0;c<3;c++){if(i[c]>1e-6)a=min(a,(1-b[c])/i[c]);else if(i[c]<-1e-6)a=min(a,(0-b[c])/i[c]);}return b+saturate(a)*i;}
float Dith(int2 p){float3 p3=frac(float3(p.xyx)*float3(.1031,.1030,.0973));p3+=dot(p3,p3.yzx+33.33);float2 r=frac((p3.xx+p3.yz)*p3.zy);return (r.x+r.y-1)/255.;}
float3 SLC(float3 v,float lg,float cg){float y=dot(v,LW);return lg*y+cg*(v-y);}
float DR(int2 q){if(DepthWidth==0)return 1;return DepthGuide.Load(int3(clamp(q,0,int2(DepthWidth,DepthHeight)-1),0));}
float LinD(float r){if(r<MaskRaw)return CameraNear;if(r>=SkyRaw||CameraRange<=0||CameraProduct<=0)return SkyLin;return CameraProduct/max(-r*CameraRange+CameraFar,1e-5);}
int2 Od(int2 q){return clamp(int2(round((float2(q)+.5)*float2(DepthWidth,DepthHeight)/float2(FullWidth,FullHeight)-.5)),0,int2(DepthWidth,DepthHeight)-1);}
int2 Gd(int2 q){return clamp(int2(round((float2(q)+.5)*float2(GuidanceWidth,GuidanceHeight)/float2(FullWidth,FullHeight)-.5)),0,int2(GuidanceWidth,GuidanceHeight)-1);}
bool RP(int2 a,int2 b){float ra=DR(a),rb=DR(b);if(ra<MaskRaw||rb<MaskRaw)return false;bool sa=ra>=SkyRaw,sb=rb>=SkyRaw;if(sa!=sb)return true;if(sa||sb)return false;return abs(LinD(ra)-LinD(rb))/max(min(LinD(ra),LinD(rb)),1)>lerp(.18,.028,saturate(DepthGuidance));}
bool RC(int2 r){return RP(r,r+int2(1,0))||RP(r,r+int2(-1,0))||RP(r,r+int2(0,1))||RP(r,r+int2(0,-1));}
float2 GG(int2 p){int2 g=Gd(p);return float2(dot(OG(g+int2(1,0)),LW)-dot(OG(g+int2(-1,0)),LW),dot(OG(g+int2(0,1)),LW)-dot(OG(g+int2(0,-1)),LW));}
float LV(int2 p,bool g){float s=0,s2=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++){
 float yv=g?dot(OG(Gd(p+int2(x,y))),LW):dot(GuideLow.Load(int3(clamp(p+int2(x,y),0,int2(FullWidth,FullHeight)-1),0)).rgb,LW);s+=yv;s2+=yv*yv;}
 return max(s2/9-(s/9)*(s/9),0);}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 int2 loc=id.xy;int2 org=int2(int(EdgeBlockMode&0xffff),int(EdgeBlockMode>>16));bool cr=EdgeBlockMode!=0;
 if(cr){if(loc.x>=NeuralWidth||loc.y>=NeuralHeight)return;}else if(loc.x>=FullWidth||loc.y>=FullHeight)return;
 int2 n=loc;int2 p=cr?loc+org:loc;int2 nmax=cr?int2(NeuralWidth,NeuralHeight)-1:int2(FullWidth,FullHeight)-1;
 float4 rcS=NeuralColor.Load(int3(n,0));float3 rc=rcS.rgb;float3 lo=GuideLow.Load(int3(n,0)).rgb;
 float2 np=NP(n);float4 gS=GA(np);float3 g=gS.rgb;
 bool rh=p.x*2>=(int)FullWidth;if(CompareMode==1||(CompareMode==2&&rh)||(CompareMode==3&&!rh))g=1;
 float3 det=GD(p)-lo;float3 e=rc-lo;float3 el=EA(np);float3 e13=0;int2 nb=int2(floor(np));
 [unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)e13+=LE(nb+int2(x,y));e13/=9;float3 em=el-e13;float3 eh=e-el;
 if(EdgeBlockDilateD>1e-4){float2 gg=GG(p);int2 st=int2(round(gg/max(length(gg),1e-5)));if(all(st==0))st=int2(1,0);
  float3 ep=NeuralColor.Load(int3(clamp(n+st,0,nmax),0)).rgb-GuideLow.Load(int3(clamp(n+st,0,nmax),0)).rgb-EA(NP(n+st));
  float3 emi=NeuralColor.Load(int3(clamp(n-st,0,nmax),0)).rgb-GuideLow.Load(int3(clamp(n-st,0,nmax),0)).rgb-EA(NP(n-st));
  eh+=EdgeBlockDilateD*(eh-.5*(ep+emi));}
 float3 inj=g*SLC(det,DetailLuma,DetailChroma)+SLC(e13,ToneGainLuma-1,ToneGainChroma-1)+SLC(em,.5*(ToneGainLuma-1),.5*(ToneGainChroma-1))+(DetailGain-1)*eh;
 float3 o=Cube(rc,inj);float bd=max(max(abs(ToneGainLuma-1),abs(ToneGainChroma-1)),abs(DetailGain-1));float dt=DitherStrength*saturate(bd*2);if(dt>0)o=saturate(o+Dith(p)*dt);
 if(DebugView==1)o=g*.5;else if(DebugView==2)o=saturate(.5+det*4);else if(DebugView==3)o=saturate(.5+e*4);else if(DebugView==4)o=saturate(.5+el*4);
 else if(DebugView==5)o=saturate(.5+eh*8);else if(DebugView==6)o=lerp(dot(rc,LW).xxx*.5,float3(1,.2,.1),RC(Od(p))?1:0);
 else if(DebugView==7){float eps=max(Regularization*Regularization*.05,1e-6);float vG=LV(p,true),vL=LV(p,false);
  o=lerp(dot(rc,LW).xxx*.45,float3(1,.55,.15),saturate(max(vG-vL,0)/(max(vG-vL,0)+vL+eps)));}
 else if(DebugView==9)o=lerp(dot(rc,LW).xxx*.45,float3(1,.85,.2),gS.a);
 OutputColor[id.xy]=float4(o,rcS.a);})";

		constexpr char kSubrectBlendShader[] = R"(
Texture2D<float4> SrcTex:register(t0);RWTexture2D<float4> DstTex:register(u0);
cbuffer BlendCB:register(b0){uint DstOffsetX,DstOffsetY,SubWidth,SubHeight,BlendMode;float FeatherWidth;uint FrameIndex,SrcOffsetX;float DitherStrength;uint pad0,pad1,pad2;};
float BlueNoise(uint2 pos,uint frame){float x=float(pos.x)+5.588238*float(frame);float y=float(pos.y)+5.588238*float(frame);return frac(52.9829189*frac(0.06711056*x+0.00583715*y));}
[numthreads(8,8,1)]void main(uint3 tid:SV_DispatchThreadID){
 if(tid.x>=SubWidth||tid.y>=SubHeight)return;
 uint2 srcPos=uint2(tid.x+SrcOffsetX,tid.y);uint2 dstPos=uint2(tid.x+DstOffsetX,tid.y+DstOffsetY);
 float4 dlss=SrcTex.Load(int3(srcPos,0));
 float distL=(float)tid.x,distR=(float)(SubWidth-1-tid.x),distT=(float)tid.y,distB=(float)(SubHeight-1-tid.y);
 float edgeDist=min(min(distL,distR),min(distT,distB));
 if(edgeDist>=FeatherWidth){DstTex[dstPos]=dlss;return;}
 float4 bg=DstTex[dstPos];
 if(BlendMode==2){float t=smoothstep(0,1,saturate(edgeDist/max(FeatherWidth,1e-5)));float n=BlueNoise(dstPos,0);DstTex[dstPos]=lerp(bg,dlss,saturate(t+(n-.5)*DitherStrength*t*(1-t)));}
 else if(BlendMode==1){float t=edgeDist/FeatherWidth;float noise=BlueNoise(srcPos,FrameIndex);DstTex[dstPos]=lerp(bg,dlss,saturate(t+(noise-0.5)*DitherStrength));}
 else DstTex[dstPos]=lerp(bg,dlss,smoothstep(0.0,FeatherWidth,edgeDist));
})";

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
			ComPtr<ID3D12Resource> gainN;
			ComPtr<ID3D12Resource> editLowN;
			ComPtr<ID3D12Resource> guideLowD;
			ComPtr<ID3D12Resource> act3Color;
			ComPtr<ID3D12Resource> act3Depth;
			ComPtr<ID3D12Resource> act3Motion;
			ComPtr<ID3D12Resource> act3Output;
			ComPtr<ID3D12Resource> act3DepthD;
			ComPtr<ID3D12Resource> act3MotionD;
			ComPtr<ID3D12Resource> act3Gain;
			ComPtr<ID3D12Resource> act3EditLow;
			ComPtr<ID3D12Resource> act3Fusion;
			ComPtr<ID3D12Resource> act3GuideLow;
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
			DXGI_FORMAT motionOutputFormat = DXGI_FORMAT_UNKNOWN;
			std::uint32_t act3Width = 0;
			std::uint32_t act3Height = 0;
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
			ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kSrvsPerBind, 0, 0, 0 };
			ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, kSrvsPerBind };
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
				CreatePipeline(kTentShader, "AriseTent", resampleTent) &&
				CreatePipeline(kMitchellShader, "AriseMitchell", resampleMitchell) &&
				CreatePipeline(kEasuShader, "AriseEASU", resampleEasu) &&
				CreatePipeline(kResizeGuidesShader, "AriseResizeGuides", resizeGuides) &&
				CreatePipeline(kGuidanceFitShader, "AriseGuidanceFit", guidanceFit) &&
				CreatePipeline(kFusionShader, "AriseFusion", fusion) &&
				CreatePipeline(kSubrectBlendShader, "AriseSubrectBlend", subrectBlend) &&
				CreatePipeline(kRcasShader, "AriseRCAS", rcas);
		}

		void Shutdown()
		{
			dlss.Shutdown();
			slots.clear();
			resample.Reset();
			resampleTent.Reset();
			resampleMitchell.Reset();
			resampleEasu.Reset();
			resizeGuides.Reset();
			guidanceFit.Reset();
			fusion.Reset();
			subrectBlend.Reset();
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
			const bool slotMatches =
				slot.displayWidth == plan.displayWidth && slot.displayHeight == plan.displayHeight &&
				slot.sceneInputWidth == plan.sceneInputWidth && slot.sceneInputHeight == plan.sceneInputHeight &&
				slot.engineWidth == plan.engineWidth && slot.engineHeight == plan.engineHeight &&
				slot.act1OutputWidth == plan.act1OutputWidth && slot.act1OutputHeight == plan.act1OutputHeight &&
				slot.nrWidth == plan.nrWidth && slot.nrHeight == plan.nrHeight &&
				slot.sceneFormat == sceneDesc.Format &&
				slot.depthFormat == depthDesc.Format &&
				slot.motionFormat == motionDesc.Format;
			if (slotMatches)
				return true;
			dlss.Reset();
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
			slot.motionOutputFormat = motionOutputFormat;
			if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&slot.heap))) ||
				!CreateTexture(slot.colorN, plan.nrWidth, plan.nrHeight, kColorFormat, L"ARISE Graph Color N") ||
				!CreateTexture(slot.act1Dlss, plan.act1OutputWidth, plan.act1OutputHeight, kColorFormat, L"ARISE Graph ACT1 DLSS") ||
				!CreateTexture(slot.depthN, plan.nrWidth, plan.nrHeight, DXGI_FORMAT_R32_FLOAT, L"ARISE Graph Depth N") ||
				!CreateTexture(slot.motionN, plan.nrWidth, plan.nrHeight, motionOutputFormat, L"ARISE Graph Motion N") ||
				!CreateTexture(slot.neuralN, plan.nrWidth, plan.nrHeight, kColorFormat, L"ARISE Graph Neural N") ||
				!CreateTexture(slot.referenceD, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Reference D") ||
				!CreateTexture(slot.displayA, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Display A") ||
				!CreateTexture(slot.displayB, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Display B") ||
				!CreateTexture(slot.act2Dlss, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph ACT2 DLSS") ||
				!CreateTexture(slot.gainN, plan.nrWidth, plan.nrHeight, kColorFormat, L"ARISE Graph Guidance Gain N") ||
				!CreateTexture(slot.editLowN, plan.nrWidth, plan.nrHeight, kColorFormat, L"ARISE Graph Guidance Edit Low N") ||
				!CreateTexture(slot.guideLowD, plan.displayWidth, plan.displayHeight, kColorFormat, L"ARISE Graph Guide Low D"))
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
			VrPassTimer::Mark(list, "VrDb::DlssSr");
			if (plan.renderDocReplacement && plan.act1 == Act1Backend::kDLSS) {
				act1DlssSucceeded = DispatchResample(list, slot, inputs.scene, slot.act1Dlss.Get(),
					plan.sceneInputWidth, plan.sceneInputHeight, plan.act1OutputWidth, plan.act1OutputHeight,
					SrvFormat(inputs.scene->GetDesc().Format), kColorFormat);
			} else if (plan.act1 == Act1Backend::kDLSS) {
				act1DlssSucceeded = dlss.Record(list, {
					.frameId = plan.frameId, .viewport = plan.act1Viewport,
					.inputWidth = plan.sceneInputWidth, .inputHeight = plan.sceneInputHeight,
					.outputWidth = plan.act1OutputWidth, .outputHeight = plan.act1OutputHeight,
					.quality = plan.act1Quality, .preset = plan.act1Preset, .reset = plan.reset,
					.camera = plan.act1Camera,
					.color = inputs.scene, .output = slot.act1Dlss.Get(), .depth = inputs.depth,
					.motionVectors = inputs.motionVectors,
					.colorBuffersHDR = plan.colorBuffersHDR });
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
			ID3D12PipelineState* downsample = plan.act1Downsample == 1 ? resampleTent.Get() : resample.Get();
			ID3D12Resource* finalColor = nullptr;
			ID3D12Resource* act2Rc = nullptr;
			bool nrSucceeded = false;
			bool fusionSucceeded = false;
			if (!plan.requested) {
				// No NR: the SR image is larger than D. Downsample it to D, then ACT3 and RCAS.
				VrPassTimer::Mark(list, "VrDb::ToD");
				if (!DispatchResample(list, slot, act1Color, slot.displayA.Get(), act1Width, act1Height,
						plan.displayWidth, plan.displayHeight, act1Format, kColorFormat, downsample)) {
					++failed;
					return result;
				}
				++lanczos;
				finalColor = slot.displayA.Get();
				act2Rc = finalColor;
			} else {
			VrPassTimer::Mark(list, "VrDb::ToN");
			if (!DispatchResample(list, slot, act1Color, slot.colorN.Get(), act1Width, act1Height,
					plan.nrWidth, plan.nrHeight, act1Format, kColorFormat, downsample) ||
				!DispatchResizeGuides(list, slot, inputs.depth, inputs.motionVectors, slot.depthN.Get(), slot.motionN.Get(),
					plan.sceneInputWidth, plan.sceneInputHeight, plan.nrWidth, plan.nrHeight,
					act1Width, act1Height, plan.act1DepthDownsample)) {
				++failed;
				return result;
			}
			++lanczos;
			VrPassTimer::Mark(list, "VrDb::Nr");
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
					plan.motionVectorScaleX, plan.motionVectorScaleY, plan.tuning, plan.reset || forceReset,
					plan.nrSlot);
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
			const bool identityDisplay = plan.nrWidth == plan.displayWidth && plan.nrHeight == plan.displayHeight;
			finalColor = neural;
			VrPassTimer::Mark(list, "VrDb::ToD");
			if (!identityDisplay) {
				if (!DispatchResample(list, slot, neural, slot.displayA.Get(), plan.nrWidth, plan.nrHeight,
						plan.displayWidth, plan.displayHeight, kColorFormat, kColorFormat,
						SpatialUpsamplePipeline(plan.act2Sr))) {
					++failed;
					return result;
				}
				++lanczos;
				finalColor = slot.displayA.Get();
				if (plan.act2Sr == Act2SrBackend::kDLSS) {
					if (plan.renderDocReplacement && DispatchResample(list, slot, neural, slot.act2Dlss.Get(),
							plan.nrWidth, plan.nrHeight, plan.displayWidth, plan.displayHeight,
							kColorFormat, kColorFormat))
						finalColor = slot.act2Dlss.Get();
					else if (!plan.renderDocReplacement && dlss.Record(list, {
						.frameId = plan.frameId, .viewport = plan.act2Viewport,
						.inputWidth = plan.nrWidth, .inputHeight = plan.nrHeight,
						.outputWidth = plan.displayWidth, .outputHeight = plan.displayHeight,
						.quality = plan.act2Quality, .preset = plan.act2Preset, .reset = plan.reset,
						.camera = plan.act2Camera,
						.color = neural, .output = slot.act2Dlss.Get(), .depth = slot.depthN.Get(),
						.motionVectors = slot.motionN.Get(),
						.colorBuffersHDR = plan.colorBuffersHDR }))
						finalColor = slot.act2Dlss.Get();
					else
						++fallback;
				}
			}
			act2Rc = finalColor;
			VrPassTimer::Mark(list, "VrDb::Guidance");
			if (plan.fusionEnabled) {
				ID3D12Resource* referenceG = act1DlssSucceeded ? slot.act1Dlss.Get() : slot.referenceD.Get();
				const std::uint32_t referenceWidth = act1DlssSucceeded ? plan.act1OutputWidth : plan.displayWidth;
				const std::uint32_t referenceHeight = act1DlssSucceeded ? plan.act1OutputHeight : plan.displayHeight;
				const bool referenceReady = act1DlssSucceeded || DispatchResample(list, slot, inputs.scene,
					slot.referenceD.Get(), plan.sceneInputWidth, plan.sceneInputHeight,
					plan.displayWidth, plan.displayHeight, SrvFormat(inputs.scene->GetDesc().Format), kColorFormat);
				// The detail band is G minus the NR input lifted by the same UD kernel that produced RC.
				const bool spatialAct2 = plan.act2Sr == Act2SrBackend::kLanczos || plan.act2Sr == Act2SrBackend::kTent ||
					plan.act2Sr == Act2SrBackend::kMitchell || plan.act2Sr == Act2SrBackend::kEasu;
				const bool guidanceReady = referenceReady &&
					DispatchResample(list, slot, slot.colorN.Get(), slot.guideLowD.Get(), plan.nrWidth, plan.nrHeight,
						plan.displayWidth, plan.displayHeight, kColorFormat, kColorFormat,
						spatialAct2 ? SpatialUpsamplePipeline(plan.act2Sr) : resample.Get()) &&
					DispatchGuidanceFit(list, slot, slot.colorN.Get(), neural, inputs.depth, slot.gainN.Get(), slot.editLowN.Get(), plan);
				if (guidanceReady && DispatchFusion(list, slot, referenceG, finalColor, slot.gainN.Get(), slot.guideLowD.Get(),
					slot.editLowN.Get(), slot.displayB.Get(), referenceWidth, referenceHeight, plan, inputs.depth)) {
					finalColor = slot.displayB.Get();
					fusionSucceeded = true;
					++fusionCount;
				} else {
					++failed;
					++fallback;
				}
			}
			}
			const bool act3OwnGuidance = plan.act3.enabled && plan.act3.guidanceEnabled && plan.requested;
			bool rcasSucceeded = false;
			auto applyRcas = [&]() {
				VrPassTimer::Mark(list, "VrDb::Rcas");
				ID3D12Resource* target = finalColor == slot.displayA.Get() ? slot.displayB.Get() : slot.displayA.Get();
				if (DispatchRcas(list, slot, finalColor, target, plan)) {
					finalColor = target;
					rcasSucceeded = true;
					++rcasCount;
					return true;
				}
				++failed;
				++fallback;
				return false;
			};
			if (plan.sharpen && !act3OwnGuidance)
				applyRcas();
			if (plan.act3.enabled && plan.act3.cropWidth >= 8 && plan.act3.cropHeight >= 8) {
				VrPassTimer::Mark(list, "VrDb::Act3");
				if (act3OwnGuidance && !plan.fusionEnabled) {
					const bool spatialAct2 = plan.act2Sr == Act2SrBackend::kLanczos || plan.act2Sr == Act2SrBackend::kTent ||
						plan.act2Sr == Act2SrBackend::kMitchell || plan.act2Sr == Act2SrBackend::kEasu;
					if (!DispatchResample(list, slot, slot.colorN.Get(), slot.guideLowD.Get(), plan.nrWidth, plan.nrHeight,
							plan.displayWidth, plan.displayHeight, kColorFormat, kColorFormat,
							spatialAct2 ? SpatialUpsamplePipeline(plan.act2Sr) : resample.Get()))
						logger::warn("[ACT3] ACT2 Lo lift failed; crop Guidance will use the RC crop");
				}
				ID3D12Resource* act3Color = finalColor;
				if (DispatchAct3(list, slot, finalColor, act3OwnGuidance ? act2Rc : finalColor,
						act3OwnGuidance ? slot.guideLowD.Get() : nullptr,
						inputs.depth, inputs.motionVectors, plan,
						act1Color, act1Width, act1Height, act3Color))
					finalColor = act3Color;
				else
					++failed;
			}
			if (plan.sharpen && act3OwnGuidance)
				applyRcas();
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
			case 3: return "Bypass > N > NR > Guidance";
			case 4: return "Bypass > N > NR > Guidance > RCAS";
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
			ID3D12Resource* source2 = nullptr, DXGI_FORMAT source2Format = DXGI_FORMAT_UNKNOWN,
			ID3D12Resource* source3 = nullptr, DXGI_FORMAT source3Format = DXGI_FORMAT_UNKNOWN,
			ID3D12Resource* source4 = nullptr, DXGI_FORMAT source4Format = DXGI_FORMAT_UNKNOWN,
			ID3D12Resource* destination1 = nullptr,
			ID3D12Resource* source5 = nullptr, DXGI_FORMAT source5Format = DXGI_FORMAT_UNKNOWN,
			ID3D12Resource* source6 = nullptr, DXGI_FORMAT source6Format = DXGI_FORMAT_UNKNOWN,
			ID3D12Resource* source7 = nullptr, DXGI_FORMAT source7Format = DXGI_FORMAT_UNKNOWN)
		{
			if (!slot.heap || slot.descriptorCursor > kDescriptorsPerSlot - kDescriptorsPerBind)
				return {};
			const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto handle = slot.heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(slot.descriptorCursor) * stride;
			auto gpuHandle = slot.heap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<UINT64>(slot.descriptorCursor) * stride;
			for (auto [resource, format] : std::array<std::pair<ID3D12Resource*, DXGI_FORMAT>, kSrvsPerBind>{ {
				{ source0, source0Format }, { source1, source1Format }, { source2, source2Format },
				{ source3, source3Format }, { source4, source4Format }, { source5, source5Format },
				{ source6, source6Format }, { source7, source7Format } } }) {
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
			device->CreateUnorderedAccessView(destination1, nullptr, &uav, handle);
			slot.descriptorCursor += kDescriptorsPerBind;
			return gpuHandle;
		}

		D3D12_GPU_DESCRIPTOR_HANDLE BindGuideViews(Slot& slot, ID3D12Resource* depth, DXGI_FORMAT depthFormat,
			ID3D12Resource* motion, DXGI_FORMAT motionFormat, ID3D12Resource* destinationDepth,
			ID3D12Resource* destinationMotion)
		{
			if (!slot.heap || slot.descriptorCursor > kDescriptorsPerSlot - kDescriptorsPerBind)
				return {};
			const UINT stride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			auto handle = slot.heap->GetCPUDescriptorHandleForHeapStart();
			handle.ptr += static_cast<SIZE_T>(slot.descriptorCursor) * stride;
			auto gpuHandle = slot.heap->GetGPUDescriptorHandleForHeapStart();
			gpuHandle.ptr += static_cast<UINT64>(slot.descriptorCursor) * stride;
			for (auto [resource, format] : std::array<std::pair<ID3D12Resource*, DXGI_FORMAT>, kSrvsPerBind>{ {
				{ depth, depthFormat }, { motion, motionFormat }, { nullptr, kColorFormat }, { nullptr, kColorFormat },
				{ nullptr, kColorFormat }, { nullptr, kColorFormat }, { nullptr, kColorFormat }, { nullptr, kColorFormat } } }) {
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
			slot.descriptorCursor += kDescriptorsPerBind;
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

		ID3D12PipelineState* SpatialUpsamplePipeline(Act2SrBackend backend) const
		{
			switch (backend) {
			case Act2SrBackend::kTent: return resampleTent.Get();
			case Act2SrBackend::kMitchell: return resampleMitchell.Get();
			case Act2SrBackend::kEasu: return resampleEasu.Get();
			default: return resample.Get();
			}
		}

		bool DispatchResample(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* source,
			ID3D12Resource* destination, std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight,
			DXGI_FORMAT sourceFormat, DXGI_FORMAT destinationFormat,
			ID3D12PipelineState* pipeline = nullptr)
		{
			if (!source || !destination)
				return false;
			if (sourceWidth == destinationWidth && sourceHeight == destinationHeight &&
				sourceFormat == destinationFormat) {
				Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
				Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
				list->CopyResource(destination, source);
				Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
				return true;
			}
			if (!pipeline)
				pipeline = resample.Get();
			const std::uint32_t constants[]{ sourceWidth, sourceHeight, destinationWidth, destinationHeight };
			const auto views = BindViews(slot, source, sourceFormat, destination, destinationFormat);
			if (!views.ptr)
				return false;
			Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, pipeline, constants, 4, views))
				return false;
			list->Dispatch((destinationWidth + 7) / 8, (destinationHeight + 7) / 8, 1);
			Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		bool DispatchResizeGuides(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* depth,
			ID3D12Resource* motion, ID3D12Resource* destinationDepth, ID3D12Resource* destinationMotion,
			std::uint32_t sourceWidth, std::uint32_t sourceHeight,
			std::uint32_t destinationWidth, std::uint32_t destinationHeight,
			std::uint32_t guidanceWidth, std::uint32_t guidanceHeight, std::uint32_t filter)
		{
			if (!depth || !motion || !destinationDepth || !destinationMotion)
				return false;
			struct Constants {
				std::uint32_t source[2], destination[2], guidance[2], filter;
				float multiplier;
			} constants{
				{ sourceWidth, sourceHeight }, { destinationWidth, destinationHeight },
				{ guidanceWidth, guidanceHeight }, filter, 1.0f };
			static_assert(sizeof(Constants) == 8 * sizeof(std::uint32_t));
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

		// Positional twin of the HLSL FusionConstants cbuffer shared with ResolveFusionCS.hlsl / GuidanceFitCS.hlsl.
		struct FusionConstants
		{
			std::uint32_t full[2], guidance[2];
			float detailLuma, detailChroma, rdEdgeFloor, regularization;
			std::uint32_t compareMode, debugView;
			float gainPrior, fitWindow;
			std::uint32_t depth[2];
			float maxGain, depthGuidance;
			std::uint32_t edgeBlockMode;
			float edgeBlockGThresh;
			std::uint32_t neuralWidth, neuralHeight;
			float cameraFar, cameraNear, cameraRange, cameraProduct;
			float depthDilate, detailGain, toneGainLuma, toneGainChroma;
			float ditherStrength, edgeBlockSkip, edgeBlockDilateD, edgeBlockMix;
		};
		static_assert(sizeof(FusionConstants) == kRootConstantDwords * sizeof(std::uint32_t));

		FusionConstants MakeFusionConstants(const Plan& plan, std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			std::uint32_t depthWidth, std::uint32_t depthHeight) const
		{
			const auto camera = Util::GetCameraData();
			return FusionConstants{ { plan.displayWidth, plan.displayHeight }, { guidanceWidth, guidanceHeight },
				plan.fusion.detailLuma, plan.fusion.detailChroma, plan.fusion.rdEdgeFloor, plan.fusion.regularization,
				plan.fusion.compareMode, plan.fusion.debugView, plan.fusion.gainPrior, plan.fusion.fitWindow,
				{ depthWidth, depthHeight }, plan.fusion.maxGain, plan.fusion.depthGuidance,
				plan.fusion.edgeBlockMode, plan.fusion.edgeBlockGThresh, plan.nrWidth, plan.nrHeight,
				camera.x, camera.y, camera.z, camera.w,
				plan.fusion.depthDilate, plan.fusion.detailGain, plan.fusion.toneGainLuma, plan.fusion.toneGainChroma,
				plan.fusion.ditherStrength, plan.fusion.edgeBlockSkip, plan.fusion.edgeBlockDilateD, plan.fusion.edgeBlockMix };
		}

		FusionConstants MakeAct3FusionConstants(const Plan& plan, std::uint32_t guidanceWidth,
			std::uint32_t guidanceHeight) const
		{
			const auto camera = Util::GetCameraData();
			const std::uint32_t cropPacked = (plan.act3.cropX & 0xffffu) | ((plan.act3.cropY & 0xffffu) << 16);
			return FusionConstants{ { plan.displayWidth, plan.displayHeight }, { guidanceWidth, guidanceHeight },
				plan.act3.guidanceLuma, plan.act3.guidanceChroma, plan.fusion.rdEdgeFloor, plan.fusion.regularization,
				0u, 0u, plan.act3.guidancePrior, plan.fusion.fitWindow,
				{ plan.act3.cropWidth, plan.act3.cropHeight }, plan.act3.guidanceMaxGain, plan.fusion.depthGuidance,
				cropPacked, plan.fusion.edgeBlockGThresh, plan.act3.cropWidth, plan.act3.cropHeight,
				camera.x, camera.y, camera.z, camera.w,
				plan.fusion.depthDilate, plan.act3.guidanceDetailGain, plan.act3.guidanceToneLuma, plan.act3.guidanceToneChroma,
				plan.act3.guidanceDither, plan.fusion.edgeBlockSkip, plan.act3.guidanceDirSharpen, plan.fusion.edgeBlockMix };
		}

		bool DispatchGuidanceFit(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* neuralInput,
			ID3D12Resource* neural, ID3D12Resource* depth, ID3D12Resource* gain, ID3D12Resource* editLow, const Plan& plan)
		{
			const auto constants = MakeFusionConstants(plan, 0, 0, plan.sceneInputWidth, plan.sceneInputHeight);
			const auto views = BindViews(slot, neuralInput, kColorFormat, gain, kColorFormat,
				neural, kColorFormat, depth, SrvFormat(depth->GetDesc().Format), nullptr, DXGI_FORMAT_UNKNOWN,
				nullptr, DXGI_FORMAT_UNKNOWN, editLow);
			if (!views.ptr)
				return false;
			for (auto* resource : { neuralInput, neural, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			for (auto* resource : { gain, editLow })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, guidanceFit.Get(), &constants, kRootConstantDwords, views))
				return false;
			list->Dispatch((plan.nrWidth + 7) / 8, (plan.nrHeight + 7) / 8, 1);
			for (auto* resource : { neuralInput, neural, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			for (auto* resource : { gain, editLow })
				Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		bool DispatchFusion(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* original,
			ID3D12Resource* neural, ID3D12Resource* gain, ID3D12Resource* guideLow, ID3D12Resource* editLow,
			ID3D12Resource* destination, std::uint32_t originalWidth, std::uint32_t originalHeight, const Plan& plan,
			ID3D12Resource* depth)
		{
			const auto constants = MakeFusionConstants(plan, originalWidth, originalHeight,
				plan.sceneInputWidth, plan.sceneInputHeight);
			const auto views = BindViews(slot, original, SrvFormat(original->GetDesc().Format), destination, kColorFormat,
				neural, kColorFormat, gain, kColorFormat, guideLow, kColorFormat, editLow, kColorFormat, nullptr,
				depth, SrvFormat(depth->GetDesc().Format));
			if (!views.ptr)
				return false;
			for (auto* resource : { original, neural, gain, guideLow, editLow, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, fusion.Get(), &constants, kRootConstantDwords, views))
				return false;
			list->Dispatch((plan.displayWidth + 7) / 8, (plan.displayHeight + 7) / 8, 1);
			for (auto* resource : { original, neural, gain, guideLow, editLow, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		bool DispatchAct3Guidance(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* neuralInput,
			ID3D12Resource* neural, ID3D12Resource* depth, ID3D12Resource* guideLow, ID3D12Resource* guidance,
			std::uint32_t guidanceWidth, std::uint32_t guidanceHeight, const Plan& plan)
		{
			if (!neuralInput || !neural || !depth || !guidance || !slot.act3Gain || !slot.act3EditLow || !slot.act3Fusion)
				return false;
			ID3D12Resource* lo = guideLow ? guideLow : neuralInput;
			const auto constants = MakeAct3FusionConstants(plan, guidanceWidth, guidanceHeight);
			const auto fitViews = BindViews(slot, lo, kColorFormat, slot.act3Gain.Get(), kColorFormat,
				neural, kColorFormat, depth, SrvFormat(depth->GetDesc().Format), nullptr, DXGI_FORMAT_UNKNOWN,
				nullptr, DXGI_FORMAT_UNKNOWN, slot.act3EditLow.Get());
			if (!fitViews.ptr)
				return false;
			for (auto* resource : { lo, neural, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			for (auto* resource : { slot.act3Gain.Get(), slot.act3EditLow.Get() })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, guidanceFit.Get(), &constants, kRootConstantDwords, fitViews)) {
				for (auto* resource : { lo, neural, depth })
					Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				for (auto* resource : { slot.act3Gain.Get(), slot.act3EditLow.Get() })
					Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
				return false;
			}
			list->Dispatch((plan.act3.cropWidth + 7) / 8, (plan.act3.cropHeight + 7) / 8, 1);
			for (auto* resource : { lo, neural, depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			for (auto* resource : { slot.act3Gain.Get(), slot.act3EditLow.Get() })
				Transition(list, resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);

			const auto resolveViews = BindViews(slot, guidance, SrvFormat(guidance->GetDesc().Format),
				slot.act3Fusion.Get(), kColorFormat, neural, kColorFormat, slot.act3Gain.Get(), kColorFormat,
				lo, kColorFormat, slot.act3EditLow.Get(), kColorFormat, nullptr,
				depth, SrvFormat(depth->GetDesc().Format));
			if (!resolveViews.ptr)
				return false;
			for (auto* resource : { guidance, neural, slot.act3Gain.Get(), lo, slot.act3EditLow.Get(), depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, slot.act3Fusion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, fusion.Get(), &constants, kRootConstantDwords, resolveViews)) {
				for (auto* resource : { guidance, neural, slot.act3Gain.Get(), lo, slot.act3EditLow.Get(), depth })
					Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.act3Fusion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
				return false;
			}
			list->Dispatch((plan.act3.cropWidth + 7) / 8, (plan.act3.cropHeight + 7) / 8, 1);
			for (auto* resource : { guidance, neural, slot.act3Gain.Get(), lo, slot.act3EditLow.Get(), depth })
				Transition(list, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, slot.act3Fusion.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			return true;
		}

		void CopyTextureSubrect(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* destination,
			std::uint32_t srcX, std::uint32_t srcY, std::uint32_t dstX, std::uint32_t dstY,
			std::uint32_t width, std::uint32_t height)
		{
			Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			const D3D12_TEXTURE_COPY_LOCATION src{ source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
			const D3D12_TEXTURE_COPY_LOCATION dst{ destination, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, 0 };
			const D3D12_BOX box{ srcX, srcY, 0, srcX + width, srcY + height, 1 };
			list->CopyTextureRegion(&dst, dstX, dstY, 0, &src, &box);
			Transition(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			Transition(list, destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
		}

		bool EnsureAct3Resources(Slot& slot, const Plan& plan)
		{
			if (slot.act3Width == plan.act3.cropWidth && slot.act3Height == plan.act3.cropHeight &&
				slot.act3Color && slot.act3Depth && slot.act3Motion && slot.act3Output &&
				slot.act3DepthD && slot.act3MotionD && slot.act3Gain && slot.act3EditLow && slot.act3Fusion &&
				slot.act3GuideLow)
				return true;
			slot.act3Color.Reset();
			slot.act3Output.Reset();
			slot.act3Depth.Reset();
			slot.act3Motion.Reset();
			slot.act3DepthD.Reset();
			slot.act3MotionD.Reset();
			slot.act3Gain.Reset();
			slot.act3EditLow.Reset();
			slot.act3Fusion.Reset();
			slot.act3GuideLow.Reset();
			slot.act3Width = slot.act3Height = 0;
			if (!CreateTexture(slot.act3Color, plan.act3.cropWidth, plan.act3.cropHeight, kColorFormat, L"ARISE Graph ACT3 Color") ||
				!CreateTexture(slot.act3Output, plan.act3.cropWidth, plan.act3.cropHeight, kColorFormat, L"ARISE Graph ACT3 Output") ||
				!CreateTexture(slot.act3Depth, plan.act3.cropWidth, plan.act3.cropHeight, DXGI_FORMAT_R32_FLOAT, L"ARISE Graph ACT3 Depth") ||
				!CreateTexture(slot.act3Motion, plan.act3.cropWidth, plan.act3.cropHeight, slot.motionOutputFormat, L"ARISE Graph ACT3 Motion") ||
				!CreateTexture(slot.act3DepthD, plan.displayWidth, plan.displayHeight, DXGI_FORMAT_R32_FLOAT, L"ARISE Graph ACT3 Depth D") ||
				!CreateTexture(slot.act3MotionD, plan.displayWidth, plan.displayHeight, slot.motionOutputFormat, L"ARISE Graph ACT3 Motion D") ||
				!CreateTexture(slot.act3Gain, plan.act3.cropWidth, plan.act3.cropHeight, kColorFormat, L"ARISE Graph ACT3 Gain") ||
				!CreateTexture(slot.act3EditLow, plan.act3.cropWidth, plan.act3.cropHeight, kColorFormat, L"ARISE Graph ACT3 EditLow") ||
				!CreateTexture(slot.act3Fusion, plan.act3.cropWidth, plan.act3.cropHeight, kColorFormat, L"ARISE Graph ACT3 Fusion") ||
				!CreateTexture(slot.act3GuideLow, plan.act3.cropWidth, plan.act3.cropHeight, kColorFormat, L"ARISE Graph ACT3 GuideLow"))
				return false;
			slot.act3Width = plan.act3.cropWidth;
			slot.act3Height = plan.act3.cropHeight;
			return true;
		}

		bool DispatchAct3(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* destColor,
			ID3D12Resource* cropSource, ID3D12Resource* act2Lo, ID3D12Resource* sourceDepth,
			ID3D12Resource* sourceMotion, const Plan& plan,
			ID3D12Resource* guidance, std::uint32_t guidanceWidth, std::uint32_t guidanceHeight,
			ID3D12Resource*& finalColor)
		{
			if (!destColor)
				return false;
			if (!cropSource)
				cropSource = destColor;
			if (!cropSource || !sourceDepth || !sourceMotion || slot.motionOutputFormat == DXGI_FORMAT_UNKNOWN ||
				!EnsureAct3Resources(slot, plan))
				return false;
			if (!DispatchResizeGuides(list, slot, sourceDepth, sourceMotion, slot.act3DepthD.Get(), slot.act3MotionD.Get(),
					plan.sceneInputWidth, plan.sceneInputHeight, plan.displayWidth, plan.displayHeight, 0, 0,
					plan.act1DepthDownsample))
				return false;
			CopyTextureSubrect(list, cropSource, slot.act3Color.Get(),
				plan.act3.cropX, plan.act3.cropY, 0, 0, plan.act3.cropWidth, plan.act3.cropHeight);
			CopyTextureSubrect(list, slot.act3DepthD.Get(), slot.act3Depth.Get(),
				plan.act3.cropX, plan.act3.cropY, 0, 0, plan.act3.cropWidth, plan.act3.cropHeight);
			CopyTextureSubrect(list, slot.act3MotionD.Get(), slot.act3Motion.Get(),
				plan.act3.cropX, plan.act3.cropY, 0, 0, plan.act3.cropWidth, plan.act3.cropHeight);

			if (!plan.renderDocReplacement) {
				Transition(list, slot.act3Color.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				Transition(list, slot.act3Depth.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				Transition(list, slot.act3Motion.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				Transition(list, slot.act3Output.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
				const bool succeeded = Renderer::Instance().ExecuteModel3(list, slot.act3Color.Get(), slot.act3Depth.Get(),
					slot.act3Motion.Get(), slot.act3Output.Get(), plan.act3.cropWidth, plan.act3.cropHeight,
					static_cast<float>(plan.engineWidth), static_cast<float>(plan.engineHeight),
					plan.act3.tuning, plan.reset || plan.act3.resetEveryFrame, 1);
				Transition(list, slot.act3Color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.act3Depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.act3Motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, slot.act3Output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
				if (!succeeded)
					return false;
			} else {
				CopyTextureSubrect(list, slot.act3Color.Get(), slot.act3Output.Get(),
					0, 0, 0, 0, plan.act3.cropWidth, plan.act3.cropHeight);
			}

			ID3D12Resource* writeback = slot.act3Output.Get();
			if (plan.act3.guidanceEnabled && guidance && guidanceWidth && guidanceHeight) {
				ID3D12Resource* lo = nullptr;
				if (act2Lo && slot.act3GuideLow) {
					CopyTextureSubrect(list, act2Lo, slot.act3GuideLow.Get(),
						plan.act3.cropX, plan.act3.cropY, 0, 0, plan.act3.cropWidth, plan.act3.cropHeight);
					lo = slot.act3GuideLow.Get();
				}
				if (DispatchAct3Guidance(list, slot, slot.act3Color.Get(), slot.act3Output.Get(),
						slot.act3Depth.Get(), lo, guidance, guidanceWidth, guidanceHeight, plan))
					writeback = slot.act3Fusion.Get();
				else
					logger::warn("[ACT3] Crop Guidance failed; writing raw ACT3 NR");
			}
			if (!DispatchAct3Blend(list, slot, writeback, destColor, plan))
				CopyTextureSubrect(list, writeback, destColor,
					0, 0, plan.act3.cropX, plan.act3.cropY, plan.act3.cropWidth, plan.act3.cropHeight);
			finalColor = destColor;
			if (plan.act3.sharpen) {
				ID3D12Resource* target = destColor == slot.displayA.Get() ? slot.displayB.Get() : slot.displayA.Get();
				Plan rcasPlan = plan;
				rcasPlan.sharpness = plan.act3.sharpness;
				if (DispatchRcas(list, slot, destColor, target, rcasPlan))
					finalColor = target;
			}
			return true;
		}

		bool DispatchAct3Blend(ID3D12GraphicsCommandList* list, Slot& slot, ID3D12Resource* source,
			ID3D12Resource* destination, const Plan& plan)
		{
			struct Constants {
				std::uint32_t dstOffsetX, dstOffsetY, subWidth, subHeight, blendMode;
				float featherWidth;
				std::uint32_t frameIndex, srcOffsetX;
				float ditherStrength;
				std::uint32_t pad0, pad1, pad2;
			} constants{
				plan.act3.cropX, plan.act3.cropY, plan.act3.cropWidth, plan.act3.cropHeight,
				plan.act3.blendMode == 2u ? 1u : 0u,
				plan.act3.featherWidth, static_cast<std::uint32_t>(plan.frameId), 0u,
				plan.act3.ditherStrength, 0u, 0u, 0u };
			if (plan.act3.blendMode == 0u)
				return false;
			const auto views = BindViews(slot, source, kColorFormat, destination, kColorFormat);
			if (!views.ptr)
				return false;
			Transition(list, source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			Transition(list, destination, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			if (!BeginDispatch(list, slot, subrectBlend.Get(), &constants, 12, views)) {
				Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
				Transition(list, destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
				return false;
			}
			list->Dispatch((plan.act3.cropWidth + 7) / 8, (plan.act3.cropHeight + 7) / 8, 1);
			Transition(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
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
		ComPtr<ID3D12PipelineState> resampleTent;
		ComPtr<ID3D12PipelineState> resampleMitchell;
		ComPtr<ID3D12PipelineState> resampleEasu;
		ComPtr<ID3D12PipelineState> resizeGuides;
		ComPtr<ID3D12PipelineState> guidanceFit;
		ComPtr<ID3D12PipelineState> fusion;
		ComPtr<ID3D12PipelineState> subrectBlend;
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