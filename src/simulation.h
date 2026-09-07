// Storm - the cloud simulation.
//
// A staggered-grid solver on the GPU, ported from the CPU solvers validated in
// Spikes 03 and 04. It runs at its own rate, decoupled from the display: cloud
// dynamics at 20 Hz is more than enough, and freeing it from the refresh rate
// means a 144 Hz monitor does not cost 144 fluid steps a second.
//
// No vorticity confinement. Spike 03 measured it convecting a statically stable
// atmosphere from rest to 29.9 m/s and destroying placement control.
#pragma once

#include "gpu.h"
#include "slots.h"

// The environment the storm grows in, as one vector.
//
// This is the sounding-parameter vector the plan asks Phase 03 for, and the
// point of it is that it is the *only* place the atmosphere is described.
// Nothing in the solver carries a tuned constant of its own: every number the
// physics reads arrives from here, so a different storm is a different vector
// and nothing else. Phase 05 derives one from the run's seed; until then there
// is a single default, and it is the one below.
struct Sounding
{
    // ---- the boundary layer
    // Neutral enough that a thermal crosses it for free, and topped just below
    // the condensation level so the environment never saturates on its own.
    float mixedTop      = 900.0f;    // m
    float lapseMixed    = 0.0005f;   // K/m of potential temperature
    // Sets the cloud base, and it is a tight constraint rather than a taste:
    // vapour is surfaceRH * satSurface and saturates where satVapour matches
    // it, so 0.65 puts the base at 948 m - just above the mixed layer.
    float surfaceRH     = 0.65f;

    // ---- the free troposphere
    //
    // This number and the equilibrium level below are not independent, and
    // that is the single most useful thing Phase 03 learned about directing
    // the storm. A parcel lifted out of the mixed layer gains 2488 K per unit
    // of condensate; the environment gains lapseTropo per metre. Where those
    // cross is the parcel's own level of neutral buoyancy, and an inversion
    // placed above it caps nothing at all. At 4.0 K/km the crossing is at
    // 9.2 km, so the cap at 10.5 km was never reached: the plume ran out of
    // buoyancy at mid-level and spread there instead, which the /arc band
    // profile showed as 22,700 cloudy cells at 5.6 km against 2,700 at the
    // cap. At 3.4 K/km the crossing moves to just above the cap, and the
    // stable layer is what stops the storm - which is what it is for.
    float lapseTropo    = 0.0034f;   // K/m - conditionally unstable
    float upperRH       = 0.30f;
    float rhTransition  = 6000.0f;   // m over which humidity falls to upperRH

    // ---- the lid
    //
    // A capping inversion of finite strength and depth, sitting wherever the
    // arc has got to. Distinct from the equilibrium level below, which does
    // not move: this is the thing that erodes through an afternoon and lets a
    // fair-weather cumulus field become a storm.
    //
    // Finite is the operative word. The first version of the acts moved the
    // whole stratosphere down to act as the lid, and raising it 4 km between
    // acts changed the environment's potential temperature by seventy kelvin
    // at a stroke - the entire layer above the old lid became buoyant at once
    // and the domain filled with half a million cells of spurious cloud. A
    // 4 K step over 500 m does the same job to a parcel and is something the
    // atmosphere can be moved through.
    float capStrength   = 5.5f;      // K
    float capDepth      = 500.0f;    // m

    // ---- the cap
    // The equilibrium level: where the anvil spreads. A real storm overshoots
    // it, and this one overshoots it by 1.0 to 1.5 km, which is why the plan
    // asks for a calibration curve here rather than a direct mapping.
    // Placed from the calibration sweep, not guessed. Cloud top against a
    // prescribed equilibrium level measures:
    //
    //     EL      5000   6500   8000   9500  10500  12000
    //     top     6975   8235   9135   9855  10125   9855
    //
    // - monotonic to about 9.5 km and then flat, because above that the cap
    // stops being what limits the storm and the parcel's own level of neutral
    // buoyancy takes over at around 10 km. At 10500 the storm was topping out
    // on its own rather than against the lid, which is why nothing spread:
    // there is no anvil without something for the outflow to spread under.
    // 9200 puts the cap under the neutral level, so the storm arrives at it
    // with about a kilometre of overshoot and has to go sideways.
    float equilibrium   = 9200.0f;   // m
    float lapseStrato   = 0.0230f;   // K/m - a strong inversion above it

    // ---- saturation
    float satSurface    = 0.0210f;   // saturation mixing ratio at the ground
    float satScale      = 2200.0f;   // m, its e-folding depth

