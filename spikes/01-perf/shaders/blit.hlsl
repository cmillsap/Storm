// Storm - Spike 01
// Fullscreen-triangle blit of the raymarch target to the swap chain.
//
// The raymarch target is allocated once at the largest benchmark resolution,
// so a config that renders a smaller region samples only that sub-rect and
// lets the hardware scale it to the window.

Texture2D<float4> gRaymarchTarget : register(t2);
SamplerState      gClamp          : register(s1);

cbuffer Params : register(b0)
{
    float3 gCamPos;       float gTime;
    float3 gCamFwd;       float gTanHalfFov;
    float3 gCamRight;     float gAspect;
    float3 gCamUp;        float gCoverage;
    float2 gOutSize;      float2 gTexSize;
    int    gNumSteps;     int   gMode;         int   gLightSteps;  int   gFlags;
    float  gDensityScale; float gCloudBottom;  float gCloudTop;    float gEdgeSoftness;
    float3 gSunDir;       float gSigmaT;
    float3 gBoxCentre;    float gBoxHalfXZ;
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
    float2 scale = gOutSize / gTexSize;
    return gRaymarchTarget.SampleLevel(gClamp, i.uv * scale, 0);
}
