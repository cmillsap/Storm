// Storm - Spike 01
// Bare volumetric cloud raymarch, instrumented for cost measurement.
//
// Five modes of increasing realism share one march loop so the timing deltas
// between them isolate a single cost each:
//
//   0  analytic density          - loop overhead and ALU only, zero texture work
//   1  base volume               - 1 x 3D sample per step
//   2  base + detail volume      - 2 x 3D samples per step (the Nubis sampler)
//   3  adaptive cheap/expensive  - mode 2 plus empty-space skipping
//   4  adaptive + light march    - mode 3 plus an N-tap cone march to the sun
//
// Mode 4 is the shape of the production shader. Modes 0-2 step a fixed number
// of times so that (t[128] - t[64]) / 64 gives a clean marginal cost per step.
//
// The cloud occupies a finite box rather than an unbounded layer. That matches
// what the project actually renders - one storm sitting in open sky - and it is
// the only framing under which empty-space skipping means anything, since a
// slab that fills the view has no empty space to skip.

RWTexture2D<float4> gOutput         : register(u0);
RWTexture3D<float>  gLightVolumeOut : register(u3);
Texture3D<float4>   gBaseNoise      : register(t0);
Texture3D<float4>   gDetailNoise    : register(t1);
Texture3D<float>    gLightVolume    : register(t3);
SamplerState        gWrap           : register(s0);
SamplerState        gClamp          : register(s1);

// Resolution of the precomputed sun-transmittance volume. Self-shadowing in
// cloud is a low-frequency effect, so ~200 m voxels are ample.
static const uint3 LIGHT_VOLUME_RES = uint3(64, 48, 64);

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

// gFlags bit 0: suppress the transmittance early-out, so every ray takes
// exactly gNumSteps samples. Unrealistic, but it is the only way to measure
// the cost of a step without ray termination confounding the result.
static const int FLAG_NO_EARLY_OUT = 1;

// gFlags bit 1: the light march samples only the low-frequency base volume.
// Shadowing barely depends on the detail octaves, so this halves the sampler
// work in the inner loop.
static const int FLAG_LIGHT_BASE_ONLY = 2;

// One tile of the base volume spans ~3.8 km, so a 13 km wide storm sees three
// to four periods of it. At the previous 0.000105 a tile was ~9.5 km - larger
// than the volume itself - so the noise barely varied inside the box and the
// edge falloff ended up doing all the shaping, producing a filled cuboid.
static const float BASE_SCALE   = 0.000265;   // world metres -> base volume uv
static const float DETAIL_SCALE = 0.001100;   // world metres -> detail volume uv
static const float WIND         = 8.0;        // metres / second of drift

float remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) * (nhi - nlo) / (hi - lo);
}

// Vertical profile of a cumulus: hard flat base, body through most of the
// layer, eroded top. Concentrating density into the lowest third produces a
// flat raft rather than anything with towers in it.
float heightGradient(float h)
{
    float base = saturate(remap(h, 0.00, 0.08, 0.0, 1.0));
    float top  = saturate(remap(h, 0.55, 1.00, 1.0, 0.0));
    return base * top;
}

float normalisedHeight(float3 p)
{
    return saturate((p.y - gCloudBottom) / (gCloudTop - gCloudBottom));
}

// Thins the field toward the walls of the box so cloud does not terminate on a
// flat plane where the volume ends.
float edgeFade(float3 p)
{
    float2 q = abs(p.xz - gBoxCentre.xz) / gBoxHalfXZ;
    return smoothstep(0.0, gEdgeSoftness, 1.0 - max(q.x, q.y));
}

// Mode 0. A few trig terms standing in for the density field, so the march
// loop, the ray setup and the accumulation are all still paid for but no
// memory is touched.
float densityAnalytic(float3 p)
{
    float3 q = p * 0.00035 + float3(gTime * 0.02, 0.0, 0.0);
    float  n = sin(q.x) * cos(q.z)
             + 0.5 * sin(q.x * 2.3 + q.y * 0.7) * cos(q.z * 1.9)
             + 0.25 * sin(q.x * 4.1) * cos(q.z * 3.7 + q.y);
    float shape = saturate(n * 0.35 + 0.5);
    shape *= heightGradient(normalisedHeight(p)) * edgeFade(p);
    return saturate(remap(shape, 1.0 - gCoverage, 1.0, 0.0, 1.0)) * gDensityScale;
}

