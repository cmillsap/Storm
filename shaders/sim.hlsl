// Storm - the cloud simulation passes.
//
// One step is: advect, force, condense, project. Ported from the solvers
// validated in Spikes 03 and 04, which established that this is enough to be
// directed - buoyancy goes where it is told, a stability layer sets the cloud
// top, and every seed produces a storm.

#include "sim.hlsli"

// Ping-pong. gSimPhase names the set holding the current data; advection reads
// it through an SRV and writes the other set through a UAV, and every pass
// after that works in place on the destination.
//
// Nothing here reads the source through a UAV. A resource cannot be in both
// UNORDERED_ACCESS and NON_PIXEL_SHADER_RESOURCE at once, and advection needs
// the source filterable - so the source is read only by the sampling helpers in
// sim.hlsli, which land exactly on texel centres when asked for a face position
// and therefore return the stored value unchanged.
void writeU(int3 c, float value)  { if (gSimPhase == 0) gSimU1[c] = value; else gSimU0[c] = value; }
void writeV(int3 c, float value)  { if (gSimPhase == 0) gSimV1[c] = value; else gSimV0[c] = value; }
void writeW(int3 c, float value)  { if (gSimPhase == 0) gSimW1[c] = value; else gSimW0[c] = value; }
void writeS(int3 c, float4 value) { if (gSimPhase == 0) gSimS1[c] = value; else gSimS0[c] = value; }

float  readUDst(int3 c) { return (gSimPhase == 0) ? gSimU1[c] : gSimU0[c]; }
float  readVDst(int3 c) { return (gSimPhase == 0) ? gSimV1[c] : gSimV0[c]; }
float  readWDst(int3 c) { return (gSimPhase == 0) ? gSimW1[c] : gSimW0[c]; }
float4 readSDst(int3 c) { return (gSimPhase == 0) ? gSimS1[c] : gSimS0[c]; }

int3 wrapCell(int3 c)
{
    c.x = (c.x % gSimRes.x + gSimRes.x) % gSimRes.x;
    c.z = (c.z % gSimRes.z + gSimRes.z) % gSimRes.z;
    c.y = clamp(c.y, 0, gSimRes.y - 1);
    return c;
}

// ---- advection ------------------------------------------------------------
//
// Semi-Lagrangian: trace backwards from each sample point and read what was
// there. Unconditionally stable, which is what lets the step size be chosen for
// how fast the storm should evolve rather than for the CFL condition.

[numthreads(8, 8, 1)]
void CSAdvect(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.z >= gSimRes.z) return;

    if (c.y < gSimRes.y)
    {
        // u face: (i, j+0.5, k+0.5)
        float3 pu = gSimOrigin + float3(c.x, c.y + 0.5, c.z + 0.5) * gSimCell;
        float3 vu = float3(sampleU(pu), sampleV(pu), sampleW(pu));
        writeU(c, sampleU(pu - vu * gSimDt));

        // w face: (i+0.5, j+0.5, k)
        float3 pw = gSimOrigin + float3(c.x + 0.5, c.y + 0.5, c.z) * gSimCell;
        float3 vw = float3(sampleU(pw), sampleV(pw), sampleW(pw));
        writeW(c, sampleW(pw - vw * gSimDt));

        // cell centre scalars
        float3 pc = gSimOrigin + (float3(c) + 0.5) * gSimCell;
        float3 vc = float3(sampleU(pc), sampleV(pc), sampleW(pc));
        writeS(c, sampleScalars(pc - vc * gSimDt));
    }

    // v face: (i+0.5, j, k+0.5). Rigid ground and lid.
    if (c.y == 0 || c.y == gSimRes.y)
    {
        writeV(c, 0.0);
    }
    else if (c.y < gSimRes.y)
    {
        float3 pv = gSimOrigin + float3(c.x + 0.5, c.y, c.z + 0.5) * gSimCell;
        float3 vv = float3(sampleU(pv), sampleV(pv), sampleW(pv));
        writeV(c, sampleV(pv - vv * gSimDt));
    }
}

// ---- forcing and buoyancy -------------------------------------------------

