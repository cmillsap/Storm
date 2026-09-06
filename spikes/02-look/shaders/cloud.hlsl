// Storm - Spike 02: can one frame of cumulus be made to look good?
//
// Everything that only affects the look lives here as a named constant, so it
// can be tuned and re-run without rebuilding - the host compiles this file at
// startup.

#include "atmosphere.hlsli"

RWTexture2D<float4> gOutput         : register(u0);
RWTexture3D<float>  gLightVolumeOut : register(u3);
Texture3D<float4>   gBaseNoise      : register(t0);
Texture3D<float4>   gDetailNoise    : register(t1);
Texture3D<float>    gLightVolume    : register(t3);
SamplerState        gWrap           : register(s0);
SamplerState        gClamp          : register(s1);

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

static const uint3 LIGHT_VOLUME_RES = uint3(64, 64, 64);

// ---- look constants -------------------------------------------------------

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
static const float kSilverLobe    = 0.78;      // forward HG lobe
static const float kBackLobe      = -0.28;
static const float kLobeMix       = 0.32;
static const float kGroundY       = 0.0;

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

float normalisedHeight(float3 p)
{
    return saturate((p.y - gCloudBottom) / (gCloudTop - gCloudBottom));
}

// ---- shape ----------------------------------------------------------------

// Cumulus congestus profile: narrow where it condenses at the base, bulging
// through the middle where the updraft is widest, drawing in toward a domed
// top. This is a container the noise gets carved out of - it never appears in
// the silhouette itself.
float containment(float3 p)
{
    float h = normalisedHeight(p);

    // Wide at the base, bulging slightly through the middle, drawing into a
    // dome. The base must stay wide: narrowing the container at h=0 rounds the
    // underside off into a balloon, and a flat base is the defining feature of
    // the genus.
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
    // tens of metres of the lifting condensation level. Fading it over a few
    // hundred metres is the single most common way to make cloud look like fog.
    // ...but flat is not the same as machined. Undulating the condensation
    // level by a few percent of the cloud depth keeps the base reading as a
    // sharp edge while stopping it from looking sliced with a knife.
    float baseWobble = (gBaseNoise.SampleLevel(gWrap, p * 0.00031 + 13.7, 0).g - 0.5) * 0.075;
    float baseCut = smoothstep(baseWobble, baseWobble + kBaseHardness, h);
    float topFade = 1.0 - smoothstep(0.88, 1.00, h);

    float3 uvw = p * BASE_SCALE + float3(gTime * 0.00035, 0.0, 0.0);
    float4 b = gBaseNoise.SampleLevel(gWrap, uvw, 0);
    float  wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;

    float shape = saturate(remap(b.r, wfbm - 1.0, 1.0, 0.0, 1.0));
    shape = pow(shape, 2.0);

    // Spatially varying threshold. Where the container is strong the threshold
    // drops and most of the noise survives; toward the edges it rises until
    // nothing does. The silhouette is therefore drawn by the noise.
    float occupancy = gCoverage * cont * baseCut * topFade;
    float lo = 1.0 - occupancy;
    float d  = saturate(remap(shape, lo, min(lo + kEdgeBand, 1.0), 0.0, 1.0));
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

// ---- cloud bounds ---------------------------------------------------------

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

// ---- precomputed sun transmittance ---------------------------------------

float sampleLightVolume(float3 p)
{
    float3 uvw = (p - cloudBoxMin()) / (cloudBoxMax() - cloudBoxMin());
    return gLightVolume.SampleLevel(gClamp, uvw, 0);
}

[numthreads(4, 4, 4)]
void CSLightVolume(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid >= LIGHT_VOLUME_RES)) return;

    float3 uvw = (float3(tid) + 0.5) / float3(LIGHT_VOLUME_RES);
    float3 p   = lerp(cloudBoxMin(), cloudBoxMax(), uvw);

    float  tau  = 0.0;
    float  step = 90.0;
    float3 lp   = p;

    for (int i = 0; i < 28; ++i)
    {
        lp   += gSunDir * step;
        tau  += sampleDensity(lp, false) * step;
        step *= 1.14;
    }
    gLightVolumeOut[tid] = exp(-tau * kExtinction);
}

// ---- lighting -------------------------------------------------------------