// Low-frequency shape from the base volume: one 3D sample.
float densityBase(float3 p)
{
    float  h   = normalisedHeight(p);
    float3 uvw = p * BASE_SCALE + float3(gTime * WIND * BASE_SCALE, 0.0, 0.0);

    float4 base = gBaseNoise.SampleLevel(gWrap, uvw, 0);
    float  wfbm = base.g * 0.625 + base.b * 0.25 + base.a * 0.125;

    // The lower bound must be allowed to go negative - that is what widens the
    // remap window and lets the Worley field carve the Perlin field. Clamping
    // it to zero makes this an identity mapping and throws away all structure.
    float shape = saturate(remap(base.r, wfbm - 1.0, 1.0, 0.0, 1.0));

    // The Perlin-Worley construction is biased high - typical samples land
    // around 0.8 - so without a contrast curve almost the whole volume clears
    // the coverage threshold and the silhouette becomes the bounding box rather
    // than the noise. This spreads the distribution so only peaks survive.
    shape = pow(shape, 2.5);

    shape *= heightGradient(h) * edgeFade(p);
    shape  = saturate(remap(shape, 1.0 - gCoverage, 1.0, 0.0, 1.0));
    return shape * gDensityScale;
}

// Shape plus high-frequency erosion: two 3D samples. The detail volume flips
// from wispy to billowy with altitude, which is what separates a ragged cloud
// base from a hard cauliflower top.
float densityFull(float3 p)
{
    float shape = densityBase(p);
    if (shape <= 0.0) return 0.0;

    float  h = normalisedHeight(p);
    float3 d = gDetailNoise.SampleLevel(gWrap, p * DETAIL_SCALE, 0).rgb;
    float  dfbm = d.r * 0.625 + d.g * 0.25 + d.b * 0.125;
    float  modifier = lerp(dfbm, 1.0 - dfbm, saturate(h * 5.0));

    return saturate(remap(shape / gDensityScale, modifier * 0.50, 1.0, 0.0, 1.0)) * gDensityScale;
}

float densityAt(float3 p, int mode)
{
    if (mode == 0) return densityAnalytic(p);
    if (mode == 1) return densityBase(p);
    return densityFull(p);
}

// Slab-method ray/box intersection against the storm volume.
bool intersectBox(float3 ro, float3 rd, out float t0, out float t1)
{
    float3 half3 = float3(gBoxHalfXZ, (gCloudTop - gCloudBottom) * 0.5, gBoxHalfXZ);
    float3 centre = float3(gBoxCentre.x, (gCloudTop + gCloudBottom) * 0.5, gBoxCentre.z);

    float3 invDir = 1.0 / rd;
    float3 a = (centre - half3 - ro) * invDir;
    float3 b = (centre + half3 - ro) * invDir;
    float3 lo = min(a, b);
    float3 hi = max(a, b);

    t0 = max(max(lo.x, lo.y), max(lo.z, 0.0));
    t1 = min(min(hi.x, hi.y), hi.z);
    return t1 > t0;
}

