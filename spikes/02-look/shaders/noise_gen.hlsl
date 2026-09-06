// Storm - Spike 01
// Tileable 3D noise generation for the cloud density sampler.
//
// Produces the two volumes a Nubis-style sampler expects:
//   base    128^3 RGBA8 : R = Perlin-Worley, GBA = Worley fbm at 3 frequencies
//   detail   32^3 RGBA8 : RGB = Worley fbm at 3 frequencies
//
// Both tile seamlessly: every hash is taken on cell coordinates wrapped by the
// octave frequency, so sampling with a wrap sampler has no visible seam.

RWTexture3D<float4> gBaseOut   : register(u1);
RWTexture3D<float4> gDetailOut : register(u2);

static const float BASE_RES   = 128.0;
static const float DETAIL_RES = 32.0;

float remap(float v, float lo, float hi, float nlo, float nhi)
{
    return nlo + (v - lo) * (nhi - nlo) / (hi - lo);
}

// Three decorrelated 32-bit hashes folded into [0,1)^3.
float3 hash33(float3 p)
{
    uint3 q = uint3(int3(p)) * uint3(1597334673u, 3812015801u, 2798796415u);
    q = (q.x ^ q.y ^ q.z) * uint3(1597334673u, 3812015801u, 2798796415u);
    return float3(q) * (1.0 / 4294967296.0);
}

// Cellular (Worley) noise. Returns distance to the nearest feature point,
// saturated to [0,1]. freq must be integral for the result to tile.
float worley(float3 uvw, float freq)
{
    float3 p  = uvw * freq;
    float3 id = floor(p);
    float3 fd = frac(p);

    float minDistSq = 1e9;
    for (int z = -1; z <= 1; ++z)
    {
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                float3 offset = float3(x, y, z);
                float3 cell   = fmod(id + offset + freq, freq);   // wrap for tiling
                float3 delta  = offset + hash33(cell) - fd;
                minDistSq     = min(minDistSq, dot(delta, delta));
            }
        }
    }
    return saturate(sqrt(minDistSq));
}

// Inverted Worley fbm: bright at cell centres, which is the shape that reads
// as cauliflower once it erodes a cloud edge.
float worleyFbm(float3 uvw, float freq)
{
    return (1.0 - worley(uvw, freq       )) * 0.625
         + (1.0 - worley(uvw, freq * 2.0 )) * 0.250
         + (1.0 - worley(uvw, freq * 4.0 )) * 0.125;
}

float3 gradient(float3 cell)
{
    return normalize(hash33(cell) * 2.0 - 1.0 + 1e-5);
}

// Tileable Perlin gradient noise, roughly [-0.7, 0.7].
float perlin(float3 uvw, float freq)
{
    float3 p = uvw * freq;
    float3 i = floor(p);
    float3 f = frac(p);
    float3 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);

    float n = 0.0;
    for (int c = 0; c < 8; ++c)
    {
        float3 o    = float3(c & 1, (c >> 1) & 1, (c >> 2) & 1);
        float3 cell = fmod(i + o + freq, freq);
        float  d    = dot(gradient(cell), f - o);
        float  w    = lerp(1.0 - u.x, u.x, o.x)
                    * lerp(1.0 - u.y, u.y, o.y)
                    * lerp(1.0 - u.z, u.z, o.z);
        n += d * w;
    }
    return n;
}

float perlinFbm(float3 uvw, float freq, int octaves)
{
    float sum = 0.0, amp = 1.0, norm = 0.0;
    for (int i = 0; i < octaves; ++i)
    {
        sum  += perlin(uvw, freq) * amp;
        norm += amp;
        freq *= 2.0;      // stays integral, so every octave still tiles
        amp  *= 0.5;
    }
    return sum / norm;
}

[numthreads(4, 4, 4)]
void CSGenBase(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid >= uint3(BASE_RES, BASE_RES, BASE_RES))) return;

    float3 uvw = (float3(tid) + 0.5) / BASE_RES;

    float pfbm = saturate(perlinFbm(uvw, 4.0, 7) * 1.5 + 0.5);
    float wfbm = worleyFbm(uvw, 4.0);

    // Dilate the Perlin field by the Worley field. This is what turns smooth
    // billows into the puffy, self-similar structure clouds actually have.
    float perlinWorley = saturate(remap(pfbm, 0.0, 1.0, wfbm, 1.0));

    gBaseOut[tid] = float4(perlinWorley,
                           worleyFbm(uvw,  4.0),
                           worleyFbm(uvw,  8.0),
                           worleyFbm(uvw, 16.0));
}

[numthreads(4, 4, 4)]
void CSGenDetail(uint3 tid : SV_DispatchThreadID)
{
    if (any(tid >= uint3(DETAIL_RES, DETAIL_RES, DETAIL_RES))) return;

    float3 uvw = (float3(tid) + 0.5) / DETAIL_RES;

    gDetailOut[tid] = float4(worleyFbm(uvw, 2.0),
                             worleyFbm(uvw, 4.0),
                             worleyFbm(uvw, 8.0),
                             1.0);
}
