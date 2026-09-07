// Storm - the light volume build and the cloud march.
//
// Both run every frame in Phase 01. The light volume is what Spike 01 found:
// marching to the sun from every shading sample made lighting ~60% of the
// frame, and computing transmittance once into a coarse volume instead is
// 2.28x faster for a maximum image error of 4/255. It also decouples lighting
// resolution from screen resolution, which is why the volume can afford 24
// taps where the per-sample march could only afford 6.

#include "atmosphere.hlsli"
#include "clouds.hlsli"

// ---- coarse peak condensate -----------------------------------------------
//
// One cell per 4x4x4 block of simulation cells, rebuilt whenever the solver
// steps. It exists because the density mapping needs to know what "full" means
// locally, and there is no single answer: a tower core carries ten times the
// condensate of the anvil it feeds, and the erosion below is a threshold, so
// normalising both against one curve deletes the anvil outright rather than
// carving it. Normalised against its own neighbourhood, every part of the
// cloud presents the erosion with a field that reaches one in the interior and
// falls to zero at the edge, which is what the erosion was written for.
//
// The floor in sampleDensity is what stops that from turning a stray wisp into
// solid cloud just because it is locally the densest thing around.

[numthreads(4, 4, 4)]
void CSCloudMax(uint3 tid : SV_DispatchThreadID)
{
    int3 coarse = (gSimRes + 3) / 4;
    if (any(int3(tid) >= coarse)) return;

    float peak = 0.0;
    int3 base = int3(tid) * 4;
    for (int z = 0; z < 4; ++z)
    for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x)
    {
        int3 c = base + int3(x, y, z);
        if (any(c >= gSimRes)) continue;
        peak = max(peak, readSCurrent(c).b);
    }
    gCloudMaxRW[tid] = peak;
}

// ---- light volume ---------------------------------------------------------

[numthreads(4, 4, 4)]
void CSLightVolume(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid >= (uint3)gLightVolumeRes)) return;

    float3 uvw = (float3(tid) + 0.5) / float(gLightVolumeRes);
    float3 p   = lerp(cloudBoxMin(), cloudBoxMax(), uvw);

    // Geometrically growing stride: fine detail close to the sample where it
    // matters, and enough reach to leave the volume entirely.
    float  tau  = 0.0;
    float  step = 90.0;
    float3 lp   = p;

    for (int i = 0; i < 28; ++i)
    {
        lp   += gSunDirection * step;
        tau  += sampleDensity(lp, false) * step;
        step *= 1.14;
    }
    gLightVolumeRW[tid] = exp(-tau * kExtinction);
}

// ---- cloud march ----------------------------------------------------------

// Sunlight colour after crossing the atmosphere to reach a point.
float3 sunTransmittanceAt(float3 p)
{
    float3 origin = toPlanetSpace(p);
    float l0, l1;
    raySphere(origin, gSunDirection, kAtmoRadius, l0, l1);

    float seg = max(l1, 0.0) / 8.0;
    float odR = 0.0, odM = 0.0, t = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float h = max(length(origin + gSunDirection * (t + seg * 0.5)) - kEarthRadius, 0.0);
        odR += exp(-h / kScaleHeightR) * seg;
        odM += exp(-h / kScaleHeightM) * seg;
        t += seg;
    }
    return exp(-(kBetaRayleigh * odR + kBetaMie * 1.1 * odM));
}