float henyeyGreenstein(float cosTheta, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (12.566370614 * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

// Six-tap cone march toward the sun. Samples spread with distance, which
// cheaply approximates a wider light-gathering volume further from the surface.
float sunTransmittance(float3 p, int taps)
{
    const float3 kCone[6] =
    {
        float3( 0.38,  0.19, -0.90), float3(-0.55,  0.71,  0.44),
        float3( 0.63, -0.66,  0.41), float3(-0.27, -0.44, -0.85),
        float3( 0.81,  0.52,  0.26), float3( 0.00,  0.00,  0.00)
    };

    float tau  = 0.0;
    float step = 90.0;
    float3 lp  = p;
    bool  baseOnly = (gFlags & FLAG_LIGHT_BASE_ONLY) != 0;

    for (int i = 0; i < taps; ++i)
    {
        lp += gSunDir * step + kCone[min(i, 5)] * step * 0.35 * float(i);
        tau += (baseOnly ? densityBase(lp) : densityFull(lp)) * step;
        step *= 1.45;
    }
    return exp(-tau * gSigmaT);
}

// --- Precomputed sun transmittance -----------------------------------------
//
// Marching to the sun from every shading sample makes the light march the
// single most expensive thing in the frame: it multiplies the density sampler
// by the tap count, on every accumulation step, every frame. But the answer it
// computes barely changes - the sun moves slowly and the cloud moves slowly.
// So compute it once per simulation tick into a coarse volume and read it back
// with a single fetch.

float3 lightVolumeMin() { return float3(gBoxCentre.x - gBoxHalfXZ, gCloudBottom, gBoxCentre.z - gBoxHalfXZ); }
float3 lightVolumeMax() { return float3(gBoxCentre.x + gBoxHalfXZ, gCloudTop,    gBoxCentre.z + gBoxHalfXZ); }

float sampleLightVolume(float3 p)
{
    float3 uvw = (p - lightVolumeMin()) / (lightVolumeMax() - lightVolumeMin());
    return gLightVolume.SampleLevel(gClamp, uvw, 0);
}

[numthreads(4, 4, 4)]
void CSLightVolume(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid >= LIGHT_VOLUME_RES)) return;

    float3 uvw = (float3(tid) + 0.5) / float3(LIGHT_VOLUME_RES);
    float3 p   = lerp(lightVolumeMin(), lightVolumeMax(), uvw);

    // Geometrically growing stride covers the ~18 km box diagonal in 24 taps.
    float  tau  = 0.0;
    float  step = 120.0;
    float3 lp   = p;

    for (int i = 0; i < 24; ++i)
    {
        lp  += gSunDir * step;
        tau += densityBase(lp) * step;
        step *= 1.16;
    }

    gLightVolumeOut[tid] = exp(-tau * gSigmaT);
}

// Light reaching a sample: direct sun through the cloud, plus skylight that
// grows with altitude because the top of a cloud sees more of the sky dome.
// The powder term darkens the first few metres a ray travels into dense
// material, which gives cumulus its crisp bright rim and dark core.
float3 inScatter(float3 p, float sunVisibility, float phase, float tau)
{
    float h      = normalisedHeight(p);
    float powder = 1.0 - exp(-tau * 2.0);

    // Wrenninge multiple-scattering octaves: evaluate the same path three times
    // with progressively weaker extinction. Single scattering alone leaves cloud
    // interiors black, because by the time light has crossed a few hundred
    // metres of dense material its direct transmittance is essentially zero -
    // real clouds are bright inside precisely because the light bounces.
    float ms = pow(sunVisibility, 1.00) * 1.00
             + pow(sunVisibility, 0.50) * 0.45
             + pow(sunVisibility, 0.25) * 0.20;

    float3 sun = float3(1.00, 0.96, 0.90) * 3.8 * ms * phase * powder;
    float3 sky = lerp(float3(0.30, 0.38, 0.52), float3(0.90, 0.95, 1.05), h) * 1.05;
    return sun + sky;
}

float3 skyColour(float3 rd)
{
    float  up      = saturate(rd.y * 0.5 + 0.5);
    float3 horizon = float3(0.66, 0.74, 0.85);
    float3 zenith  = float3(0.16, 0.34, 0.66);
    float3 sky     = lerp(horizon, zenith, pow(up, 0.85));
    float  sun     = pow(saturate(dot(rd, gSunDir)), 900.0);
    return sky + float3(1.6, 1.4, 1.1) * sun;
}

