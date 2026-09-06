// Storm - a cross-section through the simulation, for development only.
//
// The storm arc was tuned for a long time off outside views and a table of
// numbers, and both mislead: a lumpy silhouette does not say whether the
// updraft is tilted, whether the rain is falling where the cloud is, or
// whether the anvil is spreading or the tower is merely tall. This draws the
// fields themselves on a vertical plane through the middle of the domain.
//
// Background is vertical velocity - red rising, blue sinking. Over it, cloud
// in white and rain in blue-grey. The horizontal rules are the condensation
// level, the glaciation level and the equilibrium level, so the structure can
// be read against the sounding that produced it.

#include "clouds.hlsli"

static const float kSliceMaxW = 40.0;   // m/s at full colour

float3 rule(float3 colour, float worldY, float level, float metresPerPixel, float3 tint)
{
    return (abs(worldY - level) < metresPerPixel) ? tint : colour;
}

[numthreads(8, 8, 1)]
void CSSlice(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gFullSize.x || tid.y >= (uint)gFullSize.y) return;

    float3 lo = cloudBoxMin();
    float3 hi = cloudBoxMax();

    // Fit the domain into the target without distorting it: a stretched slice
    // would misreport every angle in the picture, and tilt is the thing this
    // exists to show.
    float2 uv = (float2(tid.xy) + 0.5) / gFullSize;
    float  domainAspect = (hi.x - lo.x) / (hi.y - lo.y);
    float  targetAspect = gFullSize.x / gFullSize.y;
    float2 fit = (targetAspect > domainAspect)
               ? float2(domainAspect / targetAspect, 1.0)
               : float2(1.0, targetAspect / domainAspect);
    float2 c = (uv - 0.5) / fit + 0.5;

    float3 background = float3(0.06, 0.06, 0.08);
    if (any(c < 0.0) || any(c > 1.0)) { gSharedTarget[tid.xy] = float4(background, 1.0); return; }

    float3 p = float3(lerp(lo.x, hi.x, c.x), lerp(hi.y, lo.y, c.y), (lo.z + hi.z) * 0.5);

    // Vertical velocity, signed.
    float w = sampleV(p);
    float3 colour = background;
    colour += float3(0.85, 0.20, 0.10) * saturate( w / kSliceMaxW);
    colour += float3(0.15, 0.35, 0.95) * saturate(-w / kSliceMaxW);

    float4 s = sampleScalars(p);
    float cloud = saturate(s.b / 0.010);
    float rain  = saturate(s.a / 0.0025);

    colour = lerp(colour, float3(1.0, 1.0, 1.0), cloud * 0.92);
    colour = lerp(colour, float3(0.32, 0.42, 0.55), rain * 0.85);

    // The funnel, in green, because it is the one thing on this plane that the
    // solver knows nothing about and it has to be possible to tell them apart.
    float debris;
    float funnel = tornadoDensity(p, debris);
    colour = lerp(colour, float3(0.25, 0.95, 0.45), saturate(funnel) * 0.9);

    // The sounding, drawn on the picture it produced.
    float metresPerPixel = (hi.y - lo.y) / (gFullSize.y * fit.y);
    float lcl = -gSatScale * log(gSurfaceRH);
    colour = rule(colour, p.y, lcl,          metresPerPixel, float3(0.25, 0.85, 0.45));
    colour = rule(colour, p.y, gGlaciation,  metresPerPixel, float3(0.35, 0.65, 0.95));
    colour = rule(colour, p.y, gEquilibrium, metresPerPixel, float3(0.95, 0.75, 0.25));
    if (gCapStrength > 0.05)
        colour = rule(colour, p.y, gCapHeight, metresPerPixel, float3(0.85, 0.35, 0.75));

    // And the margin the domain fades out across.
    float margin = lateralMargin(p);
    if (margin > 0.0 && margin < 0.02) colour = float3(0.45, 0.45, 0.50);

    gSharedTarget[tid.xy] = float4(pow(saturate(colour), 1.0 / 2.2), 1.0);
}
