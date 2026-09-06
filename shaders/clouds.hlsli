// Storm - cloud density and lighting.
//
// Phase 02 replaces the analytic container from Spike 02 with the simulation's
// condensate field. The lighting constants are unchanged from the values
// arrived at in Spike 02; they are collected at the top because they are what
// gets tuned, and the shaders compile at startup so changing one and re-running
// is the whole iteration loop.

#ifndef STORM_CLOUDS_HLSLI
#define STORM_CLOUDS_HLSLI

#include "common.hlsli"
#include "sim.hlsli"

// The light volume resolution arrives in the frame constants rather than as
// a literal here. Held in two places it silently drifted apart: the host
// dispatched 96 while this said 64, so two thirds of the volume was never
// written and the cloud top sampled uninitialised memory as full shadow.

static const float BASE_SCALE     = 0.000210;  // world metres -> base volume uv
static const float DETAIL_SCALE   = 0.001450;  // world metres -> detail volume uv
static const float kExtinction    = 0.055;     // per metre, at density 1
static const float kDetailErosion = 0.72;
static const float kShapeErosion  = 0.32;      // larger-scale erosion of the sim boundary
static const float kAnvilErosion  = 0.16;      // how much of that survives in ice
static const float kRainErosion   = 0.30;      // and in a rain shaft
static const float kRainAlbedo    = 0.55;      // rain scatters less than cloud does
static const float kPowder        = 2.2;
static const float kPowderFloor   = 0.35;      // how lit the thinnest material stays
static const float kAmbientLow    = 0.20;      // skylight reaching the cloud base
static const float kAmbientScale  = 0.040;
static const float kSunGain       = 2.4;
static const float kSilverLobe    = 0.78;      // forward Henyey-Greenstein lobe
static const float kBackLobe      = -0.28;
static const float kLobeMix       = 0.32;
// Lightning. Slightly blue-white, and attenuated hard with distance: a real
// flash lights a limited volume because the cloud it is inside is optically
// thick, and an inverse-square falloff alone lights the whole storm evenly and
// reads as the sun coming on.
static const float3 kFlashColour  = float3(0.72, 0.80, 1.00);
static const float  kFlashReach   = 1400.0;   // metres, e-folding
static const float  kFlashGain    = 5.0;

float normalisedHeight(float3 p)
{
    return saturate((p.y - gCloudBottom) / (gCloudTop - gCloudBottom));
}

// ---- bounds ---------------------------------------------------------------
//
// The cloud volume is the simulation domain. Rays that miss it never sample.

float3 cloudBoxMin() { return gSimOrigin; }
float3 cloudBoxMax() { return gSimOrigin + float3(gSimRes) * gSimCell; }

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

// ---- density --------------------------------------------------------------
//
// The plan's central bet, made concrete: the solver supplies the low-frequency
// shape and the noise supplies texture. At 50 m cells the condensate field is
// far coarser than the features the eye reads as cauliflower, and numerical
// diffusion smooths its boundary further, so the same erosion that carved the
// analytic container in Spike 02 now carves this.