float hg(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float cloudPhase(float c)
{
    return lerp(hg(c, kSilverLobe), hg(c, kBackLobe), kLobeMix);
}

// Sunlight colour after crossing the atmosphere to reach a point. Eight samples
// is plenty for a quantity this smooth, and it is what turns the sun warm and
// the cloud golden as the elevation drops.
float3 sunTransmittanceAt(float3 p)
{
    float3 origin = toPlanetSpace(p);
    float l0, l1;
    raySphere(origin, gSunDir, kAtmoRadius, l0, l1);

    float seg = max(l1, 0.0) / 8.0;
    float odR = 0.0, odM = 0.0, t = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float h = max(length(origin + gSunDir * (t + seg * 0.5)) - kEarthRadius, 0.0);
        odR += exp(-h / kScaleHeightR) * seg;
        odM += exp(-h / kScaleHeightM) * seg;
        t += seg;
    }
    return exp(-(kBetaRayleigh * odR + kBetaMie * 1.1 * odM));
}

// ---- ground ---------------------------------------------------------------

float groundCloudShadow(float3 p)
{
    float t0, t1;
    if (!intersectCloud(p, gSunDir, t0, t1)) return 1.0;

    const int N = 10;
    float seg = (t1 - t0) / float(N);
    float tau = 0.0;
    for (int i = 0; i < N; ++i)
        tau += sampleDensity(p + gSunDir * (t0 + seg * (float(i) + 0.5)), false) * seg;
    return exp(-tau * kExtinction);
}

float3 groundRadiance(float3 p, float3 sunCol, float3 skyAmb)
{
    // Variation across a few scales so the near ground reads as terrain rather
    // than a painted plane. The coarse term alone has a 2.4 km wavelength and
    // is invisible over the few hundred metres nearest the camera.
    float v = 0.5 + 0.30 * sin(p.x * 0.00042) * cos(p.z * 0.00037)
                  + 0.14 * sin(p.x * 0.0034 + 1.7) * cos(p.z * 0.0029)
                  + 0.06 * sin(p.x * 0.0130 + 0.4) * cos(p.z * 0.0115 + 2.1);
    float3 albedo = lerp(float3(0.055, 0.062, 0.036), float3(0.105, 0.098, 0.058), saturate(v));

    // The ground sees the whole sky dome, so its ambient term is much larger
    // than the fraction of it a point inside a cloud receives.
    float shadow = groundCloudShadow(p);
    float ndotl  = saturate(gSunDir.y);
    return albedo * (sunCol * ndotl * shadow + skyAmb * 5.0);
}

// ---- tonemap --------------------------------------------------------------

