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

        // cell centre scalars. Rain is advected by the same wind as everything
        // else plus its own fall speed, which is why it is traced separately:
        // done here it reads the source through the SRVs and cannot race, and
        // doing it in the microphysics pass instead would have each cell
        // reading a neighbour another thread was in the middle of writing.
        float3 pc = gSimOrigin + (float3(c) + 0.5) * gSimCell;
        float3 vc = float3(sampleU(pc), sampleV(pc), sampleW(pc));
        float4 carried = sampleScalars(pc - vc * gSimDt);
        float3 vr = vc - float3(0.0, gRainFallSpeed, 0.0);
        carried.a = sampleScalars(pc - vr * gSimDt).a;
        writeS(c, carried);
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

    // Loading counts rain as well as cloud water. It is the heavier of the two
    // where it matters - under the base, where the rain shaft is and the cloud
    // is not - and it is half of what drives the downdraft. The other half is
    // the evaporative cooling in the microphysics pass.
    float bBelow = kGravity * ((below.r - thetaEnv(yBelow)) / kTheta0
                             + 0.61 * (below.g - vapourEnv(yBelow)) - below.b - below.a);
    float bAbove = kGravity * ((above.r - thetaEnv(yAbove)) / kTheta0
                             + 0.61 * (above.g - vapourEnv(yAbove)) - above.b - above.a);

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

    float ice = iceFraction(y);

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
        // Evaporation is instantaneous for water and very nearly refused for
        // ice. This one line is the anvil. Under a pure saturation adjustment
        // everything the tower detrains meets 30% humidity and is gone in a
        // step, and the /arc harness measured exactly that: an 11 km tower
        // with a cloud radius that never left the core.
        float rate = lerp(1.0, gIceEvaporation, ice);
        float dq = min(s.b, -excess) * rate;
        s.g += dq;
        s.b -= dq;
        s.r -= kLatentOverCp * dq;
    }

    // Condensate becomes rain: autoconversion once there is enough of it to
    // collide, then accretion, which is what makes the shaft run away once it
    // has started. Both are switched off in ice - an anvil that rained itself
    // out would not be an anvil - and what is left of the old blanket fallout
    // stands in for the slow loss of ice crystals.
    float warm = warmFraction(y);
    float rainMade = (max(s.b - gAutoThreshold, 0.0) * gAutoRate
                   + s.b * s.a * gAccretionRate) * warm * gSimDt;
    rainMade = min(rainMade, s.b);
    s.b -= rainMade;
    s.a += rainMade;
    s.b -= s.b * lerp(gFallout, gIceFallout, ice) * gSimDt;

    // Rain falling through unsaturated air evaporates into it and cools it.
    // This is the downdraft. The latent heat comes back out of the potential
    // temperature, the parcel becomes negatively buoyant, and it descends -
    // which is the whole mechanism a storm uses to spread cold air under its
    // own base.
    if (s.a > 0.0)
    {
        // Proportional to how much rain there is, not just to how dry the air
        // is. Written the other way the rate is the same for a downpour and
        // for the last drop of it, and the first version of this emptied a
        // shaft of 0.002 in a single second of storm time - the rain never got
        // below the cloud base at all, and the shaft the phase is supposed to
        // ship was a stub two cells deep.
        float deficit = max(qs - s.g, 0.0);
        float evaporated = min(s.a, gRainEvaporation * deficit * s.a * gSimDt);
        s.a -= evaporated;
        s.g += evaporated;
        s.r -= kLatentOverCp * evaporated;
    }

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
        // The environment starts as the shear profile, in the storm-relative
        // frame. A horizontally uniform wind that varies only with height is
        // already divergence free, so the projection leaves it alone and the
        // domain begins in balance rather than ringing for the first minute.
        float yU = gSimOrigin.y + (min(c.y, gSimRes.y - 1) + 0.5) * gSimCell;
        float2 wind = windEnv(yU);
        gSimU0[c] = wind.x; gSimU1[c] = wind.x;
        gSimV0[c] = 0.0;    gSimV1[c] = 0.0;
        gSimW0[c] = wind.y; gSimW1[c] = wind.y;
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

// ---- the lateral margin ---------------------------------------------------
//
// A relaxation zone around the sides of the domain. Everything - the three
// velocity components and all four scalars - is pulled back toward the
// environment, so the storm's outflow is absorbed rather than carried out of
// one periodic face and back in through the other.
//
// It has to damp the wind as well as what the wind carries. An earlier version
// relaxed only the scalars, and the outflow simply refilled the margin from
// inside on the next step.