// Density, and how much of it is rain rather than cloud. The two are returned
// together because every caller that shades needs to know the difference: a
// rain shaft is the same medium geometrically and a much darker one optically.
float sampleDensityAndRain(float3 p, bool detail, out float rainShare)
{
    rainShare = 0.0;

    float4 s = sampleScalars(p);
    float qc = s.b;
    float qr = s.a;
    if (qc <= 0.0 && qr <= 0.0) return 0.0;

    // What counts as opaque is local, and this is the part of Phase 03 that
    // took longest to get right. Phase 02 normalised against one constant,
    // which works while the cloud is a 6.4 km cumulus whose core is the only
    // thing in the box. Over a 14 km storm it fails twice over: the /arc
    // profile measures peak condensate climbing from 0.0019 at the base to
    // 0.0117 in the anvil, and at any one height the periphery carries a tenth
    // of what the core does. Normalise against a constant, or against the
    // adiabatic curve, and the erosion below - which is a threshold - does not
    // carve the anvil, it deletes every sample of it.
    //
    // So the reference is the cloud's own peak in this neighbourhood, with a
    // floor. Every part of the cloud then hands the erosion a field that
    // reaches one in the interior and falls to zero at the edge, and the floor
    // is what keeps a stray wisp from being promoted to solid cloud on the
    // strength of being the densest thing near it.
    float3 uvw = (p - cloudBoxMin()) / (cloudBoxMax() - cloudBoxMin());
    float  localPeak = gCloudMax.SampleLevel(gClamp, uvw, 0);
    float  reference = max(gQcRef * localPeak, gQcFloor);
    float  cloud = saturate(qc / reference);

    // Rain is measured absolutely rather than against the neighbourhood: a
    // shaft is a shaft whether or not there is anything dense beside it, and
    // the whole point of it is that it is thinner than the cloud it falls out
    // of.
    float rain = saturate(qr / gRainOpaque);

    float d = max(cloud, rain);
    if (d <= 0.0) return 0.0;
    rainShare = (d > 0.0) ? saturate(rain / max(d, 1e-4)) * saturate(1.0 - cloud) : 0.0;

    if (detail)
    {
        float h = normalisedHeight(p);

        // The erosion is a threshold, and that is the whole difficulty with it.
        // It is written to carve a boundary out of a field that saturates to
        // one, so anything sitting below the threshold everywhere is not
        // carved but deleted - which is what happened to the first anvil that
        // reached the top of the domain. The tower's core is at or above one
        // and keeps its cauliflower; the anvil is detrained and diluted, an
        // eighth of that, and had every sample of it erased.
        //
        // Backing the erosion off with the ice fraction fixes it, and is what
        // the cloud looks like anyway: cauliflower is what liquid convection
        // does, and an anvil is a smooth fibrous sheet that has none of it.
        // Rain shafts get most of the erosion held off them too. Falling rain
        // is streaked along its own direction, not billowed: eroded like a
        // cumulus it breaks into floating lumps under the cloud base.
        float erosion = lerp(1.0, kAnvilErosion, iceFraction(p.y));
        erosion = lerp(erosion, kRainErosion, rainShare);

        // Larger-scale erosion first: breaks the smooth simulated boundary into
        // something with a silhouette.
        float4 b = gBaseNoise.SampleLevel(gWrap, p * BASE_SCALE * 2.2, 0);
        float  wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;
        d = saturate(remap(d, wfbm * kShapeErosion * erosion, 1.0, 0.0, 1.0));
        if (d <= 0.0) return 0.0;

        // Then the fine detail. Wispy near the base, billowy up top, which is
        // what separates a ragged underside from a hard cauliflower crown.
        float3 dn = gDetailNoise.SampleLevel(gWrap, p * DETAIL_SCALE, 0).rgb;
        float  dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        float  m = lerp(dfbm, 1.0 - dfbm, saturate(h * 4.0));
        d = saturate(remap(d, m * kDetailErosion * erosion, 1.0, 0.0, 1.0));
    }

    // Fade out across the same margin the solver relaxes in. The solver's
    // damping alone is not enough to hide the boundary, and the reason is the
    // local normalisation above: as the margin drives condensate down, the
    // local peak goes down with it, so the ratio stays near one and the anvil
    // stays fully opaque until it falls off a cliff. The first sheared storm
    // ended in a straight vertical edge in mid-air where its anvil met the
    // margin. Fading the rendered density directly is what removes it, and it
    // is honest about what it is: the domain ends, and a real anvil does not.
    return d * (1.0 - lateralMargin(p)) * gDensityScale;
}

// The shading passes that do not care which is which.
float sampleDensity(float3 p, bool detail)
{
    float rainShare;
    return sampleDensityAndRain(p, detail, rainShare);
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

// Radiance a sample receives from the current flash. No shadowing: a second
// transmittance volume for something that lasts a fifth of a second would cost
// a rebuild per frame, and the distance falloff is standing in for it. What
// sells it is that only part of the storm lights up.
float3 flashLight(float3 p)
{
    if (gFlashIntensity <= 0.0) return float3(0.0, 0.0, 0.0);
    float r = length(gFlashPosition - p);
    return kFlashColour * gFlashIntensity * exp(-r / kFlashReach) * kFlashGain;
}

#endif
