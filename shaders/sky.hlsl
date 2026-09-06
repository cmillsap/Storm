// Storm - Phase 00 sky pass.
//
// Sky, sun and ground, with no cloud yet. The atmosphere is the real
// Rayleigh/Mie integral rather than a placeholder gradient, so Phase 01 adds a
// cloud march to this rather than replacing it. Everything the cloud will need
// is already here: aerial perspective, the sun's transmitted colour, and a
// ground that is the planet rather than a plane.

#include "atmosphere.hlsli"

RWTexture2D<float4> gOutput : register(u0);

cbuffer FrameConstants : register(b0)
{
    float3 gCamPos;       float gTime;
    float3 gCamForward;   float gTanHalfFov;
    float3 gCamRight;     float gAspect;
    float3 gCamUp;        float gExposure;
    float2 gOutSize;      float2 gTexSize;
    float3 gSunDirection; float gSunIntensity;
    float2 gCropScale;    float2 gCropOffset;
};

static const float kGroundY = 0.0;

float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Sunlight colour after crossing the atmosphere to reach a point. Eight samples
// is plenty for a quantity this smooth, and it is what turns the light warm as
// the sun drops.
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
    // Variation across a few scales so the near ground reads as terrain rather
    // than a painted plane.
    float v = 0.5 + 0.30 * sin(p.x * 0.00042) * cos(p.z * 0.00037)
                  + 0.14 * sin(p.x * 0.0034 + 1.7) * cos(p.z * 0.0029)
                  + 0.06 * sin(p.x * 0.0130 + 0.4) * cos(p.z * 0.0115 + 2.1);
    float3 albedo = lerp(float3(0.055, 0.062, 0.036), float3(0.105, 0.098, 0.058), saturate(v));

    // The ground sees the whole sky dome, so its ambient term is large.
    float ndotl = saturate(gSunDirection.y);
    return albedo * (sunColour * ndotl + skyAmbient * 5.0);
}

float3 aces(float3 x)
{
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

[numthreads(8, 8, 1)]
void CSSky(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gOutSize.x || tid.y >= (uint)gOutSize.y) return;

    float2 uv  = (float2(tid.xy) + 0.5) / gOutSize;
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    float3 rd = normalize(gCamForward
                        + gCamRight * ndc.x * gTanHalfFov * gAspect
                        + gCamUp    * ndc.y * gTanHalfFov);
    float3 ro = gCamPos;

    float3 skyAmbient = float3(0.40, 0.56, 0.92) * gSunIntensity * 0.040
                      * saturate(gSunDirection.y + 0.22);

    float3 colour;

    // The ground is the planet, not a plane. A flat plane extended to the
    // horizon dives beneath the spherical atmosphere; those samples clamp to
    // sea-level density and paint a dark band along the horizon.
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
        colour = lit * transmittance + inscatter;

        // The last kilometres before the horizon converge onto the sky just
        // above them; without this the two meet in a hard step.
        float horizonDistance = sqrt(2.0 * kEarthRadius * max(ro.y, 1.0));
        float fade = smoothstep(0.45, 1.00, gt0 / horizonDistance);
        if (fade > 0.0)
        {
            float3 skyDir = normalize(float3(rd.x, max(rd.y, 0.0) + 0.0015, rd.z));
            colour = lerp(colour, skyRadiance(ro, skyDir, gSunDirection, gSunIntensity), fade);
        }
    }
    else
    {
        colour = skyRadiance(ro, rd, gSunDirection, gSunIntensity);
    }

    colour = aces(colour * gExposure);
    colour = pow(colour, 1.0 / 2.2);

    // Break up banding in the sky gradient before the 8-bit write.
    colour += (hash12(float2(tid.xy) + frac(gTime) * 17.3) - 0.5) / 255.0;

    gOutput[tid.xy] = float4(saturate(colour), 1.0);
}
