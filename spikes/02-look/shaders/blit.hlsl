// Storm - Spike 02
// Fullscreen-triangle blit of the render target to the swap chain.

Texture2D<float4> gRenderTarget : register(t2);
SamplerState      gClamp        : register(s1);

cbuffer Params : register(b0)
{
    float3 gCamPos;       float gTime;
    float3 gCamFwd;       float gTanHalfFov;
    float3 gCamRight;     float gAspect;
    float3 gCamUp;        float gCoverage;
    float2 gOutSize;      float2 gTexSize;
    int    gNumSteps;     int   gLightSteps;   int   gFlags;     int   gFrameIndex;
    float  gDensityScale; float gCloudBottom;  float gCloudTop;  float gCloudRadius;
    float3 gSunDir;       float gSunIntensity;
    float3 gCloudCentre;  float gExposure;
};

struct VSOut
{
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSFullscreen(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSBlit(VSOut i) : SV_Target
{
    return gRenderTarget.SampleLevel(gClamp, i.uv * (gOutSize / gTexSize), 0);
}
