// Storm - cloud density and lighting.
//
// Phase 02 replaces the analytic container from Spike 02 with the simulation's
// condensate field. The lighting constants are unchanged from the values
// arrived at in Spike 02; they are collected at the top because they are what
// gets tuned, and the shaders compile at startup so changing one and re-running
// is the whole iteration loop.

#ifndef STORM_CLOUDS_HLSLI
#define STORM_CLOUDS_HLSLI

#include "common.hlsli"
#include "sim.hlsli"

// The light volume resolution arrives in the frame constants rather than as
// a literal here. Held in two places it silently drifted apart: the host
// dispatched 96 while this said 64, so two thirds of the volume was never
// written and the cloud top sampled uninitialised memory as full shadow.

static const float BASE_SCALE     = 0.000210;  // world metres -> base volume uv
static const float DETAIL_SCALE   = 0.001450;  // world metres -> detail volume uv
static const float kExtinction    = 0.055;     // per metre, at density 1
static const float kDetailErosion = 0.72;
static const float kShapeErosion  = 0.32;      // larger-scale erosion of the sim boundary
static const float kAnvilErosion  = 0.16;      // how much of that survives in ice
static const float kRainErosion   = 0.30;      // and in a rain shaft
static const float kRainAlbedo    = 0.55;      // rain scatters less than cloud does
// And rain extinguishes far less than cloud does, per unit of water. Extinction
// goes as the total cross-section, which for a fixed mass goes as 1/radius: a
// millimetre raindrop is a hundred times a cloud droplet, so the same water as
// rain blocks a small fraction of what it blocks as cloud. Treating the two
// alike is why the storm's lower half was an opaque wall for kilometres in
// every direction, with the tornado somewhere inside it.
static const float kRainExtinction = 0.30;
// Mammatus. The underside of an anvil is not flat: it hangs in pouches, and
// they are the one cloud feature that reads instantly as "storm". Rather than
// finding the underside and displacing a surface that does not exist here, the
// sample position itself is pushed up and down by a low-frequency field inside
// the ice. A boundary sampled through a displaced coordinate is a displaced
// boundary, and it costs nothing - the lookup was happening anyway.
static const float kMammatusScale = 0.00026;
static const float kMammatusDepth = 300.0;
static const float kPowder        = 2.2;
static const float kPowderFloor   = 0.35;      // how lit the thinnest material stays
static const float kAmbientLow    = 0.20;      // skylight reaching the cloud base
static const float kAmbientScale  = 0.040;
static const float kSunGain       = 2.4;
static const float kSilverLobe    = 0.78;      // forward Henyey-Greenstein lobe
static const float kBackLobe      = -0.28;
static const float kLobeMix       = 0.32;
// Lightning. Slightly blue-white, and attenuated hard with distance: a real
// flash lights a limited volume because the cloud it is inside is optically
// thick, and an inverse-square falloff alone lights the whole storm evenly and
// reads as the sun coming on.
static const float3 kFlashColour  = float3(0.72, 0.80, 1.00);
static const float  kFlashReach   = 1400.0;   // metres, e-folding
static const float  kFlashGain    = 5.0;

// ---- the tornado ----------------------------------------------------------
//
// Analytic, and unapologetically so. At 90 m cells a funnel is four cells
// across, and Spike 04 measured what the solver does with imposed swirl at
// that scale: it produces a rotating updraft, and none of the asymmetric
// structure. So the funnel is authored - a radius profile about the
// mesocyclone axis, banded by noise that rotates with height, with a debris
// cloud where it meets the ground.
//
// It goes into sampleDensity rather than being composited afterwards, which
// costs nothing and buys two things: the light volume sees it, so it is
// shadowed by the storm above it the way it should be, and it casts its own
// shadow across the ground.

static const float kDebrisAlbedo = 0.42;   // dust is darker than cloud, and warmer