[numthreads(8, 8, 1)]
void CSDamp(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.z >= gSimRes.z || c.y > gSimRes.y) return;

    float3 p = gSimOrigin + (float3(c) + 0.5) * gSimCell;
    float  k = lateralMargin(p);

    // Quadratic, so the margin starts imperceptibly and only bites near the
    // face. A linear ramp puts a visible edge where the zone begins.
    float a = saturate(k * k * gLateralRate * gSimDt);

    float2 wind = windEnv(p.y);

    // The wind is relaxed everywhere, not only in the margin: the sides are
    // periodic and nothing else maintains the shear, so a storm left alone
    // mixes its own environment flat within a few hundred seconds and takes
    // the tilt it was given with it.
    //
    // Everywhere except inside the cloud. The relaxation cannot tell the
    // environment it is maintaining from the storm's own outflow, and at the
    // equilibrium level the outflow is precisely what an anvil is made of -
    // held against a uniform profile with a 600 second e-folding, more than
    // half of it was gone before it had spread a kilometre. Clear air is held;
    // cloud is left to do what it likes.
    float cloudy = (c.y < gSimRes.y) ? saturate(readSDst(c).b * 1.0e4) : 0.0;
    float aWind = saturate(a + gWindRelaxation * gSimDt * (1.0 - cloudy));
    if (c.y < gSimRes.y)
    {
        writeU(c, lerp(readUDst(c), wind.x, aWind));
        writeW(c, lerp(readWDst(c), wind.y, aWind));

        float4 s = readSDst(c);

        // Move the environment with the lid. Every cell's potential
        // temperature is shifted by exactly the change in the environmental
        // profile, so a parcel's departure from its surroundings - which is
        // all buoyancy depends on - comes through untouched. Relaxing toward
        // the new profile instead leaves a lag proportional to how fast the
        // lid is moving, and the lid has to cross seven kilometres inside one
        // act. This is exact and costs an add.
        s.r += capInversion(p.y, gCapHeight,     gCapStrength)
             - capInversion(p.y, gCapHeightPrev, gCapStrengthPrev);

        // Temperature and moisture are also relaxed toward the environment in
        // clear air, for the same reason the wind is: nothing else in a
        // periodic domain holds the sounding the storm is growing in. Cloud is
        // exempt, or the storm would be nudged out of existence.
        float aEnv = saturate(a + gEnvRelaxation * gSimDt * (1.0 - cloudy));
        s.r = lerp(s.r, thetaEnv(p.y),  aEnv);
        s.g = lerp(s.g, vapourEnv(p.y), aEnv);
        s.b = lerp(s.b, 0.0, a);
        s.a = lerp(s.a, 0.0, a);
        writeS(c, s);
    }
    if (c.y > 0 && c.y < gSimRes.y) writeV(c, lerp(readVDst(c), 0.0, a));
}

// ---- diagnostics ----------------------------------------------------------
//
// A reduction into eight counters, dispatched only by the /arc harness. Fixed
// point through InterlockedMax/Min/Add, because there is no float atomic in
// cs_6_0 and the precision these need is nowhere near a float's.
//
// The encodings are all recovered on the host, in Simulation::fetchStats.

static const float kCloudyQc = 1.0e-4;   // 0.1 g/kg - the conventional cloud boundary

// Alongside the eight scalars, a vertical profile in 32 bands: the peak and the
// total condensate in each. The renderer maps condensate to opacity through one
// reference value, and that mapping cannot be chosen without knowing how
// condensate actually varies from the cloud base to the anvil - which is three
// orders of magnitude of saturation vapour pressure, not a detail.
static const uint kStatsBands = 32;
static const uint kStatsPeak  = 8;                   // 8  .. 39  peak condensate
static const uint kStatsCells = 8 + kStatsBands;     // 40 .. 71  cloudy cells, ie. area

uint bandOf(int y) { return (uint)clamp(y * (int)kStatsBands / gSimRes.y, 0, (int)kStatsBands - 1); }

[numthreads(64, 1, 1)]
void CSStatsClear(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < 2 * kStatsBands) { gSimStats[kStatsPeak + tid.x] = 0; }
    if (tid.x > 0) return;

    gSimStats[0] = 0;            // cloud top    - max
    gSimStats[1] = 0xFFFFFFFFu;  // cloud base   - min
    gSimStats[2] = 0;            // updraft      - max
    gSimStats[3] = 0xFFFFFFFFu;  // downdraft    - min
    gSimStats[4] = 0;            // condensate mass
    gSimStats[5] = 0;            // rain mass
    gSimStats[6] = 0;            // cloud radius - max
    gSimStats[7] = 0;            // cloudy cells
}

[numthreads(8, 8, 1)]
void CSStats(uint3 tid : SV_DispatchThreadID)
{
    int3 c = int3(tid);
    if (c.x >= gSimRes.x || c.y >= gSimRes.y || c.z >= gSimRes.z) return;

    float4 s = readSCurrent(c);

    // Vertical velocity at the cell centre, from the two faces.
    float w = 0.5 * (readVCurrent(c) + readVCurrent(int3(c.x, c.y + 1, c.z)));
    uint wFixed = (uint)clamp((w + 100.0) * 100.0, 0.0, 40000.0);
    InterlockedMax(gSimStats[2], wFixed);
    InterlockedMin(gSimStats[3], wFixed);

    if (s.b > kCloudyQc)
    {
        // Height as a cell index plus one, so zero can mean "no cloud at all".
        InterlockedMax(gSimStats[0], (uint)(c.y + 1));
        InterlockedMin(gSimStats[1], (uint)(c.y + 1));

        float3 p = gSimOrigin + (float3(c) + 0.5) * gSimCell;
        float2 d = p.xz - gForceCentre.xz;
        InterlockedMax(gSimStats[6], (uint)length(d));

        InterlockedAdd(gSimStats[7], 1u);
    }

    // Mass sums run over every cell, not just the cloudy ones, so they stay a
    // conserved quantity rather than a function of the threshold.
    InterlockedAdd(gSimStats[4], (uint)(max(s.b, 0.0) * 1.0e5));
    InterlockedAdd(gSimStats[5], (uint)(max(s.a, 0.0) * 1.0e5));

    // Cloudy cells per band is the anvil measure: how wide the cloud is at
    // each height, which peak condensate cannot tell you.
    uint band = bandOf(c.y);
    InterlockedMax(gSimStats[kStatsPeak + band], (uint)(max(s.b, 0.0) * 1.0e6));
    if (s.b > kCloudyQc) InterlockedAdd(gSimStats[kStatsCells + band], 1u);
}
