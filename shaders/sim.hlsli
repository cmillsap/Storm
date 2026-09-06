// Storm - the cloud simulation grid.
//
// A staggered (MAC) grid, ported from the solvers validated in Spikes 03 and
// 04. Staggered rather than collocated because a collocated pressure solve
// admits an odd-even checkerboard mode, and the whole point of the simulation
// is to supply the low-frequency shape the eye reads as cloud.
//
// Axes follow world space, so the vertical is Y: u along x, v along y (the
// buoyant one), w along z. Periodic in x and z, rigid at the ground and lid.
//
// There is no vorticity confinement anywhere. Spike 03 measured it convecting
// a statically stable atmosphere from rest to 29.9 m/s and destroying placement
// control; the small-scale billowing it was meant to supply comes from
// render-time detail noise instead.

#ifndef STORM_SIM_HLSLI
#define STORM_SIM_HLSLI

#include "common.hlsli"

// The ping-pong phases live in the root constants (b1), not here: they change
// between dispatches within a single command list.
cbuffer SimParams : register(b2)
{
    float3 gSimOrigin;     float gSimCell;      // min corner in world metres, cell size
    int3   gSimRes;        float gForceDepth;   // vertical half-depth of the heated sheet
    float  gSimDt;         float gSimTime;      float gForceRamp;   float gQcRef;
    float3 gForceCentre;   float gForceRadius;
    float  gForceHeat;     float gForceMoisture; float gMixedTop;   float gLapseMixed;
    float  gTropopause;    float gLapseTropo;   float gLapseStrato; float gSurfaceRH;
    float  gUpperRH;       float gRhTransition; float gSatSurface;  float gSatScale;
};

// Velocity components live on cell faces, so each has its own texture. All
// three are allocated a row taller than the cell grid so the vertical
// component has somewhere to put its top face; the extra row is unused by u
// and w, which costs under one percent and keeps every dispatch uniform.
RWTexture3D<float>  gSimU0 : register(u7);
RWTexture3D<float>  gSimU1 : register(u8);
RWTexture3D<float>  gSimV0 : register(u9);
RWTexture3D<float>  gSimV1 : register(u10);
RWTexture3D<float>  gSimW0 : register(u11);
RWTexture3D<float>  gSimW1 : register(u12);
RWTexture3D<float4> gSimS0 : register(u13);   // theta, vapour, condensate
RWTexture3D<float4> gSimS1 : register(u14);
RWTexture3D<float>  gSimP0 : register(u15);
RWTexture3D<float>  gSimP1 : register(u16);
RWTexture3D<float>  gSimDiv : register(u17);

Texture3D<float>  gSimU0SRV : register(t7);
Texture3D<float>  gSimU1SRV : register(t8);
Texture3D<float>  gSimV0SRV : register(t9);
Texture3D<float>  gSimV1SRV : register(t10);
Texture3D<float>  gSimW0SRV : register(t11);
Texture3D<float>  gSimW1SRV : register(t12);
Texture3D<float4> gSimS0SRV : register(t13);
Texture3D<float4> gSimS1SRV : register(t14);

static const float kGravity      = 9.81;
static const float kTheta0       = 300.0;
static const float kLatentOverCp = 2488.0;   // K per unit condensed mixing ratio

// ---- environment ----------------------------------------------------------

// Three layers, not two. The middle one is the conditionally unstable
// troposphere the plan always assumed; the one below it is new, and is what
// Phase 02 was missing. With a single 4 K/km lapse all the way to the ground,
// a thermal has to be heated for several minutes of storm time before it can
// climb the few hundred metres to its condensation level, so the first cloud
// appeared only as the forcing was already decaying. A real boundary layer is
// mixed and very nearly neutral, and a thermal crosses it almost for free.
float thetaEnv(float y)
{
    if (y <= gMixedTop)   return gLapseMixed * y;
    float mixed = gLapseMixed * gMixedTop;
    if (y <= gTropopause) return mixed + gLapseTropo * (y - gMixedTop);
    return mixed + gLapseTropo * (gTropopause - gMixedTop) + gLapseStrato * (y - gTropopause);
}