    // ---- ice
    // Without this the anvil does not exist. Detrained condensate meets air at
    // 30% humidity and, under an instantaneous saturation adjustment, is gone
    // in one step; the tower measured 11 km tall and rendered as a bare column
    // because everything that left it evaporated. Ice neither evaporates nor
    // falls out at anything like the rate water does, and those two rates are
    // the anvil.
    //
    // This is a glaciation level, not a freezing level, and the difference
    // decides the storm's whole silhouette. Set at the 0 C line - 4 km here -
    // everything the tower detrains from 4 km upward becomes permanent, and
    // the cloud grows a pancake at 5 to 6 km: the /arc band profile measured
    // 21,700 cloudy cells at 5.6 km against 2,700 at the equilibrium level.
    // Deep convection actually glaciates near -38 C, around 8 km in this
    // sounding, and putting it there leaves mid-level detrainment free to
    // evaporate while the outflow at the cap survives - which is the anvil.
    float glaciationLevel = 8000.0f;  // m
    float iceEvaporation  = 0.004f;   // fraction of the water rate
    float fallout         = 0.00035f; // per second, below it
    float iceFallout      = 0.00002f; // per second, above it

    // And the freezing level, which is a different height doing a different
    // job. Warm-rain autoconversion only happens below it: cloud water above
    // freezing is ice, and ice does not collide into raindrops. Run without
    // this the storm converts its entire depth to rain - the cross-section
    // showed a 9 km column of precipitation with a thin updraft up one side,
    // and the domain totals had more rain in them than cloud.
    float freezingLevel   = 4000.0f;  // m

    // ---- rain
    //
    // The fourth scalar channel, which was there and unused. Rain is not
    // decoration: it is what makes the downdraft. Condensate is converted to
    // rain, rain falls out of the cloud at its own speed, and where it falls
    // through unsaturated air it evaporates and cools it - and cold air
    // descends. Without the evaporation term a rain shaft is a texture; with
    // it, the storm acquires the downdraft that spreads its base.
    float rainFallSpeed  = 8.0f;      // m/s, relative to the air
    float autoThreshold  = 0.0012f;   // condensate above which rain forms
    float autoRate       = 0.0025f;   // per second
    float accretionRate  = 0.5f;      // per second per unit rain - falling rain collects cloud
    float rainEvaporation = 0.80f;    // per second per unit deficit per unit rain
    float rainOpaque     = 0.0060f;   // rain mixing ratio that reads fully opaque

    // ---- the lateral margin
    // The sides are periodic because the pressure solve wants them to be, so
    // an anvil that reaches one edge comes back in at the other. This is the
    // fraction of the half-width over which everything is relaxed back to the
    // environment, absorbing the outflow before it can wrap.
    // 28% cost the anvil more than it saved. Across the shear the domain half
    // width is 7.2 km, so a margin that deep started absorbing outflow 5.2 km
    // from the storm axis - and mass continuity puts the anvil edge at about
    // 4 m/s once it is 5 km out, so that is where it stopped. Narrower and
    // more absorbent does the same job to a wrap-around in less room.
    float lateralMargin = 0.16f;
    float lateralRate   = 0.045f;    // per second at the very edge

    // ---- the wind
    // The solver runs in the storm-relative frame: what it sees is the
    // environmental wind minus the storm's own motion. Anchoring that frame on
    // the inflow layer rather than the mean wind is what keeps the forcing
    // feeding the same column while the upper levels stream past it, which is
    // what tilts the tower and carries the anvil downstream.
    // 4.0 m/s per km - 24 m/s across the storm layer, which is supercell
    // shear rather than the moderate value Phase 03 could carry. Phase 03
    // shipped 2.0 because at 3.5 the tower tilted hard and came apart, and the
    // reason was the one Spike 04 gave: a sheared updraft needs rotation to
    // sustain it. With the mesocyclone directed, the same sweep reads:
    //
    //     shear        2.0    3.0    4.0    5.0   m/s/km
    //     top, still  9765   8775   8235   6885   m
    //     top, spun  11475  11385  11205  10665   m
    //
    // Unrotated the storm loses 30% of its depth across that range and half
    // its updraft. Rotating, it loses 7%. That is the whole argument for
    // directing rotation, and it is why this line could move.
    float shear[2]       = { 4.0f, 0.0f };  // m/s per km, through shearTop
    float shearTop       = 6000.0f;         // m
    float stormMotion[2] = { 0.0f, 0.0f };  // m/s
    // How hard the environment is held against the storm's own circulation.
    // Without it the periodic domain has nothing maintaining the shear and the
    // storm mixes its own environment away within a few hundred seconds.
    float windRelaxation = 0.0015f;         // per second
    // Nothing for temperature and moisture. There was, while the lid was being
    // moved by relaxation, and it cost the storm two kilometres of depth: the
    // forcing works by accumulating heat and moisture in clear air, and a
    // relaxation that pulls clear air back to the sounding is subtracting from
    // the forcing every step. The environment shift in CSDamp made it
    // unnecessary - that moves the lid exactly rather than by nudging - and
    // the lateral margin already holds the far field. Left here at zero
    // because it is the kind of term that looks obviously right and is not.
    float envRelaxation  = 0.0f;            // per second

