// Storm - the render passes.
//
// Phase 00 draws the sky and the ground only. The cloud volume, the simulation
// and the temporal resolve arrive in Phases 01 and 02; the atmosphere here is
// the real Rayleigh/Mie integral from the look spike, not a placeholder
// gradient, so those phases extend this rather than replacing it.
#pragma once

#include "view.h"
#include <vector>

// Mirrors the cbuffer in sky.hlsl and blit.hlsl: 28 root constants. HLSL packs
// float3 + float into one 16-byte row, so this is row-for-row identical.
struct alignas(16) FrameConstants
{
    float camPos[3];      float time;
    float camForward[3];  float tanHalfFov;
    float camRight[3];    float aspect;
    float camUp[3];       float exposure;
    float outSize[2];     float texSize[2];
    float sunDirection[3];float sunIntensity;
    float cropScale[2];   float cropOffset[2];
};
static_assert(sizeof(FrameConstants) == 112, "FrameConstants must be 28 root constants");

// Chooses the shared target's dimensions for a set of views. Separated from
// allocation so the arithmetic can be checked without a graphics device - which
// is the only way to test a mixed-monitor arrangement on a machine that has one
// monitor.
void ComputeSharedTargetSize(const std::vector<View>& views, UINT& outWidth, UINT& outHeight);

struct Renderer
{
    Gpu* gpu = nullptr;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> skyPso;
    ComPtr<ID3D12PipelineState> blitPso;

    // Phase 00 renders exactly one target, mirrored everywhere. The vector is
    // the seam: independent cameras become one target per view, and the frame
    // loop below already iterates rather than assuming a single element.
    std::vector<RenderTarget> targets;

    bool initialise(Gpu& g);
    void shutdown();

    // Allocates the shared target. Sized so that every view gets at least its
    // native pixel count after cropping: the tallest view sets the height and
    // the widest aspect sets the width.
    bool createSharedTarget(const std::vector<View>& views);
    bool createTarget(UINT width, UINT height);   // explicit size, used by capture

    void renderTargets(float timeSeconds);
    void presentView(View& view, float timeSeconds);
    void finishFrame();          // returns targets to UAV state for the next pass

private:
    void fillConstants(FrameConstants& c, const RenderTarget& target, float timeSeconds) const;
};
