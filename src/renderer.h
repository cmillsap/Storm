// Storm - the render passes.
//
// Phase 01 frame, in order:
//   light volume    sun transmittance through the cloud, at 64^3
//   cloud march     half resolution, fixed stepping, one light-volume fetch
//   temporal resolve  reproject and accumulate the jittered samples
//   composite       full resolution sky, sun and ground, cloud composited over
//   blit            per view, with that view's crop rectangle
#pragma once

#include "view.h"
#include "slots.h"
#include "simulation.h"
#include <vector>

// Mirrors the cbuffer in common.hlsli. HLSL packs float3 + float into one
// 16-byte row, so this is row-for-row identical.
struct alignas(16) FrameConstants
{
    float camPos[3];        float time;
    float camForward[3];    float tanHalfFov;
    float camRight[3];      float aspect;
    float camUp[3];         float exposure;

    float prevForward[3];   float prevTanHalfFov;
    float prevRight[3];     float historyValid;
    float prevUp[3];        float historyBlend;

    float sunDirection[3];  float sunIntensity;
    float cloudCentre[3];   float cloudRadius;

    float fullSize[2];      float halfSize[2];
    float jitter[2];        float pad0[2];

    float cloudBottom;      float cloudTop;     float coverage;     float densityScale;
    int32_t numSteps;       int32_t frameIndex; int32_t historyIndex; int32_t lightVolumeRes;
};
static_assert(sizeof(FrameConstants) % 16 == 0, "FrameConstants must be 16-byte aligned");

void ComputeSharedTargetSize(const std::vector<View>& views, UINT& outWidth, UINT& outHeight);

struct Renderer
{
    Gpu* gpu = nullptr;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> psoGenBase, psoGenDetail;
    ComPtr<ID3D12PipelineState> psoLightVolume, psoCloud, psoResolve, psoComposite, psoBlit;

    // Phase 01 renders one target, mirrored everywhere. The vector is the seam
    // for independent cameras later; the frame loop already iterates.
    std::vector<RenderTarget> targets;

    ComPtr<ID3D12Resource> cloudCurrent;
    ComPtr<ID3D12Resource> cloudHistory[2];
    ComPtr<ID3D12Resource> lightVolume;
    ComPtr<ID3D12Resource> baseNoise, detailNoise;
    ComPtr<ID3D12Resource> constantBuffer;
    uint8_t* constantsMapped = nullptr;

    Simulation simulation;

    UINT halfWidth = 0, halfHeight = 0;
    int  frameIndex = 0;
    int  historyIndex = 0;
    bool historyValid = false;
    bool noiseReady = false;
    bool lightVolumeReady = false;

    bool initialise(Gpu& g);
    void shutdown();

    bool createSharedTarget(const std::vector<View>& views);
    bool createTarget(UINT width, UINT height);

    void generateNoise();                    // once, at startup
    void renderTargets(float timeSeconds, float deltaSeconds);
    void presentView(View& view);
    void finishFrame();

private:
    bool createCloudBuffers(UINT fullWidth, UINT fullHeight);
    void fillConstants(FrameConstants& c, const RenderTarget& target, float timeSeconds);

    // Previous frame's basis, kept so the resolve can reproject.
    float m_prevForward[3] = { 0, 0, 1 };
    float m_prevRight[3]   = { 1, 0, 0 };
    float m_prevUp[3]      = { 0, 1, 0 };
    float m_prevTanHalfFov = 1.0f;
};