float satVapour(float y) { return gSatSurface * exp(-y / gSatScale); }

// Moisture is specified as relative humidity against the saturation profile,
// never as an independent vapour profile. Given independently, any vapour
// scale height larger than the saturation scale height makes relative humidity
// climb with altitude until the environment is supersaturated on its own, and
// the domain fills with cloud before the first step.
float relHumidity(float y)
{
    float t = saturate((y - gMixedTop) / max(gRhTransition - gMixedTop, 1.0));
    return gSurfaceRH + (gUpperRH - gSurfaceRH) * t;
}

// Inside the mixed layer the vapour mixing ratio is constant, which is what
// "well mixed" means and is the reason a cumulus field has one flat base
// rather than a base per thermal: every parcel in the layer carries the same
// vapour, so every parcel condenses at the same height.
float vapourEnv(float y)
{
    if (y <= gMixedTop) return gSurfaceRH * gSatSurface;
    return relHumidity(y) * satVapour(y);
}

// ---- grid <-> world -------------------------------------------------------

float3 cellCentreWorld(int3 c)
{
    return gSimOrigin + (float3(c) + 0.5) * gSimCell;
}

float3 worldToGrid(float3 p) { return (p - gSimOrigin) / gSimCell; }

// ---- sampling -------------------------------------------------------------
//
// Each staggered component sits at a different place inside the cell, so each
// needs its own texel-space mapping. Texel (i,j,k) of a texture holds the value
// at grid coordinates:
//
//   u        (i,       j + 0.5, k + 0.5)     low-x face
//   v        (i + 0.5, j,       k + 0.5)     low-y face, the vertical one
//   w        (i + 0.5, j + 0.5, k      )     low-z face
//   scalars  (i + 0.5, j + 0.5, k + 0.5)     cell centre
//
// Sampling at grid position g therefore needs uv = (texelIndex + 0.5) / dims,
// which after substitution gives the expressions below. gSimSampler wraps in x
// and z and clamps in y, so the periodic horizontal is free and the lid never
// feeds into the ground.

float3 velocityDims() { return float3(gSimRes.x, gSimRes.y + 1, gSimRes.z); }
float3 cellDims()     { return float3(gSimRes); }

float sampleU(float3 world)
{
    float3 g = worldToGrid(world);
    float3 uv = float3(g.x + 0.5, g.y, g.z) / velocityDims();
    return (gSimPhase == 0) ? gSimU0SRV.SampleLevel(gSimSampler, uv, 0)
                            : gSimU1SRV.SampleLevel(gSimSampler, uv, 0);
}

float sampleV(float3 world)
{
    float3 g = worldToGrid(world);
    float3 uv = float3(g.x, g.y + 0.5, g.z) / velocityDims();
    return (gSimPhase == 0) ? gSimV0SRV.SampleLevel(gSimSampler, uv, 0)
                            : gSimV1SRV.SampleLevel(gSimSampler, uv, 0);
}

float sampleW(float3 world)
{
    float3 g = worldToGrid(world);
    float3 uv = float3(g.x, g.y, g.z + 0.5) / velocityDims();
    return (gSimPhase == 0) ? gSimW0SRV.SampleLevel(gSimSampler, uv, 0)
                            : gSimW1SRV.SampleLevel(gSimSampler, uv, 0);
}

float4 sampleScalars(float3 world)
{
    float3 uv = worldToGrid(world) / cellDims();
    return (gSimPhase == 0) ? gSimS0SRV.SampleLevel(gSimSampler, uv, 0)
                            : gSimS1SRV.SampleLevel(gSimSampler, uv, 0);
}

// Condensate at a world point, for the renderer. Outside the grid there is no
// cloud - the box test in the raymarch keeps rays from ever asking.
float sampleCondensate(float3 world)
{
    return sampleScalars(world).b;
}

#endif
