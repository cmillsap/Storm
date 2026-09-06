// Storm - presentation blit.
//
// Samples the shared render target into one monitor's swap chain. The crop
// rectangle is what makes mirroring work across displays of different shapes:
// the view takes the largest sub-rectangle of the target matching its own
// aspect, so nothing is letterboxed and nothing is stretched.

#include "common.hlsli"

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOut VSFullscreen(uint id : SV_VertexID)
{
    VSOut o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSBlit(VSOut i) : SV_Target
{
    return gSharedSRV.SampleLevel(gClamp, i.uv * gCropScale + gCropOffset, 0);
}