float tornadoDensity(float3 p, out float debrisShare)
{
    debrisShare = 0.0;
    if (gTornadoIntensity <= 0.001) return 0.0;

    float ground = gTornadoAxis.y;
    if (p.y > gTornadoTop + 120.0 || p.y < ground) return 0.0;

    // The axis leans with the storm, and wanders. A funnel that is a straight
    // cone reads as a cone; the wander is small - tens of metres - and it is
    // most of what makes it read as a funnel instead.
    float heightAbove = p.y - ground;
    float2 axis = gTornadoAxis.xz + float2(gTornadoTilt * heightAbove, 0.0);

    // Reject on the unwandered axis first, with the wander added to the bound.
    // Everything below cloud base in the whole domain reaches this function,
    // and the light volume calls it 128-cubed times as well: a texture fetch
    // before this test is a texture fetch nearly all of them pay for.
    const float kWander = 260.0;
    float reach = max(gDebrisRadius, gWallRadius) * 2.2 + kWander;
    if (dot(p.xz - axis, p.xz - axis) > reach * reach) return 0.0;

    float3 wanderAt = float3(heightAbove * 0.0011, gTornadoSwirl * 0.013, 0.31);
    float2 wander = (gBaseNoise.SampleLevel(gWrap, wanderAt, 0).rg - 0.5) * kWander;
    axis += wander * saturate(heightAbove / 400.0);

    float2 d = p.xz - axis;
    float  r = length(d);
    if (r > max(gDebrisRadius, gWallRadius) * 2.2) return 0.0;

    // Banding that turns with height and with time. Differential - the top
    // turns more slowly than the tip - and sampled at two scales, because one
    // octave at this size is a smooth cone with a wobbly outline and nothing
    // on its surface.
    float angle = atan2(d.y, d.x);
    float turn  = angle + gTornadoSwirl * (1.4 - 0.55 * saturate(p.y / max(gTornadoTop, 1.0)));
    float3 bandA = float3(cos(turn), sin(turn), heightAbove * 0.0016) * 3.1;
    float3 bandB = float3(cos(turn * 2.0), sin(turn * 2.0), heightAbove * 0.0052) * 5.7;
    float  nA = gDetailNoise.SampleLevel(gWrap, bandA, 0).r;
    float  nB = gDetailNoise.SampleLevel(gWrap, bandB, 0).g;
    float  n  = nA * 0.68 + nB * 0.32;

    float density = 0.0;

    // The condensation funnel, from the cloud base down to wherever the tip
    // has got to.
    float tip = lerp(gTornadoTop, ground, gTornadoDescent);
    if (p.y >= tip)
    {
        float h = saturate((p.y - tip) / max(gTornadoTop - tip, 1.0));
        // A trumpet: narrow at the tip, flaring into the wall cloud above.
        float radius = gTornadoRadius * (0.22 + 0.78 * pow(h, 1.35));
        radius *= 0.74 + 0.52 * n;
        // A hard core with a ragged edge, rather than one long gradient. The
        // gradient is what made the first funnel look moulded.
        density = 1.0 - smoothstep(radius * 0.72, radius, r);
        density *= 0.72 + 0.5 * nB;
        density *= 1.0 - smoothstep(0.88, 1.0, h);
    }

    // The wall cloud: a lowered collar hanging below the base around the
    // mesocyclone, which is what a funnel comes out of. Without it the funnel
    // emerges from a flat cloud base, and a flat base is the one thing a
    // supercell's updraft does not have.
    if (gWallRadius > 1.0 && p.y < gTornadoTop + 90.0 && p.y > gTornadoTop - gWallDrop)
    {
        float t  = saturate((gTornadoTop - p.y) / max(gWallDrop, 1.0));
        // Striated: the collar is cut into by the same rotation, so its edge
        // is a set of curved steps rather than a rim.
        float striate = gDetailNoise.SampleLevel(gWrap,
                          float3(cos(turn * 1.3), sin(turn * 1.3), t * 2.4) * 2.6, 0).b;
        float wr = gWallRadius * (1.0 - 0.62 * t) * (0.70 + 0.60 * striate);
        float wall = (1.0 - smoothstep(wr * 0.62, wr, r)) * (1.0 - t * t * 0.3);
        density = max(density, wall * 0.92);
    }

    // The debris cloud. Wider, rougher, and only once the tip is near enough
    // to the ground to be lifting anything.
    if (gDebrisHeight > 1.0 && p.y < gDebrisHeight * 2.0)
    {
        float dh = saturate(1.0 - heightAbove / (gDebrisHeight * 2.0));
        float dr = gDebrisRadius * (0.30 + 0.85 * dh) * (0.62 + 0.76 * n);
        float debris = dh * dh * (1.0 - smoothstep(dr * 0.45, dr, r));
        debris *= saturate(gTornadoDescent * 4.0 - 3.0);
        if (debris > 0.0)
        {
            float share = saturate(debris / max(debris + density, 1e-4));
            debrisShare = max(debrisShare, share);
            density = max(density, debris);
        }
    }

    return saturate(density) * gTornadoIntensity;
}