// Writes in-scattered radiance and transmittance, both linear and both already
// carrying aerial perspective, so the composite is a single lerp and the
// temporal resolve can blend them without bias.
[numthreads(8, 8, 1)]
void CSCloud(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gHalfSize.x || tid.y >= (uint)gHalfSize.y) return;

    float3 rd = primaryRay(float2(tid.xy), gHalfSize, gJitter);
    float3 ro = gCamPos;

    float3 scattered = float3(0.0, 0.0, 0.0);
    float  transmittance = 1.0;
    float  weightedDistance = 0.0;
    float  weightSum = 0.0;

    float t0, t1;
    if (intersectCloud(ro, rd, t0, t1))
    {
        // Fixed stepping. Spike 01 measured cheap/expensive adaptive stepping
        // at 2.7x SLOWER than this, because inside a bounded volume a ray is
        // mostly *in* cloud once it enters and the coarse phase never earns
        // back the finer samples it forces.
        float stepLength = (t1 - t0) / float(gNumSteps);

        // Interleaved offset per pixel and per frame. Without it the fixed step
        // length lays down visible shells; with it, the temporal resolve turns
        // the noise into detail.
        float jitter = hash12(float2(tid.xy) + float2(gFrameIndex * 0.7548, gFrameIndex * 0.5698));

        // And a three-dimensional one for the light volume lookup, which is
        // coarse enough that its interpolation bands are visible on grazing
        // surfaces. See sampleLightVolume.
        float3 lightDither = (hash32(float2(tid.xy) + (float)gFrameIndex * 1.6180) - 0.5)
                           * lightVolumeCell();

        float  phase = cloudPhase(dot(rd, gSunDirection));
        float3 sunColour = sunTransmittanceAt(float3(gCloudCentre.x, gCloudBottom, gCloudCentre.z))
                         * gSunIntensity;
        float3 skyAmbient = float3(0.40, 0.56, 0.92) * gSunIntensity * kAmbientScale
                          * saturate(gSunDirection.y + 0.22);

        // Sunlit ground throws a warm bounce onto the underside. Kept small:
        // at the sky ambient's strength it floods the shadow side and the cloud
        // loses the tonal separation that gives it form.
        float3 groundBounce = float3(0.17, 0.15, 0.10) * sunColour
                            * saturate(gSunDirection.y) * 0.045;

        float t = t0 + stepLength * jitter;
        for (int i = 0; i < gNumSteps; ++i)
        {
            float3 p = ro + rd * t;
            float  rainShare, debrisShare;
            float  d = sampleDensityAndRain(p, true, rainShare, debrisShare);

            if (d > 0.0)
            {
                float tau = d * stepLength * kExtinction;
                float tr  = exp(-tau);

                float light  = sampleLightVolume(p, lightDither);
                // Powder darkens material a ray has only just entered. Taken
                // literally it falls to zero with the optical depth, which was
                // fine against Spike 02's analytic container but not against a
                // simulated field: numerical diffusion leaves a lot of very
                // thin material, and unfloored it renders as grey haze rather
                // than lit cloud. A floor keeps the wisps in the light.
                float powder = kPowderFloor
                             + (1.0 - kPowderFloor) * (1.0 - exp(-tau * kPowder));

                // Wrenninge multiple-scattering octaves. Single scattering
                // alone leaves interiors black, because direct transmittance
                // through a few hundred metres of cloud is essentially zero.
                float ms = light
                         + pow(light, 0.50) * 0.42
                         + pow(light, 0.25) * 0.18;

                float  h = normalisedHeight(p);
                float  below = (1.0 - h) * (1.0 - h);
                float3 ambient = skyAmbient * lerp(kAmbientLow, 1.0, h) + groundBounce * below;
                float3 lum = sunColour * ms * phase * powder * kSunGain + ambient;

                // Rain is the same medium geometrically and a much darker one
                // optically: far fewer, far larger drops, so it scatters less
                // of what reaches it. Shading it identically to cloud puts a
                // bright white column under the base, which is the one place
                // a storm is never bright.
                lum *= lerp(1.0, kRainAlbedo, rainShare);

                // Debris is dust, not water: darker still, and warm rather
                // than neutral. It is the one part of the storm that is not
                // some shade of grey.
                lum *= lerp(1.0, kDebrisAlbedo, debrisShare);
                lum = lerp(lum, lum * float3(1.35, 1.05, 0.72), debrisShare);

                // Lightning, added after the albedo term: a lit rain shaft is
                // one of the few times rain is brighter than the cloud above.
                lum += flashLight(p) * powder;

                float w = transmittance * (1.0 - tr);
                scattered        += w * lum;
                weightedDistance += w * t;
                weightSum        += w;

                transmittance *= tr;
                if (transmittance < 0.006) break;
            }
            t += stepLength;
        }
    }

    // Fold aerial perspective in here, where the distance is known, using the
    // transmittance-weighted mean depth of what the ray actually hit.
    float3 colour = float3(0.0, 0.0, 0.0);
    if (weightSum > 0.0)
    {
        float3 inscatter, apTransmittance;
        scatterAtmosphere(ro, rd, weightedDistance / weightSum, gSunDirection, gSunIntensity,
                          inscatter, apTransmittance);
        colour = scattered * apTransmittance + inscatter * (1.0 - transmittance);
    }

    gCloudCurrent[tid.xy] = float4(colour, transmittance);

    // The depth the resolve reprojects by. Zero where the ray hit nothing,
    // which the resolve reads as "infinitely far" and handles exactly.
    gCloudDepth[tid.xy] = (weightSum > 0.0) ? weightedDistance / weightSum : 0.0;
}