[numthreads(8, 8, 1)]
void CSForces(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.y >= gSimRes.y || c.z >= gSimRes.z) return;

    float3 p = gSimOrigin + (float3(c) + 0.5) * gSimCell;
    float4 s = readSDst(c);

    // Directed forcing: a sustained warm, moist source where the tower is
    // supposed to be. The simulation supplies motion and texture; it never
    // decides what happens.
    if (gForceRamp > 0.0)
    {
        float3 d = p - gForceCentre;
        float r2 = dot(d.xz, d.xz) / (gForceRadius * gForceRadius)
                 + (d.y * d.y) / (gForceDepth * gForceDepth);
        if (r2 < 9.0)
        {
            // Low-frequency noise across the patch, so the source is not
            // axisymmetric. A perfectly round source produces a perfectly
            // round mushroom; a real cumulus is several turrets sharing one
            // base, and the difference between the two is entirely in how
            // uneven the ground under them is.
            float n = gBaseNoise.SampleLevel(gWrap, p * 0.00006, 0).r;
            float weight = exp(-r2) * (0.55 + 0.90 * n) * gForceRamp * gSimDt;
            s.r += gForceHeat * weight;
            s.g += gForceMoisture * weight;
        }
    }
    writeS(c, s);
}

[numthreads(8, 8, 1)]
void CSBuoyancy(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.z >= gSimRes.z || c.y > gSimRes.y) return;
    if (c.y == 0 || c.y == gSimRes.y) { writeV(c, 0.0); return; }

    // Buoyancy lives on the v faces, averaged from the cells either side.
    // Virtual-temperature effect from vapour and condensate loading from cloud
    // water, which is what lets a downdraft form under the anvil.
    float yBelow = gSimOrigin.y + (c.y - 0.5) * gSimCell;
    float yAbove = gSimOrigin.y + (c.y + 0.5) * gSimCell;

    float4 below = readSDst(int3(c.x, c.y - 1, c.z));
    float4 above = readSDst(c);

    float bBelow = kGravity * ((below.r - thetaEnv(yBelow)) / kTheta0
                             + 0.61 * (below.g - vapourEnv(yBelow)) - below.b);
    float bAbove = kGravity * ((above.r - thetaEnv(yAbove)) / kTheta0
                             + 0.61 * (above.g - vapourEnv(yAbove)) - above.b);

    writeV(c, readVDst(c) + 0.5 * (bBelow + bAbove) * gSimDt);
}

// ---- microphysics ---------------------------------------------------------
//
// Saturation adjustment: condense whatever exceeds saturation, release the
// latent heat into the potential temperature, and evaporate back when a parcel
// becomes subsaturated. This feedback is what makes a cumulus accelerate rather
// than just coast upward.

[numthreads(8, 8, 1)]
void CSMicrophysics(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.y >= gSimRes.y || c.z >= gSimRes.z) return;

    float y = gSimOrigin.y + (c.y + 0.5) * gSimCell;
    float qs = satVapour(y);

    float4 s = readSDst(c);
    float excess = s.g - qs;
    if (excess > 0.0)
    {
        s.g -= excess;
        s.b += excess;
        s.r += kLatentOverCp * excess;
    }
    else if (s.b > 0.0)
    {
        float dq = min(s.b, -excess);
        s.g += dq;
        s.b -= dq;
        s.r -= kLatentOverCp * dq;
    }

    // Precipitation fallout. Without it condensate accumulates and the
    // downdraft never develops.
    s.b -= s.b * 0.00035 * gSimDt;

    // Sponge under the lid, so gravity waves do not reflect back down through
    // the cloud top.
    float spongeStart = gSimOrigin.y + float(gSimRes.y) * gSimCell - 1200.0;
    if (y > spongeStart)
    {
        float k = saturate((y - spongeStart) / 1200.0);
        float damp = max(0.0, 1.0 - k * k * 3.0 * gSimDt);
        float te = thetaEnv(y);
        s.r = te + (s.r - te) * damp;
    }

    writeS(c, s);
}

// ---- pressure projection --------------------------------------------------

[numthreads(8, 8, 1)]
void CSDivergence(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.y >= gSimRes.y || c.z >= gSimRes.z) return;

    int3 xp = int3((c.x + 1) % gSimRes.x, c.y, c.z);
    int3 zp = int3(c.x, c.y, (c.z + 1) % gSimRes.z);

    float d = (readUDst(xp) - readUDst(c)) / gSimCell
            + (readVDst(int3(c.x, c.y + 1, c.z)) - readVDst(c)) / gSimCell
            + (readWDst(zp) - readWDst(c)) / gSimCell;

    gSimDiv[c] = d;
    gSimP0[c] = 0.0;
    gSimP1[c] = 0.0;
}