// The rear-flank downdraft's clear slot, and the vault under the updraft.
// Returns how much of the precipitation at this point is cleared away.
//
// Authored, and the plan always said it would have to be: Spike 04 measured
// imposed swirl producing a rotating updraft and none of the asymmetric
// structure that goes with a real one - no hook, no clear slot, no displaced
// shaft. The storm makes the rain; this decides where it is not.
float clearSlot(float3 p)
{
    if (gSlotStrength <= 0.001) return 0.0;

    float2 axis = gTornadoAxis.xz + float2(gTornadoTilt * (p.y - gTornadoAxis.y), 0.0);
    float2 d = p.xz - axis;
    float  r = length(d);
    if (r > gSlotRadius || p.y > gSlotTop) return 0.0;

    // The vault: rain-free right under the mesocyclone, whatever the azimuth.
    float vault = 1.0 - smoothstep(gWallRadius * 0.8, gWallRadius * 2.1, r);

    // And the slot itself, a wedge cut back into the precipitation from one
    // side, curving with radius the way a rear-flank downdraft wraps.
    float azimuth = atan2(d.y, d.x) - gSlotAzimuth - r * 0.00011;
    azimuth = atan2(sin(azimuth), cos(azimuth));          // wrap to -pi..pi
    float wedge = 1.0 - smoothstep(gSlotWidth * 0.55, gSlotWidth, abs(azimuth));
    wedge *= smoothstep(gWallRadius * 0.5, gWallRadius * 1.4, r)
           * (1.0 - smoothstep(gSlotRadius * 0.65, gSlotRadius, r));

    float vertical = 1.0 - smoothstep(gSlotTop * 0.55, gSlotTop, p.y);
    return gSlotStrength * saturate(max(vault, wedge)) * vertical;
}

float normalisedHeight(float3 p)
{
    return saturate((p.y - gCloudBottom) / (gCloudTop - gCloudBottom));
}

// ---- bounds ---------------------------------------------------------------
//
// The cloud volume is the simulation domain. Rays that miss it never sample.

float3 cloudBoxMin() { return gSimOrigin; }
float3 cloudBoxMax() { return gSimOrigin + float3(gSimRes) * gSimCell; }

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

// ---- density --------------------------------------------------------------
//
// The plan's central bet, made concrete: the solver supplies the low-frequency
// shape and the noise supplies texture. At 50 m cells the condensate field is
// far coarser than the features the eye reads as cauliflower, and numerical
// diffusion smooths its boundary further, so the same erosion that carved the
// analytic container in Spike 02 now carves this.

