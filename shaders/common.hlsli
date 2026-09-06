// Storm - constants and bindings shared by every pass.
//
// One definition of the frame constants, included everywhere. The spikes
// repeated the cbuffer in each shader and it was a standing invitation to
// desync a field and spend an afternoon on it.

#ifndef STORM_COMMON_HLSLI
#define STORM_COMMON_HLSLI

cbuffer Frame : register(b0)
{
    float3 gCamPos;        float gTime;
    float3 gCamForward;    float gTanHalfFov;
    float3 gCamRight;      float gAspect;
    float3 gCamUp;         float gExposure;

    // Previous frame's basis, for temporal reprojection.
    float3 gPrevForward;   float gPrevTanHalfFov;
    float3 gPrevRight;     float gHistoryValid;
    float3 gPrevUp;        float gHistoryBlend;

    float3 gSunDirection;  float gSunIntensity;
    float3 gCloudCentre;   float gCloudRadius;

    float2 gFullSize;      float2 gHalfSize;
    float2 gJitter;        float2 gPad0;

    float  gCloudBottom;   float gCloudTop;    float gCoverage;    float gDensityScale;
    int    gNumSteps;      int   gFrameIndex;  int   gHistoryIndex; int  gLightVolumeRes;

    float3 gFlashPosition; float gFlashIntensity;
};

// Values that change between dispatches inside one command list, so they
// cannot live in a shared constant buffer: the crop rectangle differs per
// monitor, and the ping-pong phases alternate per simulation pass and per
// Jacobi iteration. Root constants, set immediately before each dispatch.
cbuffer Push : register(b1)
{
    float2 gCropScale;
    float2 gCropOffset;
    int    gSimPhase;      // which simulation set currently holds the data
    int    gJacobiPhase;   // which pressure buffer the current iteration reads
    int2   gPushPad;
};

RWTexture2D<float4> gSharedTarget  : register(u0);   // full resolution, RGBA8
RWTexture2D<float4> gCloudCurrent  : register(u1);   // half resolution, RGBA16F
RWTexture2D<float4> gCloudHistory0 : register(u2);
RWTexture2D<float4> gCloudHistory1 : register(u3);
RWTexture3D<float>  gLightVolumeRW : register(u4);
RWTexture3D<float4> gBaseNoiseRW   : register(u5);
RWTexture3D<float4> gDetailNoiseRW : register(u6);
// Peak condensate over each 4x4x4 block of simulation cells, rebuilt at
// simulation rate. Two jobs: it is what the density mapping normalises
// against, and it is what tells the march where there is nothing to march
// through. (u19 - the simulation owns u7..u18.)
RWTexture3D<float>  gCloudMaxRW    : register(u19);

Texture2D<float4>   gSharedSRV     : register(t0);
Texture2D<float4>   gCloudCurrSRV  : register(t1);
Texture2D<float4>   gHistory0SRV   : register(t2);
Texture2D<float4>   gHistory1SRV   : register(t3);
Texture3D<float>    gLightVolume   : register(t4);
Texture3D<float4>   gBaseNoise     : register(t5);
Texture3D<float4>   gDetailNoise   : register(t6);
Texture3D<float>    gCloudMax      : register(t15);

SamplerState gClamp : register(s0);
SamplerState gWrap  : register(s1);
// Wraps horizontally and clamps vertically, matching the simulation's periodic
// sides and rigid ground and lid.
SamplerState gSimSampler : register(s2);

static const float kPi = 3.14159265;

float remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) * (nhi - nlo) / (hi - lo);
}

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Primary ray for a pixel centre, with the frame's sub-pixel jitter applied.
float3 primaryRay(float2 pixel, float2 size, float2 jitter)
{
    float2 uv  = (pixel + 0.5 + jitter) / size;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    return normalize(gCamForward
                   + gCamRight * ndc.x * gTanHalfFov * gAspect
                   + gCamUp    * ndc.y * gTanHalfFov);
}

// Where a world-space direction sat on screen last frame. The camera rotates
// but does not translate in Phase 00/01, which makes this exact and
// independent of depth. Once the camera starts translating, this needs the
// cloud's mean distance carried alongside the colour.
bool reprojectDirection(float3 dir, out float2 prevUv)
{
    prevUv = float2(0.0, 0.0);
    float z = dot(dir, gPrevForward);
    if (z <= 1e-4) return false;

    float x = dot(dir, gPrevRight) / (z * gPrevTanHalfFov * gAspect);
    float y = dot(dir, gPrevUp)    / (z * gPrevTanHalfFov);
    if (abs(x) > 1.0 || abs(y) > 1.0) return false;

    prevUv = float2(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return true;
}

float3 acesTonemap(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

#endif