[numthreads(8, 8, 1)]
void CSJacobi(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.y >= gSimRes.y || c.z >= gSimRes.z) return;

    int3 xm = wrapCell(int3(c.x - 1, c.y, c.z));
    int3 xp = wrapCell(int3(c.x + 1, c.y, c.z));
    int3 zm = wrapCell(int3(c.x, c.y, c.z - 1));
    int3 zp = wrapCell(int3(c.x, c.y, c.z + 1));

    // Neumann at the ground and lid: the ghost value mirrors the interior, so
    // the pressure gradient across the boundary is zero and the rigid surfaces
    // do no work.
    int3 ym = int3(c.x, max(c.y - 1, 0), c.z);
    int3 yp = int3(c.x, min(c.y + 1, gSimRes.y - 1), c.z);

    float sum;
    if (gJacobiPhase == 0)
        sum = gSimP0[xm] + gSimP0[xp] + gSimP0[ym] + gSimP0[yp] + gSimP0[zm] + gSimP0[zp];
    else
        sum = gSimP1[xm] + gSimP1[xp] + gSimP1[ym] + gSimP1[yp] + gSimP1[zm] + gSimP1[zp];

    float value = (sum - gSimCell * gSimCell * gSimDiv[c]) / 6.0;

    if (gJacobiPhase == 0) gSimP1[c] = value;
    else                   gSimP0[c] = value;
}

[numthreads(8, 8, 1)]
void CSProject(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.z >= gSimRes.z || c.y > gSimRes.y) return;

    // gJacobiPhase names the buffer the last iteration wrote.
    if (c.y < gSimRes.y)
    {
        int3 xm = wrapCell(int3(c.x - 1, c.y, c.z));
        int3 zm = wrapCell(int3(c.x, c.y, c.z - 1));

        float here = (gJacobiPhase == 0) ? gSimP0[c]  : gSimP1[c];
        float atXm = (gJacobiPhase == 0) ? gSimP0[xm] : gSimP1[xm];
        float atZm = (gJacobiPhase == 0) ? gSimP0[zm] : gSimP1[zm];

        writeU(c, readUDst(c) - (here - atXm) / gSimCell);
        writeW(c, readWDst(c) - (here - atZm) / gSimCell);
    }

    if (c.y == 0 || c.y == gSimRes.y)
    {
        writeV(c, 0.0);
    }
    else
    {
        int3 ym = int3(c.x, c.y - 1, c.z);
        float here  = (gJacobiPhase == 0) ? gSimP0[c]  : gSimP1[c];
        float below = (gJacobiPhase == 0) ? gSimP0[ym] : gSimP1[ym];
        writeV(c, readVDst(c) - (here - below) / gSimCell);
    }
}

// ---- initialisation -------------------------------------------------------

[numthreads(8, 8, 1)]
void CSInitialise(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.z >= gSimRes.z) return;

    if (c.y <= gSimRes.y)
    {
        gSimU0[c] = 0.0; gSimU1[c] = 0.0;
        gSimV0[c] = 0.0; gSimV1[c] = 0.0;
        gSimW0[c] = 0.0; gSimW1[c] = 0.0;
    }
    if (c.y >= gSimRes.y) return;

    float3 p = gSimOrigin + (float3(c) + 0.5) * gSimCell;

    // theta holds TOTAL potential temperature as a departure from 300 K, not a
    // perturbation from the environment. Advection has to carry the total: a
    // parcel lifted into warmer surroundings must become negatively buoyant on
    // its own, which only happens if it carries its absolute theta with it.
    float3 d = p - float3(gForceCentre.x, gSimOrigin.y + gMixedTop * 0.5, gForceCentre.z);
    float r2 = dot(d.xz, d.xz) / (gForceRadius * gForceRadius)
             + (d.y * d.y) / (gForceDepth * gForceDepth);
    float bubble = 2.0 * exp(-r2);
    float noise = (hash12(float2(c.x + c.z * 131, c.y * 17)) - 0.5) * 0.05;

    float4 s = float4(thetaEnv(p.y) + bubble + noise, vapourEnv(p.y), 0.0, 0.0);
    gSimS0[c] = s;
    gSimS1[c] = s;
    gSimP0[c] = 0.0;
    gSimP1[c] = 0.0;
    gSimDiv[c] = 0.0;
}
