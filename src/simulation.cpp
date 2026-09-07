#include "simulation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static ComPtr<ID3D12Resource> CreateVolume(Gpu& gpu, DXGI_FORMAT format,
                                           UINT width, UINT height, UINT depth,
                                           const char* what)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = (UINT16)depth;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    ComPtr<ID3D12Resource> resource;
    STORM_CHECK(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    nullptr, IID_PPV_ARGS(&resource)), what);
    return resource;
}

static void MakeVolumeViews(Gpu& gpu, ID3D12Resource* r, DXGI_FORMAT format,
                            UINT depth, UINT uavSlot, UINT srvSlot)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D.WSize = depth;
    gpu.device->CreateUnorderedAccessView(r, nullptr, &uav, gpu.srvHeap.cpu(uavSlot));

    if (srvSlot != UINT_MAX)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = format;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Texture3D.MipLevels = 1;
        gpu.device->CreateShaderResourceView(r, &srv, gpu.srvHeap.cpu(srvSlot));
    }
}

bool Simulation::create(Gpu& g, ID3D12RootSignature* rootSignature)
{
    gpu = &g;
    m_rootSignature = rootSignature;

    const UINT nx = resolution[0], ny = resolution[1], nz = resolution[2];
    // All three velocity components are allocated a row taller than the cell
    // grid so the vertical one has somewhere to put its top face. u and w waste
    // that row, which costs under one percent and keeps every dispatch uniform.
    const UINT vh = ny + 1;

    const UINT uavSlots[3][2] = {
        { kUavSimU0, kUavSimU1 }, { kUavSimV0, kUavSimV1 }, { kUavSimW0, kUavSimW1 }
    };
    const UINT srvSlots[3][2] = {
        { kSrvSimU0, kSrvSimU1 }, { kSrvSimV0, kSrvSimV1 }, { kSrvSimW0, kSrvSimW1 }
    };

    for (int component = 0; component < 3; ++component)
    {
        for (int set = 0; set < 2; ++set)
        {
            velocity[component][set] = CreateVolume(*gpu, DXGI_FORMAT_R16_FLOAT, nx, vh, nz,
                                                    "simulation velocity");
            MakeVolumeViews(*gpu, velocity[component][set].Get(), DXGI_FORMAT_R16_FLOAT, nz,
                            uavSlots[component][set], srvSlots[component][set]);
        }
    }

    for (int set = 0; set < 2; ++set)
    {
        scalars[set] = CreateVolume(*gpu, DXGI_FORMAT_R16G16B16A16_FLOAT, nx, ny, nz,
                                    "simulation scalars");
        MakeVolumeViews(*gpu, scalars[set].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, nz,
                        set == 0 ? kUavSimS0 : kUavSimS1,
                        set == 0 ? kSrvSimS0 : kSrvSimS1);

        // The pressure solve wants more precision than the fields it corrects.
        pressure[set] = CreateVolume(*gpu, DXGI_FORMAT_R32_FLOAT, nx, ny, nz, "simulation pressure");
        MakeVolumeViews(*gpu, pressure[set].Get(), DXGI_FORMAT_R32_FLOAT, nz,
                        set == 0 ? kUavSimP0 : kUavSimP1, UINT_MAX);
    }

    divergence = CreateVolume(*gpu, DXGI_FORMAT_R32_FLOAT, nx, ny, nz, "simulation divergence");
    MakeVolumeViews(*gpu, divergence.Get(), DXGI_FORMAT_R32_FLOAT, nz, kUavSimDiv, UINT_MAX);

    createStatsBuffers();

    constantStride = (sizeof(SimConstants) + 255) & ~255u;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = (UINT64)constantStride * kConstantSlots;
    cbDesc.Height = 1;
    cbDesc.DepthOrArraySize = 1;
    cbDesc.MipLevels = 1;
    cbDesc.SampleDesc.Count = 1;
    cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES upload = {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    upload.CreationNodeMask = 1;
    upload.VisibleNodeMask = 1;
    STORM_CHECK(gpu->device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &cbDesc,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(&constantBuffer)),
                "simulation constants");
    D3D12_RANGE noRead = { 0, 0 };
    STORM_CHECK(constantBuffer->Map(0, &noRead, (void**)&constantsMapped), "map simulation constants");

    struct { ComPtr<ID3D12PipelineState>* pso; const wchar_t* entry; const char* what; }
    passes[] = {
        { &psoInitialise,   L"CSInitialise",   "sim initialise PSO" },
        { &psoAdvect,       L"CSAdvect",       "sim advect PSO" },
        { &psoForces,       L"CSForces",       "sim forces PSO" },
        { &psoBuoyancy,     L"CSBuoyancy",     "sim buoyancy PSO" },
        { &psoMicrophysics, L"CSMicrophysics", "sim microphysics PSO" },
        { &psoDamp,         L"CSDamp",         "sim lateral damping PSO" },
        { &psoDivergence,   L"CSDivergence",   "sim divergence PSO" },
        { &psoJacobi,       L"CSJacobi",       "sim jacobi PSO" },
        { &psoProject,      L"CSProject",      "sim project PSO" },
        { &psoStatsClear,   L"CSStatsClear",   "sim stats clear PSO" },
        { &psoStats,        L"CSStats",        "sim stats PSO" },
    };
    for (auto& pass : passes)
    {
        Shader cs = gpu->compile(L"sim.hlsl", pass.entry, L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSignature;
        desc.CS = cs.bytecode();
        STORM_CHECK(gpu->device->CreateComputePipelineState(
                        &desc, IID_PPV_ARGS(pass.pso->ReleaseAndGetAddressOf())), pass.what);
    }

    m_setState[0] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    m_setState[1] = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

void Simulation::shutdown()
{
    if (constantBuffer && constantsMapped) { constantBuffer->Unmap(0, nullptr); constantsMapped = nullptr; }
    for (auto& component : velocity) for (auto& set : component) set.Reset();
    for (auto& set : scalars) set.Reset();
    for (auto& set : pressure) set.Reset();
    divergence.Reset();
    constantBuffer.Reset();
    statsReadback.Reset(); statsBuffer.Reset();
    psoStats.Reset(); psoStatsClear.Reset();
    psoProject.Reset(); psoJacobi.Reset(); psoDivergence.Reset();
    psoDamp.Reset(); psoMicrophysics.Reset();
    psoBuoyancy.Reset(); psoForces.Reset(); psoAdvect.Reset(); psoInitialise.Reset();
}

// ---- diagnostics ----------------------------------------------------------

// Eight scalars, two 32-band profiles, then seven mesocyclone moments.
static const UINT kStatsSlots = 8 + 2 * SimStats::kBands + 7;

void Simulation::createStatsBuffers()
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kStatsSlots * sizeof(uint32_t);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    STORM_CHECK(gpu->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                     nullptr, IID_PPV_ARGS(&statsBuffer)),
                "simulation stats buffer");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = kStatsSlots;
    uav.Buffer.StructureByteStride = sizeof(uint32_t);
    gpu->device->CreateUnorderedAccessView(statsBuffer.Get(), nullptr, &uav,
                                           gpu->srvHeap.cpu(kUavSimStats));

    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    STORM_CHECK(gpu->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST,
                                                     nullptr, IID_PPV_ARGS(&statsReadback)),
                "simulation stats readback");
}

