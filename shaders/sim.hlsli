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
    float  gEquilibrium;   float gLapseTropo;   float gLapseStrato; float gSurfaceRH;
    float  gUpperRH;       float gRhTransition; float gSatSurface;  float gSatScale;
    float  gGlaciation;    float gIceEvaporation; float gFallout;   float gIceFallout;
    float  gFreezingLevel; float gEnvRelaxation; float gCapHeight;   float gCapHeightPrev;
    float  gCapStrength;   float gCapStrengthPrev; float gCapDepth;  float gSimPad1;
    float  gLateralMargin; float gLateralRate;  float gShearTop;    float gQcFloor;
    float2 gShear;         float2 gStormMotion;
    float  gWindRelaxation; float gRainFallSpeed; float gAutoThreshold; float gAutoRate;
    float  gAccretionRate;  float gRainEvaporation; float gRainOpaque; float gRotationSpeed;
    float  gRotationRadius; float gRotationBase;  float gRotationTop;  float gRotationRate;
    float  gRotationTilt;   float3 gSimPad0;
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

// Diagnostics. Written only by the stats pass, which nothing but the /arc
// harness dispatches: the storm arc has to be calibrated against numbers -
// cloud top against the prescribed equilibrium level, in particular - and a
// screenshot cannot tell you where the top is to within a hundred metres.
RWStructuredBuffer<uint> gSimStats : register(u18);

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

// Readers for the set that currently holds the data, as opposed to the
// destination set the step passes write. The stats pass runs outside a step
// and wants the former.
float4 readSCurrent(int3 c) { return (gSimPhase == 0) ? gSimS0[c] : gSimS1[c]; }
float  readVCurrent(int3 c) { return (gSimPhase == 0) ? gSimV0[c] : gSimV1[c]; }
float  readUCurrent(int3 c) { return (gSimPhase == 0) ? gSimU0[c] : gSimU1[c]; }
float  readWCurrent(int3 c) { return (gSimPhase == 0) ? gSimW0[c] : gSimW1[c]; }

// Face values averaged to the cell centre, which is where vorticity has to be
// evaluated if its two terms are to sit at the same point.
float centreU(int3 c)
{
    int xp = (c.x + 1) % gSimRes.x;
    return 0.5 * (readUCurrent(c) + readUCurrent(int3(xp, c.y, c.z)));
}

float centreW(int3 c)
{
    int zp = (c.z + 1) % gSimRes.z;
    return 0.5 * (readWCurrent(c) + readWCurrent(int3(c.x, c.y, zp)));
}

// ---- environment ----------------------------------------------------------

// Three layers, not two. The middle one is the conditionally unstable
// troposphere the plan always assumed; the one below it is new, and is what
// Phase 02 was missing. With a single 4 K/km lapse all the way to the ground,
// a thermal has to be heated for several minutes of storm time before it can
// climb the few hundred metres to its condensation level, so the first cloud
// appeared only as the forcing was already decaying. A real boundary layer is
// mixed and very nearly neutral, and a thermal crosses it almost for free.
// The capping inversion: a step of gCapStrength kelvin spread over gCapDepth
// metres, wherever the arc has put the lid. Adding it as a finite step rather
// than moving the stratosphere is what lets the lid rise between acts without
// rewriting the whole profile above it.
float capInversion(float y, float capHeight, float strength)
{
    return strength * smoothstep(capHeight, capHeight + gCapDepth, y);
}