    // ---- rotation
    //
    // Directed, not emergent. Spike 04 settled that: a mesocyclone does not
    // arise from the hodograph at this resolution, and waiting for one is
    // waiting for nothing. So a target swirl is imposed about the storm's axis
    // and the flow is nudged toward it - but only its tangential component, so
    // the inflow and outflow through the same region are left alone.
    //
    // What it buys is not the look of rotation. It is that a sheared updraft
    // survives: the spike measured a rotating updraft still running at
    // 66.8 m/s with the forcing off where a non-rotating one had fallen to
    // 37.8, and cloud top two kilometres higher. Phase 03 shipped 2 m/s/km of
    // shear because 3.5 tore the storm apart. This is what pays for more.
    float rotationSpeed  = 24.0f;    // m/s peak tangential
    float rotationRadius = 1800.0f;  // m - the Rankine core
    float rotationBase   = 900.0f;   // m
    float rotationTop    = 8000.0f;  // m
    float rotationRate   = 0.05f;    // per second, how hard the flow is held to it
    // The axis leans downstream with the tower it sits in. A vertical axis in
    // a tilted storm applies the swirl beside the updraft at upper levels
    // rather than through it.
    float rotationTilt   = 0.30f;    // m of x per m of height

    // The forcing sits upstream of centre, so the anvil has the long side of
    // the domain to stream into rather than reaching the margin in half the
    // time. It is a framing decision, not a physical one.
    float forceOffset[2] = { -5000.0f, 0.0f };  // m

    // ---- the forcing
    // Wide and shallow rather than a point source: a narrow plume rises as a
    // single mushroom, and a broad heated sheet inside the mixed layer feeds
    // condensation across a whole layer, which is what gives one flat base
    // under several turrets.
    // Narrower and cooler again in Phase 04. An anvil only reads as an anvil
    // when it is wider than what feeds it, and the domain caps how wide the
    // anvil can get, so the tower is what has to give. What makes this
    // affordable is the mesocyclone: a rotating updraft sustains itself on far
    // less forcing than an unrotating one needs, which is the whole reason
    // Spike 04 said to direct the rotation.
    float forceHeat     = 0.0090f;
    float forceMoisture = 2.4e-6f;
    float forceRadius   = 1200.0f;   // m
    float forceDepth    = 400.0f;    // m, vertical half-depth
    float forceHeight   = 400.0f;    // m above the ground, inside the mixed layer

    // ---- rendering
    // The condensate at which cloud reads fully opaque, as a fraction of the
    // peak condensate in the same neighbourhood, plus an absolute floor. Local
    // rather than global because a tower core carries ten times what the anvil
    // it feeds does, and the erosion is a threshold: see sampleDensity.
    float opaqueFraction = 0.62f;
    float opaqueFloor    = 0.0016f;
};