[numthreads(8, 8, 1)]
void CSRaymarch(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gOutSize.x || tid.y >= (uint)gOutSize.y) return;

    float2 uv  = (float2(tid.xy) + 0.5) / gOutSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    float3 rd = normalize(gCamFwd
                        + gCamRight * ndc.x * gTanHalfFov * gAspect
                        + gCamUp    * ndc.y * gTanHalfFov);
    float3 ro = gCamPos;

    float3 colour = skyColour(rd);
    float  t0, t1;

    if (intersectBox(ro, rd, t0, t1))
    {
        float marchDist = t1 - t0;
        float stepLen   = marchDist / float(gNumSteps);

        // Interleaved gradient noise offset: breaks up the banding a fixed
        // step length would otherwise produce, at no cost.
        float jitter = frac(52.9829189 * frac(dot(float2(tid.xy), float2(0.06711056, 0.00583715))));

        float  transmittance = 1.0;
        float3 scattered     = 0.0.xxx;
        float  cosTheta      = dot(rd, gSunDir);
        float  phase         = lerp(henyeyGreenstein(cosTheta, 0.80),
                                    henyeyGreenstein(cosTheta, -0.15), 0.4);

        if (gMode <= 2)
        {
            // Fixed stepping. Every pixel that hits the box pays exactly
            // gNumSteps samples unless it saturates, so timing scales cleanly
            // with step count.
            float t = t0 + stepLen * jitter;
            for (int i = 0; i < gNumSteps; ++i)
            {
                float3 p = ro + rd * t;
                float  d = densityAt(p, gMode);
                if (d > 0.0)
                {
                    float tau  = d * stepLen * gSigmaT;
                    float tr   = exp(-tau);
                    scattered += transmittance * (1.0 - tr) * inScatter(p, 1.0, phase, tau);
                    transmittance *= tr;
                    if (transmittance < 0.01 && (gFlags & FLAG_NO_EARLY_OUT) == 0) break;
                }
                t += stepLen;
            }
        }
        else
        {
            // Adaptive stepping. Stride coarsely through empty air sampling only
            // the base volume; on contact, back up and switch to fine steps with
            // full detail until six consecutive samples come back empty.
            const int   kZeroRunToExit = 6;
            const float kCoarse        = 2.0;
            const float kFine          = 0.5;

            float t       = t0 + stepLen * jitter;
            bool  coarse  = true;
            int   zeroRun = 0;
            int   budget  = gNumSteps * 2;

            for (int i = 0; i < budget && t < t1; ++i)
            {
                float3 p = ro + rd * t;

                if (coarse)
                {
                    if (densityBase(p) > 0.0)
                    {
                        coarse  = false;
                        zeroRun = 0;
                        t       = max(t - stepLen * kCoarse, t0);
                        continue;
                    }
                    t += stepLen * kCoarse;
                }
                else
                {
                    float d = densityFull(p);
                    if (d > 0.0)
                    {
                        zeroRun = 0;
                        float fine = stepLen * kFine;
                        float tau  = d * fine * gSigmaT;
                        float tr   = exp(-tau);

                        float light = 1.0;
                        if      (gMode == 4) light = sunTransmittance(p, gLightSteps);
                        else if (gMode == 5) light = sampleLightVolume(p);
                        scattered  += transmittance * (1.0 - tr) * inScatter(p, light, phase, tau);

                        transmittance *= tr;
                        if (transmittance < 0.01 && (gFlags & FLAG_NO_EARLY_OUT) == 0) break;
                    }
                    else if (++zeroRun >= kZeroRunToExit)
                    {
                        coarse = true;
                    }
                    t += stepLen * kFine;
                }
            }
        }

        colour = colour * transmittance + scattered;

        // Aerial perspective, weighted by how much cloud the ray actually
        // accumulated. Applying it to every ray that merely enters the bounding
        // box paints the box itself onto the sky as a visible quadrilateral.
        float haze = saturate(1.0 - exp(-t0 * 0.000018));
        colour = lerp(colour, float3(0.72, 0.78, 0.86), haze * 0.55 * (1.0 - transmittance));
    }

    // Filmic-ish shoulder, then gamma. Enough to judge the image by eye.
    colour = colour / (1.0 + colour);
    colour = pow(saturate(colour), 1.0 / 2.2);

    gOutput[tid.xy] = float4(colour, 1.0);
}