float thetaEnv(float y)
{
    float base;
    if (y <= gMixedTop)          base = gLapseMixed * y;
    else
    {
        float mixed = gLapseMixed * gMixedTop;
        if (y <= gEquilibrium)   base = mixed + gLapseTropo * (y - gMixedTop);
        else                     base = mixed + gLapseTropo * (gEquilibrium - gMixedTop)
                                      + gLapseStrato * (y - gEquilibrium);
    }
    return base + capInversion(y, gCapHeight, gCapStrength);
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

// How glaciated the condensate at height y is. Ice is the anvil: it neither
// evaporates into dry air nor falls out at anything like the rate water does,
// and without it everything the tower detrains is gone within a step or two.
// The transition is spread over two kilometres rather than switched, because a
// hard boundary puts a visible seam across the cloud - and because the seam is
// not sharp in a real storm either.
float iceFraction(float y)
{
    return saturate((y - gGlaciation) / 2000.0);
}

// How much of the condensate at height y is still liquid water, and so able to
// collide its way into raindrops. Separate from the glaciation level above:
// that one governs whether the anvil survives, this one governs where rain can
// form at all, and running the two off one height gave a storm that was mostly
// precipitation.
float warmFraction(float y)
{
    return 1.0 - saturate((y - gFreezingLevel) / 1500.0);
}

// The environmental wind, in the storm-relative frame the solver runs in:
// the shear profile minus the storm's own motion. Zero shear leaves this the
// still air Phase 02 assumed.
float2 windEnv(float y)
{
    return gShear * min(y, gShearTop) * 0.001 - gStormMotion;
}

// ---- the mesocyclone ------------------------------------------------------
//
// A target swirl about the storm's axis. Rankine: solid body inside the core
// radius, falling as 1/r outside it, faded out well before the domain edge.
// The axis leans downstream with height because the tower does.

float2 rotationAxis(float y)
{
    return gForceCentre.xz + float2(gRotationTilt * max(y - gRotationBase, 0.0), 0.0);
}

// Target tangential speed at a world point, and zero outside the mesocyclone.
//
// What confines it is geometry, and getting that wrong is expensive. A core of
// 2.6 km faded out to 8.8 km covers most of the domain's width, and a rotating
// column that wide drags a broad layer up underneath it: 1.5 million cloudy
// cells out of 5.7 million, and a cloud radius past the box half-diagonal.
//
// The obvious repair - gate the swirl on where there is already cloud - is
// worse than the disease. It leaves the rotation unable to organise the inflow
// that feeds the storm, and all it then does is shred what it is applied to:
// the correlation stalled at 0.21, peak vorticity climbed as the small scales
// tore up, and cloud top fell from 10.8 km to 8.4 km as the swirl was raised.
// A mesocyclone is two to four kilometres across, and confining it to that is
// the whole of what it needs.
float rotationTarget(float3 p, out float2 tangent)
{
    float2 d = p.xz - rotationAxis(p.y);
    float  r = length(d);

    // Cyclonic, and the sign is not a coin toss. Vertical vorticity here is
    // zeta = du/dz - dw/dx, so solid-body rotation about +y - counter-clockwise
    // seen from above, which is what a Northern Hemisphere supercell does -
    // has velocity along (dz, -dx). The other way round gives an anticyclonic
    // storm and a correlation that runs monotonically negative, which is how
    // this was caught: the magnitude tracked the rotation asked for perfectly
    // and the sign was upside down.
    tangent = (r > 1.0) ? float2(d.y, -d.x) / r : float2(0.0, 0.0);
    if (gRotationSpeed <= 0.0) return 0.0;

    // Rankine, with the outer branch faded to nothing not far beyond the core.
    float core = gRotationRadius;
    float profile = (r < core) ? (r / core) : (core / max(r, 1.0));
    profile *= 1.0 - smoothstep(core * 1.4, core * 2.2, r);

    // Vertically, a window with soft ends: the mesocyclone occupies the storm
    // layer, not the boundary layer beneath it or the anvil above.
    float window = smoothstep(gRotationBase, gRotationBase + 1400.0, p.y)
                 * (1.0 - smoothstep(gRotationTop - 2600.0, gRotationTop, p.y));

    return gRotationSpeed * profile * window;
}

// 0 through the interior, rising to 1 at the very edge of the domain. The
// sides are periodic because the pressure solve wants them to be, so without
// this an anvil that reaches one edge comes back in at the other - which the
// /arc harness caught as a cloud radius jumping to 11 km, the half-diagonal of
// the box, the moment the anvil arrived.
float lateralMargin(float3 p)
{
    // Measured from the middle of the domain, not from the forcing: the
    // forcing deliberately sits upstream of centre, and a margin that moved
    // with it would be wider on one side than the other.
    float2 half = float2(gSimRes.x, gSimRes.z) * gSimCell * 0.5;
    float2 d = abs(p.xz - (gSimOrigin.xz + half)) / half;
    float  m = max(d.x, d.y);
    return saturate((m - (1.0 - gLateralMargin)) / max(gLateralMargin, 1e-3));
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

// Condensate and rain at a world point, for the renderer. Outside the grid
// there is no cloud - the box test in the raymarch keeps rays from ever asking.
float sampleCondensate(float3 world)
{
    return sampleScalars(world).b;
}

float sampleRain(float3 world)
{
    return sampleScalars(world).a;
}

#endif