// The storm's timeline, in seconds of storm time.
//
// The acts are the arc: a cumulus that grows and would die on its own, a
// congestus that does not, and a cumulonimbus that reaches the cap.
//
// What separates them is the cap, not the forcing. That is not the obvious
// choice and it is worth saying why. In a conditionally unstable atmosphere a
// parcel that reaches its condensation level is committed: latent heat carries
// it to its level of neutral buoyancy whatever the forcing did, so a weaker
// heat source gives a thinner cumulonimbus, not a cumulus. What actually holds
// a fair-weather sky down is a lid, and what turns a fair-weather sky into a
// storm is that lid eroding through the afternoon. So the arc raises the
// equilibrium level, and the storm follows it up.
//
// The environment is nudged toward the sounding in clear air, which is what
// lets the lid move at all: the buoyancy every cell feels is measured against
// thetaEnv, so a cap that jumped would make the whole layer above it buoyant
// at once. Moved slowly against that relaxation, the atmosphere restratifies
// as fast as the lid rises and only the storm notices.
struct StormArc
{
    // The lid has to be out of the way while the storm still has its forcing,
    // which is the one thing these numbers have to get right. Timed with the
    // lid clearing at 1500 s, the tower reached 8.3 km at 1000 s, punched into
    // an inversion that was still at 6.2 km, and had rained itself out by the
    // time the way was open.
    // A floor under the forcing, normally zero. /forced raises it, which is
    // the difference between watching a storm die and watching a sky keep
    // working: the arc ramps the boundary-layer heating to nothing at the end
    // of the decay, and after that there is no energy going into the domain at
    // all. Held at a fraction instead, the same heat source keeps running
    // under whatever the storm left behind.
    // /forced. Not a floor under the forcing but a pulse, which is a
    // distinction the solver insisted on: held at a constant level the heat
    // source settles into a steady dry thermal - 5.6 m/s, unvarying for four
    // thousand seconds, never once condensing - because a running plume
    // ventilates the heating zone faster than it can accumulate anything. The
    // original storm only got going because it started from still air. So each
    // pulse starts from still air too.
    float sustainAmplitude = 0.0f;    // 0 disables the whole thing
    float sustainPeriod    = 2600.0f; // s - one full replay of the schedule
    float sustainRelaxation = 0.0f;   // per second, and only after the arc

    float cumulus    = 250.0f;    // s - shallow, under an intact lid
    float congestus  = 500.0f;    // s - the lid erodes, towers reach mid-level
    float mature     = 800.0f;    // s - the lid is gone, the tower reaches the cap
    float sustain    = 1100.0f;   // s at full depth
    float decay      = 700.0f;    // s fading out

    float capCumulus   = 2600.0f;   // m - where the lid sits in each act
    float capCongestus = 6000.0f;   // m
    float rampCumulus  = 0.32f;     // and how hard the boundary layer is forced

    // Forcing strength, 0 to 1.
    float ramp(float stormTime) const;
    // How much of the sounding's rotation is being asked for, 0 to 1. A
    // supercell acquires its mesocyclone as it deepens rather than arriving
    // with one, and switching the swirl on under a shallow cumulus produces a
    // spinning cumulus, which is not a thing.
    float rotation(float stormTime) const;

    // The tornado's own life, 0 to 1: how far the funnel has descended from
    // the cloud base, and how strongly it is there at all. Separate curves
    // because a tornado ropes out by thinning and tilting long after it has
    // finished descending, and one curve cannot do both.
    float funnelDescent(float stormTime) const;
    float funnelIntensity(float stormTime) const;

    // When the funnel starts to come down, relative to the mature act. A
    // tornado follows the mesocyclone rather than arriving with it.
    float tornadoOnset = 0.45f;    // fraction of the way through the sustain
    float tornadoDescend = 220.0f; // s to reach the ground
    float tornadoHold  = 420.0f;   // s on the ground
    float tornadoRope  = 260.0f;   // s roping out
    // Height of the capping inversion, rising through the acts.
    float cap(float stormTime, float soundingEquilibrium) const;
    // And how strong it still is. A lid that only rises is still four kelvin
    // of extra warmth sitting exactly where the tower is trying to climb: with
    // the lid rising alone the storm topped out at 8.9 km against the 11.0 km
    // the same forcing reached without one. A capping inversion erodes by
    // weakening, which is both what happens and what leaves the mature act a
    // clean atmosphere to work in.
    float strength(float stormTime, float soundingStrength) const;

    float duration() const { return mature + sustain + decay; }
};

// Mirrors the SimParams cbuffer in sim.hlsli, row for row.
struct alignas(16) SimConstants
{
    float   origin[3];      float cellSize;
    int32_t resolution[3];  float forceDepth;
    float   dt;             float time;         float forceRamp;    float qcRef;
    float   forceCentre[3]; float forceRadius;
    float   forceHeat;      float forceMoisture; float mixedTop;    float lapseMixed;
    float   equilibrium;    float lapseTropo;   float lapseStrato;  float surfaceRH;
    float   upperRH;        float rhTransition; float satSurface;   float satScale;
    float   glaciationLevel; float iceEvaporation; float fallout;   float iceFallout;
    float   freezingLevel;  float envRelaxation; float capHeight;    float capHeightPrev;
    float   capStrength;    float capStrengthPrev; float capDepth;  float pad1;
    float   lateralMargin;  float lateralRate;  float shearTop;     float qcFloor;
    float   shear[2];       float stormMotion[2];
    float   windRelaxation; float rainFallSpeed; float autoThreshold; float autoRate;
    float   accretionRate;  float rainEvaporation; float rainOpaque; float rotationSpeed;
    float   rotationRadius; float rotationBase;  float rotationTop;  float rotationRate;
    float   rotationTilt;   float pad0[3];
};
static_assert(sizeof(SimConstants) % 16 == 0, "SimConstants must be 16-byte aligned");

