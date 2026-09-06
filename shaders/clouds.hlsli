// Storm - cloud density and lighting.
//
// The shape and lighting constants are exactly those arrived at in Spike 02.
// They are collected at the top because they are the values that get tuned; the
// shaders below compile at startup, so changing one and re-running is the whole
// iteration loop.

#ifndef STORM_CLOUDS_HLSLI
#define STORM_CLOUDS_HLSLI

#include "common.hlsli"

static const uint3 LIGHT_VOLUME_RES = uint3(64, 64, 64);

static const float BASE_SCALE     = 0.000210;  // world metres -> base volume uv
static const float DETAIL_SCALE   = 0.001450;  // world metres -> detail volume uv
static const float kExtinction    = 0.055;     // per metre, at density 1
static const float kBaseHardness  = 0.010;     // fraction of depth the base fades over
static const float kDetailErosion = 0.72;
static const float kEdgeBand      = 0.22;      // width of the density ramp at the silhouette
static const float kPowder        = 2.2;
static const float kAmbientLow    = 0.20;      // skylight reaching the cloud base
static const float kAmbientScale  = 0.040;
static const float kSunGain       = 2.4;
static const float kSilverLobe    = 0.78;      // forward Henyey-Greenstein lobe
static const float kBackLobe      = -0.28;
static const float kLobeMix       = 0.32;

float normalisedHeight(float3 p)
{
    return saturate((p.y - gCloudBottom) / (gCloudTop - gCloudBottom));
}

// Cumulus congestus profile: wide where it condenses at the base, bulging
// through the middle, drawing in toward a domed top. This is a container the
// noise gets carved out of - it never appears in the silhouette itself.
// Narrowing it at the base rounds the underside into a balloon and loses the
// flat bottom that defines the genus.
float containment(float3 p)
{
    float h = normalisedHeight(p);
    float profile = 0.88 + 0.30 * sin(kPi * pow(saturate(h), 0.75));
    profile *= 1.0 - smoothstep(0.70, 1.00, h) * 0.62;

    float2 d = (p.xz - gCloudCentre.xz) / max(gCloudRadius * profile, 1.0);
    return saturate(1.0 - dot(d, d));
}

float sampleDensity(float3 p, bool detail)
{
    float cont = containment(p);
    if (cont <= 0.0) return 0.0;

    float h = normalisedHeight(p);

    // A real cumulus base is startlingly flat - condensation switches on within
    // tens of metres of the lifting condensation level - but flat is not the
    // same as machined, so the level itself undulates by a few percent.
    float wobble = (gBaseNoise.SampleLevel(gWrap, p * 0.00031 + 13.7, 0).g - 0.5) * 0.075;
    float baseCut = smoothstep(wobble, wobble + kBaseHardness, h);
    float topFade = 1.0 - smoothstep(0.88, 1.00, h);

    float3 uvw = p * BASE_SCALE + float3(gTime * 0.00035, 0.0, 0.0);
    float4 b = gBaseNoise.SampleLevel(gWrap, uvw, 0);
    float  wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;

    // The lower bound must be allowed to go negative: that is what widens the
    // remap window and lets the Worley field carve the Perlin field.
    float shape = saturate(remap(b.r, wfbm - 1.0, 1.0, 0.0, 1.0));

    // The Perlin-Worley construction is biased high, so without a contrast
    // curve almost the whole volume clears the coverage threshold and the
    // silhouette becomes the container rather than the noise.
    shape = pow(shape, 2.0);

    // Spatially varying threshold. Where the container is strong the threshold
    // drops and most of the noise survives; toward the edges it rises until
    // nothing does. The silhouette is therefore drawn by the noise, and the
    // container must appear here and nowhere else - applying it to the shape as
    // well double-counts it and shrinks the cloud.
    float occupancy = gCoverage * cont * baseCut * topFade;
    float lo = 1.0 - occupancy;
    float d = saturate(remap(shape, lo, min(lo + kEdgeBand, 1.0), 0.0, 1.0));
    if (d <= 0.0) return 0.0;

    if (detail)
    {
        float3 dn = gDetailNoise.SampleLevel(gWrap, p * DETAIL_SCALE, 0).rgb;
        float  dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        // Wispy near the base, billowy up top - this is what separates a ragged
        // underside from a hard cauliflower crown.
        float  m = lerp(dfbm, 1.0 - dfbm, saturate(h * 4.0));
        d = saturate(remap(d, m * kDetailErosion, 1.0, 0.0, 1.0));
    }
    return d * gDensityScale;
}

// ---- bounds ---------------------------------------------------------------

float3 cloudBoxMin() { return float3(gCloudCentre.x - gCloudRadius, gCloudBottom, gCloudCentre.z - gCloudRadius); }
float3 cloudBoxMax() { return float3(gCloudCentre.x + gCloudRadius, gCloudTop,    gCloudCentre.z + gCloudRadius); }

bool intersectCloud(float3 ro, float3 rd, out float t0, out float t1)
{
    float3 a = (cloudBoxMin() - ro) / rd;
    float3 b = (cloudBoxMax() - ro) / rd;
    float3 lo = min(a, b);
    float3 hi = max(a, b);
    t0 = max(max(lo.x, lo.y), max(lo.z, 0.0));
    t1 = min(min(hi.x, hi.y), hi.z);
    return t1 > t0;
}

// ---- lighting -------------------------------------------------------------

float sampleLightVolume(float3 p)
{
    float3 uvw = (p - cloudBoxMin()) / (cloudBoxMax() - cloudBoxMin());
    return gLightVolume.SampleLevel(gClamp, uvw, 0);
}

float henyeyGreenstein(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float cloudPhase(float c)
{
    return lerp(henyeyGreenstein(c, kSilverLobe), henyeyGreenstein(c, kBackLobe), kLobeMix);
}

#endif
