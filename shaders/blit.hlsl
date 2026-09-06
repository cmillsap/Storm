// Storm - presentation blit.
//
// Samples the shared render target into one monitor's swap chain. The crop
// rectangle is what makes mirroring work across displays of different shapes:
// the view takes the largest sub-rectangle of the target matching its own
// aspect, so nothing is letterboxed and nothing is stretched.

Texture2D<float4> gSource : register(t0);
SamplerState      gClamp  : register(s0);

cbuffer FrameConstants : register(b0)
{
    float3 gCamPos;       float gTime;
    float3 gCamForward;   float gTanHalfFov;
    float3 gCamRight;     float gAspect;
    float3 gCamUp;        float gExposure;
    float2 gOutSize;      float2 gTexSize;
    float3 gSunDirection; float gSunIntensity;
    float2 gCropScale;    float2 gCropOffset;
};

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut VSFullscreen(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSBlit(VSOut i) : SV_Target
{
    // The rendered region may be smaller than the allocated texture, so the
    // crop is applied in rendered-region space and then scaled into texture
    // space. In Phase 00 the two are identical, but keeping the term means a
    // quality tier that renders a sub-rect will not need this rewritten.
    float2 uv = (i.uv * gCropScale + gCropOffset) * (gOutSize / gTexSize);
    return gSource.SampleLevel(gClamp, uv, 0);
}