// One sample of the storm, read back from the GPU. The /arc harness writes a
// row of these per sampled interval of storm time; everything in Phase 03 with
// a number attached to it - the cloud-top calibration curve above all - was
// measured through here rather than judged off a screenshot.
struct SimStats
{
    float stormTime = 0.0f;     // seconds of simulated storm time
    float cloudBase = 0.0f;     // metres, 0 when there is no cloud
    float cloudTop = 0.0f;
    float updraftMax = 0.0f;    // m/s
    float downdraftMax = 0.0f;  // m/s, negative
    float condensate = 0.0f;    // domain total, summed mixing ratio over cells
    float rain = 0.0f;
    float radius = 0.0f;        // metres from the forcing axis to the furthest cloudy cell
    float cloudyCells = 0.0f;

    // Per height band: peak condensate, and the number of cloudy cells - which
    // is the cloud's horizontal area at that height, and so the only direct
    // measure of whether an anvil is spreading or the tower is just tall.
    static const int kBands = 32;
    float peakByBand[kBands] = {};
    float cellsByBand[kBands] = {};

    // The mesocyclone, measured the way Spike 04 said to measure it: the
    // correlation between vertical velocity and vertical vorticity through the
    // storm layer. Peak vorticity is reported alongside it precisely because
    // it is the number NOT to steer on - it is non-monotonic in the rotation
    // asked for, and watching the two disagree is the point.
    float updraftVorticityCorrelation = 0.0f;
    float peakVorticity = 0.0f;   // 1/s

    bool hasCloud() const { return cloudyCells > 0.0f; }
};

// Derives a storm from a seed by perturbing the sounding, which is the only
// way Spike 03 left open: it measured cloud top varying by 0.4% across four
// noise seeds - every seed makes the same storm - while the sounding controls
// it monotonically. So the seed moves the atmosphere, not the noise.
//
// The ranges are deliberately modest. Each one on its own is a storm that is
// recognisably the same kind of thing; together they are a sky that does not
// repeat, which is what a screensaver needs.
void DeriveStorm(uint32_t seed, Sounding& sounding, StormArc& arc);

struct Simulation
{
    Gpu* gpu = nullptr;

    // The whole storm, as data. Everything below this point is machinery.
    Sounding sounding;
    StormArc arc;
    uint32_t seed = 1;

    // 224 x 160 x 160 at 90 m: 20.16 km along the shear, 14.40 km deep, 14.40
    // km across. Phase 02's 6.4 km cube was outgrown before the forcing had
    // even decayed - the /arc harness measured cloud top pegged at 6375 m,
    // which is the lid rather than the cap, and a radius still climbing
    // through 4.4 km along the ceiling. A cumulonimbus needs an equilibrium
    // level around 10 km with stratosphere above it for the anvil.
    //
    // Long in x because that is the shear direction and the anvil goes that
    // way; nearly square otherwise, because the light volume is a cube sampled
    // in normalised box coordinates and a domain far from cubic would shade
    // with a very different resolution vertically than horizontally.
    UINT resolution[3] = { 224, 160, 160 };
    float cellSize = 90.0f;

    // Centred on the camera axis, 18 km downrange: far enough that a 14 km
    // storm fits in a 55-degree field of view with the anvil inside the frame,
    // close enough that the cumulus it starts as is still legible.
    float origin[3] = { -10080.0f, 0.0f, 10800.0f };

    // Storm time per step. Semi-Lagrangian advection is unconditionally stable,
    // so this is chosen for how fast the cloud should evolve rather than for a
    // CFL limit: 1 s per step at 20 steps a second runs the sky twenty times
    // faster than real weather. At half this the cloud is meteorologically
    // more comfortable and takes forty seconds to appear, which is too long to
    // wait; and because the step count per second is unchanged, the speed-up
    // is free. A 25 m/s updraft still moves only half a cell per step.
    float stepSeconds = 1.0f;
    int   stepsPerSecond = 20;
    int   jacobiIterations = 20;