// Density, and how much of it is rain rather than cloud. The two are returned
// together because every caller that shades needs to know the difference: a
// rain shaft is the same medium geometrically and a much darker one optically.
float sampleDensityAndRain(float3 p, bool detail, out float rainShare, out float debrisShare)
{
    rainShare = 0.0;

    // The funnel first, and outside the early-out below: it exists in air the
    // solver has no condensate in at all, which is the point of it being
    // analytic.
    float funnelDebris;
    float funnel = tornadoDensity(p, funnelDebris);
    debrisShare = funnelDebris;

    // Mammatus, applied to the position rather than to the density. Only in
    // the ice, which is the only place an anvil underside exists.
    float3 sampleAt = p;
    float  ice = iceFraction(p.y);
    if (ice > 0.0)
    {
        float lobes = gBaseNoise.SampleLevel(gWrap, p * kMammatusScale, 0).b;
        sampleAt.y += (lobes - 0.5) * kMammatusDepth * ice;
    }

    float4 s = sampleScalars(sampleAt);
    float qc = s.b;
    float qr = s.a;
    if (qc <= 0.0 && qr <= 0.0)
        return (funnel > 0.0) ? funnel * gDensityScale : 0.0;

    // What counts as opaque is local, and this is the part of Phase 03 that
    // took longest to get right. Phase 02 normalised against one constant,
    // which works while the cloud is a 6.4 km cumulus whose core is the only
    // thing in the box. Over a 14 km storm it fails twice over: the /arc
    // profile measures peak condensate climbing from 0.0019 at the base to
    // 0.0117 in the anvil, and at any one height the periphery carries a tenth
    // of what the core does. Normalise against a constant, or against the
    // adiabatic curve, and the erosion below - which is a threshold - does not
    // carve the anvil, it deletes every sample of it.
    //
    // So the reference is the cloud's own peak in this neighbourhood, with a
    // floor. Every part of the cloud then hands the erosion a field that
    // reaches one in the interior and falls to zero at the edge, and the floor
    // is what keeps a stray wisp from being promoted to solid cloud on the
    // strength of being the densest thing near it.
    float3 uvw = (p - cloudBoxMin()) / (cloudBoxMax() - cloudBoxMin());
    float  localPeak = gCloudMax.SampleLevel(gClamp, uvw, 0);
    float  reference = max(gQcRef * localPeak, gQcFloor);
    float  cloud = saturate(qc / reference);

    // Rain is measured absolutely rather than against the neighbourhood: a
    // shaft is a shaft whether or not there is anything dense beside it, and
    // the whole point of it is that it is thinner than the cloud it falls out
    // of.
    //
    // And then the clear slot takes it away again where the storm's structure
    // says there should be none. Only the rain: the wall cloud lives in the
    // same place and has to survive it.
    float rain = saturate(qr / gRainOpaque) * (1.0 - clearSlot(p));

    float d = max(cloud, rain * kRainExtinction);

    // Every early-out from here on has to let the funnel past, and this one
    // caught me out: the clear slot removes the rain precisely where the
    // funnel hangs, so under the mesocyclone there is often no cloud and no
    // rain at all - and the function returned zero before the funnel was ever
    // folded in. The tornado rendered as a two-hundred-metre stub of wall
    // cloud with nothing below it, in the one place it was guaranteed to be
    // invisible.
    if (d <= 0.0) return (funnel > 0.0) ? funnel * gDensityScale : 0.0;
    rainShare = (d > 0.0) ? saturate(rain / max(d, 1e-4)) * saturate(1.0 - cloud) : 0.0;

    if (detail)
    {
        float h = normalisedHeight(p);

        // The erosion is a threshold, and that is the whole difficulty with it.
        // It is written to carve a boundary out of a field that saturates to
        // one, so anything sitting below the threshold everywhere is not
        // carved but deleted - which is what happened to the first anvil that
        // reached the top of the domain. The tower's core is at or above one
        // and keeps its cauliflower; the anvil is detrained and diluted, an
        // eighth of that, and had every sample of it erased.
        //
        // Backing the erosion off with the ice fraction fixes it, and is what
        // the cloud looks like anyway: cauliflower is what liquid convection
        // does, and an anvil is a smooth fibrous sheet that has none of it.
        // Rain shafts get most of the erosion held off them too. Falling rain
        // is streaked along its own direction, not billowed: eroded like a
        // cumulus it breaks into floating lumps under the cloud base.
        float erosion = lerp(1.0, kAnvilErosion, iceFraction(p.y));
        erosion = lerp(erosion, kRainErosion, rainShare);

        // Larger-scale erosion first: breaks the smooth simulated boundary into
        // something with a silhouette.
        float4 b = gBaseNoise.SampleLevel(gWrap, p * BASE_SCALE * 2.2, 0);
        float  wfbm = b.g * 0.625 + b.b * 0.25 + b.a * 0.125;
        d = saturate(remap(d, wfbm * kShapeErosion * erosion, 1.0, 0.0, 1.0));
        if (d <= 0.0) return (funnel > 0.0) ? funnel * gDensityScale : 0.0;

        // Then the fine detail. Wispy near the base, billowy up top, which is
        // what separates a ragged underside from a hard cauliflower crown.
        float3 dn = gDetailNoise.SampleLevel(gWrap, p * DETAIL_SCALE, 0).rgb;
        float  dfbm = dn.r * 0.625 + dn.g * 0.25 + dn.b * 0.125;
        float  m = lerp(dfbm, 1.0 - dfbm, saturate(h * 4.0));
        d = saturate(remap(d, m * kDetailErosion * erosion, 1.0, 0.0, 1.0));
    }

    // Fade out across the same margin the solver relaxes in. The solver's
    // damping alone is not enough to hide the boundary, and the reason is the
    // local normalisation above: as the margin drives condensate down, the
    // local peak goes down with it, so the ratio stays near one and the anvil
    // stays fully opaque until it falls off a cliff. The first sheared storm
    // ended in a straight vertical edge in mid-air where its anvil met the
    // margin. Fading the rendered density directly is what removes it, and it
    // is honest about what it is: the domain ends, and a real anvil does not.
    d *= 1.0 - lateralMargin(p);

    // The funnel is not subject to the erosion or the margin: it is authored
    // geometry, and eroding it with the same noise that carves cauliflower
    // turns it into a string of floating lumps.
    if (funnel > 0.0)
    {
        debrisShare = (funnel > d) ? funnelDebris : funnelDebris * saturate(funnel / max(d, 1e-4));
        d = max(d, funnel);
    }
    return d * gDensityScale;
}

