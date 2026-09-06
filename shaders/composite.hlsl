// Storm - full-resolution composite.
//
// Sky, sun and ground are drawn at full resolution and stay sharp; only the
// cloud is half resolution, which it can afford to be because it is the
// low-frequency part of the image. Tonemapping happens once, here, after the
// cloud has been composited in linear light.

#include "atmosphere.hlsli"
#include "clouds.hlsli"

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

float3 groundRadiance(float3 p, float3 sunColour, float3 skyAmbient)
{
    float v = 0.5 + 0.30 * sin(p.x * 0.00042) * cos(p.z * 0.00037)
                  + 0.14 * sin(p.x * 0.0034 + 1.7) * cos(p.z * 0.0029)
                  + 0.06 * sin(p.x * 0.0130 + 0.4) * cos(p.z * 0.0115 + 2.1);
    float3 albedo = lerp(float3(0.055, 0.062, 0.036), float3(0.105, 0.098, 0.058), saturate(v));

    // The ground is shadowed by the cloud above it. One lookup into the light
    // volume: the same precomputation that lights the cloud also shadows the
    // ground, for free, as long as the point is under the volume.
    float3 lo = cloudBoxMin(), hi = cloudBoxMax();
    float shadow = 1.0;
    if (p.x > lo.x && p.x < hi.x && p.z > lo.z && p.z < hi.z)
        shadow = lerp(0.35, 1.0, sampleLightVolume(float3(p.x, lo.y + 1.0, p.z)));

    float ndotl = saturate(gSunDirection.y);
    return albedo * (sunColour * ndotl * shadow + skyAmbient * 5.0);
}

float4 readResolved(float2 uv)
{
    return (gHistoryIndex == 0) ? gHistory0SRV.SampleLevel(gClamp, uv, 0)
                                : gHistory1SRV.SampleLevel(gClamp, uv, 0);
}

[numthreads(8, 8, 1)]
void CSComposite(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gFullSize.x || tid.y >= (uint)gFullSize.y) return;

    float3 rd = primaryRay(float2(tid.xy), gFullSize, float2(0.0, 0.0));
    float3 ro = gCamPos;

    float3 skyAmbient = float3(0.40, 0.56, 0.92) * gSunIntensity * kAmbientScale
                      * saturate(gSunDirection.y + 0.22);

    // --- background: the planet, or the sky ---
    float3 background;

    float gt0, gt1;
    bool hitGround = raySphere(toPlanetSpace(ro), rd, kEarthRadius, gt0, gt1) && gt0 > 0.0;

    if (hitGround)
    {
        float3 groundPoint = ro + rd * gt0;
        float3 sunColour = sunTransmittanceAt(float3(groundPoint.x, 100.0, groundPoint.z))
                         * gSunIntensity;
        float3 lit = groundRadiance(groundPoint, sunColour, skyAmbient);

        float3 inscatter, transmittance;
        scatterAtmosphere(ro, rd, gt0, gSunDirection, gSunIntensity, inscatter, transmittance);
        background = lit * transmittance + inscatter;

        // The last kilometres before the horizon converge onto the sky just
        // above them; without this the two meet in a hard dark step.
        float horizonDistance = sqrt(2.0 * kEarthRadius * max(ro.y, 1.0));
        float fade = smoothstep(0.45, 1.00, gt0 / horizonDistance);
        if (fade > 0.0)
        {
            float3 skyDir = normalize(float3(rd.x, max(rd.y, 0.0) + 0.0015, rd.z));
            background = lerp(background, skyRadiance(ro, skyDir, gSunDirection, gSunIntensity), fade);
        }
    }
    else
    {
        background = skyRadiance(ro, rd, gSunDirection, gSunIntensity);
    }

    // --- cloud, bilinearly upsampled from half resolution ---
    float2 uv = (float2(tid.xy) + 0.5) / gFullSize;
    float4 cloud = readResolved(uv);

    float3 colour = background * cloud.a + cloud.rgb;

    colour = acesTonemap(colour * gExposure);
    colour = pow(colour, 1.0 / 2.2);
    colour += (hash12(float2(tid.xy) + frac(gTime) * 17.3) - 0.5) / 255.0;

    gSharedTarget[tid.xy] = float4(saturate(colour), 1.0);
}