float3 aces(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// ---- main -----------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSCloud(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gOutSize.x || tid.y >= (uint)gOutSize.y) return;

    float2 uv  = (float2(tid.xy) + 0.5) / gOutSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    float3 rd = normalize(gCamFwd
                        + gCamRight * ndc.x * gTanHalfFov * gAspect
                        + gCamUp    * ndc.y * gTanHalfFov);
    float3 ro = gCamPos;

    float3 skyAmb = float3(0.40, 0.56, 0.92) * gSunIntensity * kAmbientScale
                  * saturate(gSunDir.y + 0.22);

    // --- background: ground or sky, both sitting in the atmosphere ---
    float3 background;
    float  backgroundDist = 1e9;

    // The ground is the planet, not a plane. A flat plane extended to infinity
    // dives beneath the spherical atmosphere, and those samples clamp to
    // sea-level density and register as shadowed, painting a dark band along
    // the horizon. Intersecting the sphere also puts the horizon at the correct
    // distance for the eye height - about 23 km from 40 m up.
    float gt0, gt1;
    bool  hitGround = raySphere(toPlanetSpace(ro), rd, kEarthRadius, gt0, gt1) && gt0 > 0.0;
    float groundT = hitGround ? gt0 : -1.0;
    if (groundT > 0.0)
    {
        float3 gp = ro + rd * groundT;
        float3 sunCol = sunTransmittanceAt(float3(gp.x, 100.0, gp.z)) * gSunIntensity;
        float3 lit = groundRadiance(gp, sunCol, skyAmb);

        float3 ins, tr;
        scatterAtmosphere(ro, rd, groundT, gSunDir, gSunIntensity, ins, tr);
        background = lit * tr + ins;

        // The last kilometres before the horizon have to converge onto the sky
        // sitting just above them. Aerial perspective over a ~23 km ground path
        // never reaches the brightness of the sky's full atmospheric path, so
        // without this the two meet in a hard dark step along the horizon.
        float horizonDist = sqrt(2.0 * kEarthRadius * max(ro.y, 1.0));
        float fade = smoothstep(0.45, 1.00, groundT / horizonDist);
        if (fade > 0.0)
        {
            float3 skyDir = normalize(float3(rd.x, max(rd.y, 0.0) + 0.0015, rd.z));
            background = lerp(background, skyRadiance(ro, skyDir, gSunDir, gSunIntensity), fade);
        }
        backgroundDist = groundT;
    }
    else
    {
        background = skyRadiance(ro, rd, gSunDir, gSunIntensity);
    }

    // --- cloud ---
    float3 scattered    = float3(0.0, 0.0, 0.0);
    float  transmittance = 1.0;
    float  weightedDist = 0.0;
    float  weightSum    = 0.0;

    float t0, t1;
    if (intersectCloud(ro, rd, t0, t1))
    {
        t1 = min(t1, backgroundDist);
        if (t1 > t0)
        {
            float stepLen = (t1 - t0) / float(gNumSteps);
            float jitter  = hash12(float2(tid.xy) + float2(gFrameIndex, gFrameIndex * 0.7));

            float  phase  = cloudPhase(dot(rd, gSunDir));
            float3 sunCol = sunTransmittanceAt(float3(gCloudCentre.x, gCloudBottom, gCloudCentre.z))
                          * gSunIntensity;

            // Sunlit ground throws a warm bounce onto the underside of the
            // cloud. It is a small term, but at a low sun it is the difference
            // between a grey base and one that sits in the same light as the
            // landscape below it.
            // Kept deliberately small. At a third of the sky ambient it warms
            // the underside; matching the sky ambient floods the shadow side
            // and the cloud loses the tonal separation that gives it form.
            float3 groundBounce = float3(0.17, 0.15, 0.10) * sunCol * saturate(gSunDir.y) * 0.045;

            float t = t0 + stepLen * jitter;
            for (int i = 0; i < gNumSteps; ++i)
            {
                float3 p = ro + rd * t;
                float  d = sampleDensity(p, true);

                if (d > 0.0)
                {
                    float tau = d * stepLen * kExtinction;
                    float tr  = exp(-tau);

                    // Jitter the lookup by a fraction of a voxel. Sampling a
                    // 64^3 volume on constant-t shells otherwise terraces into
                    // visible arcs through the shadowed body of the cloud.
                    float lightT = sampleLightVolume(p + rd * stepLen * (jitter - 0.5) * 2.0);
                    float powder = 1.0 - exp(-tau * kPowder);

                    // Multiple-scattering octaves. Single scattering alone
                    // leaves the interior black; the deeper octaves are what
                    // let light bleed through the body of the cloud.
                    float ms = lightT
                             + pow(lightT, 0.50) * 0.42
                             + pow(lightT, 0.25) * 0.18;

                    float  h       = normalisedHeight(p);
                    float  below   = (1.0 - h) * (1.0 - h);
                    float3 ambient = skyAmb * lerp(kAmbientLow, 1.0, h) + groundBounce * below;
                    float3 lum     = sunCol * ms * phase * powder * kSunGain + ambient;

                    float w = transmittance * (1.0 - tr);
                    scattered    += w * lum;
                    weightedDist += w * t;
                    weightSum    += w;

                    transmittance *= tr;
                    if (transmittance < 0.006) break;
                }
                t += stepLen;
            }
        }
    }

    // --- composite, with the cloud sitting in the same atmosphere ---
    float3 colour = background * transmittance;
    if (weightSum > 0.0)
    {
        float3 ins, tr;
        scatterAtmosphere(ro, rd, weightedDist / weightSum, gSunDir, gSunIntensity, ins, tr);
        colour += scattered * tr + ins * (1.0 - transmittance);
    }

    colour = aces(colour * gExposure);
    colour = pow(colour, 1.0 / 2.2);

    // Break up banding in the sky gradient before the 8-bit write.
    colour += (hash12(float2(tid.xy) + 17.3) - 0.5) / 255.0;

    gOutput[tid.xy] = float4(saturate(colour), 1.0);
}