// The shading passes that do not care which is which.
float sampleDensity(float3 p, bool detail)
{
    float rainShare, debrisShare;
    return sampleDensityAndRain(p, detail, rainShare, debrisShare);
}

// ---- lighting -------------------------------------------------------------

// Size of one light-volume cell in world metres, per axis.
float3 lightVolumeCell()
{
    return (cloudBoxMax() - cloudBoxMin()) / (float)gLightVolumeRes;
}

// The transmittance volume is coarse - 157 m a cell across this domain - and
// trilinear interpolation of it is smooth but piecewise. On a surface facing
// the camera that is invisible; on one nearly tangent to the view it is not,
// because a small step across the screen crosses many cells. Phase 05 flew the
// camera round to exactly those angles and the anvil's upper surface came out
// combed with regular striations one cell apart.
//
// The dither is per pixel and per frame, so what was a static band becomes
// noise the temporal resolve averages away. It is a great deal cheaper than
// the alternative: 192 cubed removes the banding too, and costs 3.4 times the
// light volume to build.
float sampleLightVolume(float3 p, float3 dither)
{
    float3 uvw = (p + dither - cloudBoxMin()) / (cloudBoxMax() - cloudBoxMin());
    return gLightVolume.SampleLevel(gClamp, uvw, 0);
}

float sampleLightVolume(float3 p)
{
    return sampleLightVolume(p, float3(0.0, 0.0, 0.0));
}

float henyeyGreenstein(float c, float g)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

float cloudPhase(float c)
{
    return lerp(henyeyGreenstein(c, kSilverLobe), henyeyGreenstein(c, kBackLobe), kLobeMix);
}

// Radiance a sample receives from the current flash. No shadowing: a second
// transmittance volume for something that lasts a fifth of a second would cost
// a rebuild per frame, and the distance falloff is standing in for it. What
// sells it is that only part of the storm lights up.
float3 flashLight(float3 p)
{
    if (gFlashIntensity <= 0.0) return float3(0.0, 0.0, 0.0);
    float r = length(gFlashPosition - p);
    return kFlashColour * gFlashIntensity * exp(-r / kFlashReach) * kFlashGain;
}

#endif