void Simulation::gatherStats()
{
    // The reduction reads the current set through its UAVs, so it has to be in
    // that state - which it is straight after a step, but not after a frame has
    // handed it to the render passes.
    setSetState(phase, m_setState[phase], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    bindPhase(phase, 0);
    gpu->cmd->SetPipelineState(psoStatsClear.Get());
    gpu->cmd->Dispatch(1, 1, 1);   // one 64-thread group covers both profiles
    barrierUav(statsBuffer.Get());

    gpu->cmd->SetPipelineState(psoStats.Get());
    gpu->cmd->Dispatch((resolution[0] + 7) / 8, (resolution[1] + 7) / 8, resolution[2]);
    barrierUav(statsBuffer.Get());

    auto toCopy = Gpu::transition(statsBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu->cmd->ResourceBarrier(1, &toCopy);
    gpu->cmd->CopyResource(statsReadback.Get(), statsBuffer.Get());
    auto back = Gpu::transition(statsBuffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    gpu->cmd->ResourceBarrier(1, &back);
}

SimStats Simulation::fetchStats() const
{
    SimStats out;
    out.stormTime = simulatedTime;

    uint32_t* raw = nullptr;
    D3D12_RANGE range = { 0, kStatsSlots * sizeof(uint32_t) };
    if (FAILED(statsReadback->Map(0, &range, (void**)&raw))) return out;

    // Cell indices are stored plus one so that zero can mean "no cloud".
    if (raw[0] > 0) out.cloudTop  = origin[1] + ((float)raw[0] - 0.5f) * cellSize;
    if (raw[1] != 0xFFFFFFFFu && raw[1] > 0)
                    out.cloudBase = origin[1] + ((float)raw[1] - 0.5f) * cellSize;

    out.updraftMax   = (float)raw[2] / 100.0f - 100.0f;
    out.downdraftMax = (raw[3] == 0xFFFFFFFFu) ? 0.0f : (float)raw[3] / 100.0f - 100.0f;
    out.condensate   = (float)raw[4] / 1.0e5f;
    out.rain         = (float)raw[5] / 1.0e5f;
    out.radius       = (float)raw[6];
    out.cloudyCells  = (float)raw[7];

    for (int b = 0; b < SimStats::kBands; ++b)
    {
        out.peakByBand[b] = (float)raw[8 + b] / 1.0e6f;
        out.cellsByBand[b] = (float)raw[8 + SimStats::kBands + b];
    }

    // The mesocyclone. Undo the bias-and-scale encoding the reduction had to
    // use - there is no float atomic in cs_6_0 - and form the Pearson
    // correlation from the raw moments.
    {
        const uint32_t* m = raw + 72;
        const double n = (double)m[0];
        out.peakVorticity = (float)m[6] / 1.0e5f;
        if (n > 100.0)
        {
            const double sw  = (double)m[1] / 64.0   - n * 50.0;
            const double sz  = (double)m[2] / 1.0e5  - n * 0.1;
            const double sw2 = (double)m[3] / 4.0;
            const double sz2 = (double)m[4] / 1.0e6;
            const double swz = (double)m[5] / 2000.0 - n * 8.0;

            const double covariance = swz - sw * sz / n;
            const double varW = sw2 - sw * sw / n;
            const double varZ = sz2 - sz * sz / n;

            // Clamped, and deliberately so. The moments are quantised
            // independently, so nothing guarantees the ratio lands inside the
            // range a correlation is allowed to occupy - and a value outside
            // it should read as the noise it is, not as a bigger number.
            if (varW > 1.0e-6 && varZ > 1.0e-12)
            {
                const double r = covariance / std::sqrt(varW * varZ);
                out.updraftVorticityCorrelation = (float)(std::max)(-1.0, (std::min)(1.0, r));
            }
        }
    }

    D3D12_RANGE nothing = { 0, 0 };
    statsReadback->Unmap(0, &nothing);
    return out;
}

D3D12_GPU_VIRTUAL_ADDRESS Simulation::constantsAddress() const
{
    return constantBuffer->GetGPUVirtualAddress();
}

// Fills one slot with the constants as of a given moment of storm time.
void Simulation::writeConstants(int slot, float atTime)
{
    const float wasAt = simulatedTime;
    simulatedTime = atTime;
    SimConstants constants = {};
    fillConstants(constants);
    simulatedTime = wasAt;
    std::memcpy(constantsMapped + (size_t)slot * constantStride, &constants, sizeof(constants));
}

// Smooth everywhere, because both of these are read as an environment the
// solver is relaxing toward and a kink in either shows up as a pulse.
static float SmoothStep(float from, float to, float x)
{
    const float t = std::min(std::max((x - from) / (to - from), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The forcing schedule the acts were built around.
static float ScheduledRamp(const StormArc& arc, float stormTime)
{
    const float onset = arc.cumulus * 0.35f;
    float value = arc.rampCumulus * SmoothStep(0.0f, onset, stormTime)
                + (1.0f - arc.rampCumulus) * SmoothStep(arc.cumulus, arc.mature, stormTime);
    const float end = arc.mature + arc.sustain;
    if (stormTime > end)
        value *= (std::max)(0.0f, 1.0f - (stormTime - end) / arc.decay);
    return value;
}

float StormArc::ramp(float stormTime) const
{
    if (sustainAmplitude <= 0.0f || stormTime <= duration())
        return ScheduledRamp(*this, stormTime);

    // /forced. Rather than invent a shape, replay the schedule that is already
    // known to build a storm - against whatever the last one left behind
    // instead of against a fresh sounding.
    //
    // Two shapes were tried before this and neither reached condensation. A
    // constant floor settles into a steady dry thermal, because a running
    // plume ventilates the heating zone faster than it can accumulate: 5.6 m/s,
    // unvarying, for four thousand seconds. A smooth pulse gives a proper
    // rhythm - the updraft breathes between 2.3 and 5.9 m/s on the period asked
    // for - and still tops out short of the 7 to 8 m/s the first cloud of a
    // fresh storm needs. What both were missing is that a storm from rest also
    // starts with a two-kelvin bubble in the initial condition, and neither
    // shape builds one against the mean wind blowing through the patch.
    const float into = std::fmod(stormTime - duration(), sustainPeriod);
    return sustainAmplitude * ScheduledRamp(*this, into);
}

// Rotation arrives with the congestus and is fully established by the time the
// tower reaches the cap, then holds until the storm is well into its decay.
float StormArc::rotation(float stormTime) const
{
    const float fading = mature + sustain + decay * 0.7f;
    return SmoothStep(cumulus, mature, stormTime)
         * (1.0f - SmoothStep(mature + sustain, fading, stormTime));
}

// The funnel comes down after the mesocyclone is established, holds, and then
// ropes out. Descent and intensity are deliberately different shapes: a
// tornado is at full strength while it is still descending, and it dies by
// thinning rather than by rising back into the cloud.
static float FunnelStart(const StormArc& arc)
{
    return arc.mature + arc.sustain * arc.tornadoOnset;
}

float StormArc::funnelDescent(float stormTime) const
{
    const float start = FunnelStart(*this);
    return SmoothStep(start, start + tornadoDescend, stormTime);
}

float StormArc::funnelIntensity(float stormTime) const
{
    const float start = FunnelStart(*this);
    const float ropeFrom = start + tornadoDescend + tornadoHold;
    return SmoothStep(start, start + tornadoDescend * 0.45f, stormTime)
         * (1.0f - SmoothStep(ropeFrom, ropeFrom + tornadoRope, stormTime));
}

// And how strong the lid still is.
float StormArc::strength(float stormTime, float soundingStrength) const
{
    return soundingStrength * (1.0f - SmoothStep(cumulus, mature, stormTime));
}

float StormArc::cap(float stormTime, float soundingEquilibrium) const
{
    if (stormTime <= cumulus) return capCumulus;
    if (stormTime <= congestus)
        return capCumulus + (capCongestus - capCumulus) * SmoothStep(cumulus, congestus, stormTime);
    return capCongestus + (soundingEquilibrium - capCongestus)
                        * SmoothStep(congestus, mature, stormTime);
}

void Simulation::fillConstants(SimConstants& c) const
{
    c.origin[0] = origin[0]; c.origin[1] = origin[1]; c.origin[2] = origin[2];
    c.cellSize = cellSize;
    for (int i = 0; i < 3; ++i) c.resolution[i] = (int32_t)resolution[i];

    c.dt = stepSeconds;
    c.time = simulatedTime;
    c.forceRamp = arc.ramp(simulatedTime);

    // Everything from here is the sounding, projected into the layout the
    // shader reads. There is deliberately no arithmetic in this block beyond
    // placing the forcing on the domain axis: if a number needs choosing, it
    // belongs in the Sounding, where a seed can reach it.
    c.forceCentre[0] = centre(0) + sounding.forceOffset[0];
    c.forceCentre[1] = origin[1] + sounding.forceHeight;
    c.forceCentre[2] = centre(2) + sounding.forceOffset[1];
    c.forceRadius    = sounding.forceRadius;
    c.forceDepth     = sounding.forceDepth;
    c.forceHeat      = sounding.forceHeat;
    c.forceMoisture  = sounding.forceMoisture;

    c.mixedTop      = sounding.mixedTop;
    c.lapseMixed    = sounding.lapseMixed;
    c.equilibrium   = sounding.equilibrium;
    c.lapseTropo    = sounding.lapseTropo;
    c.lapseStrato   = sounding.lapseStrato;
    c.surfaceRH     = sounding.surfaceRH;
    c.upperRH       = sounding.upperRH;
    c.rhTransition  = sounding.rhTransition;
    c.satSurface    = sounding.satSurface;
    c.satScale      = sounding.satScale;

    c.glaciationLevel = sounding.glaciationLevel;
    c.iceEvaporation  = sounding.iceEvaporation;
    c.fallout         = sounding.fallout;
    c.iceFallout      = sounding.iceFallout;
    c.freezingLevel   = sounding.freezingLevel;
    // Environmental relaxation, and it only ever runs after the scripted arc.
    // Applied from the start it suppresses the storm it is meant to outlive:
    // with it on from t=0 the domain never reached any condensate at all, at
    // every floor tried. That is the same finding Phase 03 made from the other
    // direction, arriving again the moment the term came back.
    float sustaining = 0.0f;
    if (arc.sustainRelaxation > 0.0f)
        sustaining = SmoothStep(arc.duration(), arc.duration() + 400.0f, simulatedTime);
    c.envRelaxation   = sounding.envRelaxation + arc.sustainRelaxation * sustaining;
    // The lid, and where it was a step ago. The difference between the two is
    // added to every cell's potential temperature, which moves the environment
    // without disturbing a single parcel's buoyancy - see CSDamp.
    const float previously = (std::max)(simulatedTime - stepSeconds, 0.0f);
    c.capHeight       = arc.cap(simulatedTime, sounding.equilibrium);
    c.capHeightPrev   = arc.cap(previously, sounding.equilibrium);
    c.capStrength     = arc.strength(simulatedTime, sounding.capStrength);
    c.capStrengthPrev = arc.strength(previously, sounding.capStrength);
    c.capDepth        = sounding.capDepth;
    c.pad1 = 0.0f;

    c.lateralMargin = sounding.lateralMargin;
    c.lateralRate   = sounding.lateralRate;
    c.shearTop      = sounding.shearTop;
    for (int i = 0; i < 2; ++i)
    {
        c.shear[i] = sounding.shear[i];
        c.stormMotion[i] = sounding.stormMotion[i];
    }
    c.windRelaxation  = sounding.windRelaxation;
    c.rainFallSpeed   = sounding.rainFallSpeed;
    c.autoThreshold   = sounding.autoThreshold;
    c.autoRate        = sounding.autoRate;
    c.accretionRate   = sounding.accretionRate;
    c.rainEvaporation = sounding.rainEvaporation;
    c.rainOpaque      = sounding.rainOpaque;

    c.rotationSpeed  = sounding.rotationSpeed * arc.rotation(simulatedTime);
    c.rotationRadius = sounding.rotationRadius;
    c.rotationBase   = sounding.rotationBase;
    c.rotationTop    = sounding.rotationTop;
    c.rotationRate   = sounding.rotationRate;
    c.rotationTilt   = sounding.rotationTilt;
    c.pad0[0] = c.pad0[1] = c.pad0[2] = 0.0f;

    // What reads as opaque, as a fraction of the local peak. See sampleDensity
    // for why it has to be local: Phase 02's single constant was tuned against
    // a cumulus that was the only thing in the box, and across a 14 km storm
    // the same value deletes the anvil rather than carving it.
    c.qcRef  = sounding.opaqueFraction;
    c.qcFloor = sounding.opaqueFloor;
}

void Simulation::bindPhase(int simPhase, int jacobiPhase)
{
    PushConstants push = {};
    push.cropScale[0] = push.cropScale[1] = 1.0f;
    push.simPhase = simPhase;
    push.jacobiPhase = jacobiPhase;
    gpu->cmd->SetComputeRoot32BitConstants(kRootPush, sizeof(PushConstants) / 4, &push, 0);
}

void Simulation::barrierUav(ID3D12Resource* r)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = r;
    gpu->cmd->ResourceBarrier(1, &barrier);
}

void Simulation::setSetState(int index, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    if (from == to) return;
    D3D12_RESOURCE_BARRIER barriers[4] = {
        Gpu::transition(velocity[0][index].Get(), from, to),
        Gpu::transition(velocity[1][index].Get(), from, to),
        Gpu::transition(velocity[2][index].Get(), from, to),
        Gpu::transition(scalars[index].Get(), from, to),
    };
    gpu->cmd->ResourceBarrier(4, barriers);
    m_setState[index] = to;
}

// A cheap, well-mixed integer hash, used to pull independent unit values out of
// one seed. Deterministic across runs and machines, which the plan asks for:
// the same seed has to give the same storm.
static float SeedValue(uint32_t seed, uint32_t index)
{
    uint32_t h = seed * 747796405u + index * 2891336453u;
    h = ((h >> ((h >> 28) + 4)) ^ h) * 277803737u;
    h = (h >> 22) ^ h;
    return (float)(h & 0xFFFFFFu) / (float)0xFFFFFF;
}

// Symmetric about zero, so a perturbation is as likely either way.
static float SeedSpread(uint32_t seed, uint32_t index, float amount)
{
    return (SeedValue(seed, index) * 2.0f - 1.0f) * amount;
}

void DeriveStorm(uint32_t seed, Sounding& sounding, StormArc& arc)
{
    sounding = Sounding();
    arc = StormArc();

    // The cloud base. Tight, because it is the most visible single number in
    // the frame and the mixed layer has to stay under it.
    sounding.surfaceRH   += SeedSpread(seed, 1, 0.030f);
    // How deep the storm gets, through two independent routes: where its own
    // neutral level lands, and where the cap is. Phase 03's calibration curve
    // is what says these are the levers.
    sounding.lapseTropo  += SeedSpread(seed, 2, 0.00016f);
    sounding.equilibrium += SeedSpread(seed, 3, 700.0f);
    // How hard it leans, and how hard it spins. Kept in step with each other -
    // Phase 04 measured that shear without rotation tears the storm apart, so
    // a seed that asks for more of one asks for more of the other.
    const float vigour = SeedValue(seed, 4);
    sounding.shear[0]     = 3.3f + vigour * 1.4f;
    sounding.rotationSpeed = 19.0f + vigour * 9.0f;
    // And how wide the tower is, which sets how much of the frame it fills.
    sounding.forceRadius += SeedSpread(seed, 5, 220.0f);
    sounding.capStrength += SeedSpread(seed, 6, 0.9f);
    // Where it stands, so it is not always dead centre.
    sounding.forceOffset[1] += SeedSpread(seed, 7, 1400.0f);

    // And the tornado's own timing. Some storms hold one for a long time and
    // some barely manage it; the difference is most of what makes two runs
    // feel like different weather rather than the same clip.
    arc.tornadoOnset  += SeedSpread(seed, 8, 0.10f);
    arc.tornadoHold    = 300.0f + SeedValue(seed, 9) * 320.0f;
    arc.tornadoDescend = 180.0f + SeedValue(seed, 10) * 110.0f;
}

void Simulation::sustain(float floorFraction)
{
    arc.sustainAmplitude = floorFraction;

    // A fixed rate, deliberately NOT proportional to the amplitude. Scaling the
    // two together was the obvious thing and it is exactly wrong: what a
    // parcel can reach is the heating rate divided by the relaxation rate, so
    // tying them makes that ratio constant and raising the floor buys nothing
    // at all. Measured, it did precisely that - 0.20, 0.35 and 0.50 all gave
    // the same empty sky. The amplitude is the knob; this is the environment
    // coming back underneath it, at about a 28 minute e-folding of storm time.
    arc.sustainRelaxation = (floorFraction > 0.0f) ? 0.0015f : 0.0f;

    // And a shallower, moister boundary layer, which is what finally made this
    // work. The shipped sounding puts the condensation level at 948 m against
    // a 900 m mixed layer - a margin of one cell, thin by construction because
    // that is what makes the cloud base flat. A thermal in a domain a storm has
    // already worked over arrives there with slightly too little vapour, and
    // the cross-section showed exactly that: a warm plume rising to the
    // condensation level and stopping on it.
    //
    // Simply adding humidity does not work, and fails in the way Phase 03
    // warned it would. The condensation level is -satScale * ln(surfaceRH), so
    // raising humidity lowers it, and once it drops below the mixed layer the
    // environment saturates on its own: at +0.045 the domain grew a flat
    // stratus sheet at 855 m, 5,000 cells of it, that simply sat there.
    //
    // The two have to move together. A shallower mixed layer permits a higher
    // humidity while keeping the condensation level at its top rather than
    // inside it, which lowers the bar a thermal has to clear by 250 m and
    // leaves the environment sub-saturated. It is also what the ground under a
    // storm actually looks like afterwards: cooler, damper, less deeply mixed.

}

void Simulation::restart(uint32_t nextSeed)
{
    seed = nextSeed;
    DeriveStorm(seed, sounding, arc);
    reset();
}

bool Simulation::finished() const
{
    // A little past the arc, so the sky is empty rather than merely fading
    // when the cut happens.
    return simulatedTime > arc.duration() + 320.0f;
}

void Simulation::reset()
{
    simulatedTime = 0.0f;
    accumulator = 0.0f;
    phase = 0;
    initialised = false;
}

void Simulation::step(int constantSlot)
{
    const UINT groupsX = (resolution[0] + 7) / 8;
    const UINT slicesZ = resolution[2];
    // Dispatched over the taller velocity grid so the vertical component's top
    // face is covered; passes that only touch cells guard on it themselves.
    const UINT groupsYVelocity = (resolution[1] + 1 + 7) / 8;
    const UINT groupsYCells = (resolution[1] + 7) / 8;

    const int src = phase;
    const int dst = phase ^ 1;

    // Each step reads the slot written for its own moment of storm time.
    gpu->cmd->SetComputeRootConstantBufferView(
        kRootSim, constantBuffer->GetGPUVirtualAddress()
                + (UINT64)constantSlot * constantStride);

    // Advection reads the source filterable and writes the destination.
    setSetState(src, m_setState[src], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    setSetState(dst, m_setState[dst], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    bindPhase(src, 0);

    gpu->cmd->SetPipelineState(psoAdvect.Get());
    gpu->cmd->Dispatch(groupsX, groupsYVelocity, slicesZ);
    barrierUav(scalars[dst].Get());

    gpu->cmd->SetPipelineState(psoForces.Get());
    gpu->cmd->Dispatch(groupsX, groupsYCells, slicesZ);
    barrierUav(scalars[dst].Get());

    gpu->cmd->SetPipelineState(psoBuoyancy.Get());
    gpu->cmd->Dispatch(groupsX, groupsYVelocity, slicesZ);
    barrierUav(velocity[1][dst].Get());

    gpu->cmd->SetPipelineState(psoMicrophysics.Get());
    gpu->cmd->Dispatch(groupsX, groupsYCells, slicesZ);
    barrierUav(scalars[dst].Get());

    // The lateral margin. Runs over the velocity grid because it damps all
    // three components as well as the scalars: an outflow that is absorbed in
    // the scalars but not in the wind carrying them just refills the margin.
    gpu->cmd->SetPipelineState(psoDamp.Get());
    gpu->cmd->Dispatch(groupsX, groupsYVelocity, slicesZ);
    barrierUav(scalars[dst].Get());

    gpu->cmd->SetPipelineState(psoDivergence.Get());
    gpu->cmd->Dispatch(groupsX, groupsYCells, slicesZ);
    barrierUav(divergence.Get());

    // Jacobi, ping-ponging between the two pressure buffers.
    gpu->cmd->SetPipelineState(psoJacobi.Get());
    for (int i = 0; i < jacobiIterations; ++i)
    {
        bindPhase(src, i & 1);
        gpu->cmd->Dispatch(groupsX, groupsYCells, slicesZ);
        barrierUav(pressure[(i & 1) ^ 1].Get());
    }

    // The last iteration wrote pressure[(iterations-1 & 1) ^ 1].
    const int finalPressure = ((jacobiIterations - 1) & 1) ^ 1;
    bindPhase(src, finalPressure);
    gpu->cmd->SetPipelineState(psoProject.Get());
    gpu->cmd->Dispatch(groupsX, groupsYVelocity, slicesZ);

    phase ^= 1;
    simulatedTime += stepSeconds;
}

bool Simulation::advance(float elapsedSeconds)
{
    gpu->cmd->SetComputeRootSignature(m_rootSignature);
    gpu->cmd->SetComputeRootDescriptorTable(kRootTable, gpu->srvHeap.gpu(0));
    gpu->cmd->SetComputeRootConstantBufferView(kRootSim, constantsAddress());

    writeConstants(0, simulatedTime);

    bool changed = false;

    if (!initialised)
    {
        setSetState(0, m_setState[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setSetState(1, m_setState[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bindPhase(0, 0);
        gpu->cmd->SetPipelineState(psoInitialise.Get());
        gpu->cmd->Dispatch((resolution[0] + 7) / 8, (resolution[1] + 1 + 7) / 8, resolution[2]);
        barrierUav(scalars[0].Get());
        initialised = true;
        changed = true;
    }

    accumulator += elapsedSeconds;
    const float interval = 1.0f / (float)stepsPerSecond;

    // Cap the catch-up. If the process was suspended - which for a screensaver
    // means the machine slept - replaying minutes of simulation in one frame
    // would stall for seconds.
    int budget = kMaxStepsPerFrame;
    while (accumulator >= interval && budget > 0)
    {
        accumulator -= interval;
        const int slot = kConstantSlots - budget;   // 1, then 2, then 3
        --budget;
        writeConstants(slot, simulatedTime);
        step(slot);
        changed = true;
    }
    if (accumulator > interval) accumulator = interval;

    // Slot 0 is what the render passes read, so it has to describe the state
    // the steps above left behind rather than the one they started from.
    if (changed) writeConstants(0, simulatedTime);

    // Restore the binding for whatever runs next in this command list.
    gpu->cmd->SetComputeRootConstantBufferView(kRootSim, constantsAddress());
    return changed;
}

void Simulation::transitionForReading()
{
    setSetState(phase, m_setState[phase], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    currentIsReadable = true;
}
