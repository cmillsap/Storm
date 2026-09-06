// Storm - temporal resolve.
//
// The one part of the renderer no spike built, and the plan's largest remaining
// unknown: every spike rendered stills. Its job is to accumulate the per-frame
// jittered cloud samples so the march can be cheap and still look clean, and to
// keep the result stable while the camera drifts.

#include "clouds.hlsli"

float4 readHistory(float2 uv)
{
    // Ping-pong: gHistoryIndex names the buffer written last frame.
    return (gHistoryIndex == 0) ? gHistory1SRV.SampleLevel(gClamp, uv, 0)
                                : gHistory0SRV.SampleLevel(gClamp, uv, 0);
}

[numthreads(8, 8, 1)]
void CSResolve(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= (uint)gHalfSize.x || tid.y >= (uint)gHalfSize.y) return;

    int2 pixel = int2(tid.xy);
    float4 current = gCloudCurrent[pixel];

    float4 result = current;

    if (gHistoryValid > 0.5)
    {
        // Reproject through the world direction rather than a screen-space
        // motion vector. The camera rotates without translating, which makes
        // this exact and independent of depth - a volumetric buffer has no
        // single depth to reproject by in the first place.
        float3 dir = primaryRay(float2(pixel), gHalfSize, float2(0.0, 0.0));

        float2 prevUv;
        if (reprojectDirection(dir, prevUv))
        {
            float4 history = readHistory(prevUv);

            // Neighbourhood clamp. Without it, history that is no longer valid
            // - newly disoccluded cloud, or a shape that has moved - smears
            // across the frame as ghosting.
            float4 lo = current, hi = current;
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                [unroll] for (int x = -1; x <= 1; ++x)
                {
                    int2 q = clamp(pixel + int2(x, y), int2(0, 0),
                                   int2((int)gHalfSize.x - 1, (int)gHalfSize.y - 1));
                    float4 s = gCloudCurrent[q];
                    lo = min(lo, s);
                    hi = max(hi, s);
                }
            }
            history = clamp(history, lo, hi);

            result = lerp(current, history, gHistoryBlend);
        }
    }

    if (gHistoryIndex == 0) gCloudHistory0[pixel] = result;
    else                    gCloudHistory1[pixel] = result;
}