    ComPtr<ID3D12Resource> velocity[3][2];   // u, v, w - each ping-ponged
    ComPtr<ID3D12Resource> scalars[2];       // theta, vapour, condensate
    ComPtr<ID3D12Resource> pressure[2];
    ComPtr<ID3D12Resource> divergence;
    // One slot per step a frame may take, plus slot 0 for the render passes.
    // A single slot is wrong as soon as anything in the constants differs
    // between steps: the CPU writes all of them before the GPU runs any, so
    // every step in the frame would see the last one written. That was
    // harmless while the only per-step value was a slowly varying forcing
    // ramp, and stopped being harmless when the arc started moving the lid -
    // the environment shift is a difference between consecutive steps, and
    // three steps sharing one slot would apply the same difference three times
    // and lose the other two.
    static const int kMaxStepsPerFrame = 3;
    static const int kConstantSlots = kMaxStepsPerFrame + 1;
    ComPtr<ID3D12Resource> constantBuffer;
    uint8_t* constantsMapped = nullptr;
    UINT constantStride = 0;

    ComPtr<ID3D12PipelineState> psoInitialise, psoAdvect, psoForces, psoBuoyancy;
    ComPtr<ID3D12PipelineState> psoMicrophysics, psoDamp, psoDivergence, psoJacobi, psoProject;
    ComPtr<ID3D12PipelineState> psoStatsClear, psoStats;

    // Diagnostics. Allocated always - eight uints and a readback buffer is
    // nothing - but dispatched only when the harness asks.
    ComPtr<ID3D12Resource> statsBuffer, statsReadback;

    int   phase = 0;              // which set holds the current data
    float simulatedTime = 0.0f;
    float accumulator = 0.0f;
    bool  initialised = false;
    bool  currentIsReadable = false;

    bool create(Gpu& g, ID3D12RootSignature* rootSignature);
    void shutdown();

    // Advances by real elapsed seconds, stepping zero or more times. Returns
    // true if the volume changed, so the caller knows to rebuild the light
    // volume - which is the plan's amortisation: lighting at simulation rate,
    // not frame rate.
    bool advance(float elapsedSeconds);

    void reset();                                     // re-seed the domain

    // Keeps the sky working after the arc has run out, for /forced.
    //
    // A floor under the forcing is not enough on its own, and measuring that
    // was worth the trouble: with the floor alone at 0.15, 0.30 and even 0.45,
    // domain condensate still collapsed from 4.5 to 0.01 by 4000 seconds and
    // never came back. The reason is that a storm's job is to consume the
    // instability it grew in, and nothing here puts it back - the environmental
    // relaxation that would has been at zero since Phase 03, where it was found
    // to be quietly subtracting from the forcing.
    //
    // So this raises both together: heat going in, and an environment being
    // restored for it to work on. That pair is what a real sky has and a closed
    // box does not.
    void sustain(float floorFraction);

    // Ends this storm and starts a different one. The screensaver runs all
    // night; one storm is two and a half minutes.
    void restart(uint32_t nextSeed);
    // True once the storm is over and the sky is empty enough to cut away.
    bool finished() const;

    // The constants the render passes should read: the state after the last
    // step this frame. The steps themselves each read their own slot.
    D3D12_GPU_VIRTUAL_ADDRESS constantsAddress() const;

    // Leaves the current data in a state the render passes can sample.
    void transitionForReading();

    // Records the reduction and the copy to the readback buffer. Call after
    // advance() and before transitionForReading(), then submit and wait, then
    // fetchStats(). Split in two because the readback has to cross a fence.
    void gatherStats();
    SimStats fetchStats() const;

    UINT cellCount() const { return resolution[0] * resolution[1] * resolution[2]; }
    float extent(int axis) const { return (float)resolution[axis] * cellSize; }
    float centre(int axis) const { return origin[axis] + extent(axis) * 0.5f; }

private:
    void createStatsBuffers();
    void fillConstants(SimConstants& c) const;
    void step(int constantSlot);
    void bindPhase(int simPhase, int jacobiPhase);
    void writeConstants(int slot, float atTime);
    void barrierUav(ID3D12Resource* r);
    void setSetState(int index, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to);

    ID3D12RootSignature* m_rootSignature = nullptr;
    D3D12_RESOURCE_STATES m_setState[2] = {};
};
