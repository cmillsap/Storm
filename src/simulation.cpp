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

    const UINT n = resolution;
    // All three velocity components are allocated a row taller than the cell
    // grid so the vertical one has somewhere to put its top face. u and w waste
    // that row, which costs under one percent and keeps every dispatch uniform.
    const UINT vh = n + 1;

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
            velocity[component][set] = CreateVolume(*gpu, DXGI_FORMAT_R16_FLOAT, n, vh, n,
                                                    "simulation velocity");
            MakeVolumeViews(*gpu, velocity[component][set].Get(), DXGI_FORMAT_R16_FLOAT, n,
                            uavSlots[component][set], srvSlots[component][set]);
        }
    }

    for (int set = 0; set < 2; ++set)
    {
        scalars[set] = CreateVolume(*gpu, DXGI_FORMAT_R16G16B16A16_FLOAT, n, n, n,
                                    "simulation scalars");
        MakeVolumeViews(*gpu, scalars[set].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, n,
                        set == 0 ? kUavSimS0 : kUavSimS1,
                        set == 0 ? kSrvSimS0 : kSrvSimS1);

        // The pressure solve wants more precision than the fields it corrects.
        pressure[set] = CreateVolume(*gpu, DXGI_FORMAT_R32_FLOAT, n, n, n, "simulation pressure");
        MakeVolumeViews(*gpu, pressure[set].Get(), DXGI_FORMAT_R32_FLOAT, n,
                        set == 0 ? kUavSimP0 : kUavSimP1, UINT_MAX);
    }

    divergence = CreateVolume(*gpu, DXGI_FORMAT_R32_FLOAT, n, n, n, "simulation divergence");
    MakeVolumeViews(*gpu, divergence.Get(), DXGI_FORMAT_R32_FLOAT, n, kUavSimDiv, UINT_MAX);

    const UINT size = (sizeof(SimConstants) + 255) & ~255u;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = size;
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
        { &psoDivergence,   L"CSDivergence",   "sim divergence PSO" },
        { &psoJacobi,       L"CSJacobi",       "sim jacobi PSO" },
        { &psoProject,      L"CSProject",      "sim project PSO" },
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
    psoProject.Reset(); psoJacobi.Reset(); psoDivergence.Reset(); psoMicrophysics.Reset();
    psoBuoyancy.Reset(); psoForces.Reset(); psoAdvect.Reset(); psoInitialise.Reset();
}

D3D12_GPU_VIRTUAL_ADDRESS Simulation::constantsAddress() const
{
    return constantBuffer->GetGPUVirtualAddress();
}

void Simulation::fillConstants(SimConstants& c) const
{
    c.origin[0] = origin[0]; c.origin[1] = origin[1]; c.origin[2] = origin[2];
    c.cellSize = cellSize;
    c.resolution[0] = c.resolution[1] = c.resolution[2] = (int32_t)resolution;

    c.dt = stepSeconds;
    c.time = simulatedTime;

    // Forcing ramps on, sustains, then decays, so the cloud grows, boils and
    // dissipates rather than standing still. Phase 03 replaces this schedule
    // with the sounding vector that drives the whole storm arc.
    // In storm seconds. The solver's own response time sets these, not taste:
    // a thermal seeded at the ground needs about 450 s to reach its
    // condensation level and build a tower, so a schedule that ramped off at
    // 290 s had the cloud appearing only as the forcing died - it grew and
    // dissipated, but never had a mature phase.
    const float onset = 30.0f, sustain = 500.0f, decay = 400.0f;
    float ramp;
    if (simulatedTime < onset)                     ramp = simulatedTime / onset;
    else if (simulatedTime < onset + sustain)      ramp = 1.0f;
    else                                            ramp = std::max(0.0f, 1.0f - (simulatedTime - onset - sustain) / decay);
    c.forceRamp = ramp;

    // Condensate at which the cloud reads as fully opaque. This has to be set
    // from what the solver actually produces, and it is a two-sided error.
    // Mixed-layer vapour is 0.01512 and saturation at 3 km is 0.0054, so a
    // parcel lifted out of the boundary layer carries about 0.0097 of
    // condensate at the top of the cloud and far less near the base. Set an
    // order of magnitude low, the whole volume saturates to one and the noise
    // erosion has no gradient to bite into - a smooth blob. Set above what the
    // solver produces, the erosion threshold sits above the density everywhere
    // in the lower cloud and eats the flat base entirely, leaving one small
    // puff high up. It belongs just under the peak the solver reaches.
    c.qcRef = 0.0090f;

    c.forceCentre[0] = origin[0] + extent() * 0.5f;
    c.forceCentre[1] = origin[1] + 350.0f;
    c.forceCentre[2] = origin[2] + extent() * 0.5f;
    // Wide and shallow rather than a point source. A narrow plume rises as a
    // single mushroom; a broad heated patch feeds condensation across a whole
    // layer, which is what gives a cumulus its flat base.
    c.forceRadius = 2200.0f;
    // Shallow. A source as deep as it is wide is a ball, and a ball rises as
    // one mushroom - stem, cap and all. Confining the heat to a sheet inside
    // the mixed layer lifts a whole layer at once, which is what puts one flat
    // base under several turrets.
    c.forceDepth = 400.0f;
    // Much weaker than it had to be before the mixed layer existed. Against a
    // 4 K/km lapse all the way to the ground the forcing had to supply the
    // whole 2.7 K a parcel needed to reach its condensation level, and took
    // most of the storm's life doing it; against a neutral boundary layer it
    // only has to keep feeding the thermal.
    c.forceHeat = 0.0100f;
    c.forceMoisture = 2.5e-6f;

    // The boundary layer. Neutral enough that a thermal crosses it for free,
    // and topped just below the condensation level so the environment itself
    // never saturates.
    c.mixedTop = 600.0f;
    c.lapseMixed = 0.0005f;

    // A conditionally unstable troposphere under a strong cap, and moisture as
    // relative humidity against saturation rather than an independent profile.
    // The cap has to sit well inside the domain. At 5.2 km in a 6.4 km box the
    // cloud outgrew the lid and spread along the ceiling instead of topping
    // out: latent heat keeps a plume climbing long after the forcing stops, so
    // what ends the growth is the stable layer, not the forcing schedule.
    c.tropopause = 3200.0f;
    c.lapseTropo = 0.0040f;
    c.lapseStrato = 0.0230f;
    // Mixed-layer vapour is gSurfaceRH * gSatSurface = 0.01512, which saturates
    // at 723 m: the flat base sits just above the mixed layer, where it should.
    c.surfaceRH = 0.72f;
    c.upperRH = 0.30f;
    c.rhTransition = 6000.0f;
    c.satSurface = 0.0210f;
    c.satScale = 2200.0f;
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

void Simulation::reset()
{
    simulatedTime = 0.0f;
    accumulator = 0.0f;
    phase = 0;
    initialised = false;
}

void Simulation::step()
{
    const UINT n = resolution;
    const UINT groupsXZ = (n + 7) / 8;
    // Dispatched over the taller velocity grid so the vertical component's top
    // face is covered; passes that only touch cells guard on it themselves.
    const UINT slicesVelocity = n + 1;
    const UINT slicesCells = n;

    const int src = phase;
    const int dst = phase ^ 1;

    // Advection reads the source filterable and writes the destination.
    setSetState(src, m_setState[src], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    setSetState(dst, m_setState[dst], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    bindPhase(src, 0);

    gpu->cmd->SetPipelineState(psoAdvect.Get());
    gpu->cmd->Dispatch(groupsXZ, (slicesVelocity + 7) / 8, n);
    barrierUav(scalars[dst].Get());

    gpu->cmd->SetPipelineState(psoForces.Get());
    gpu->cmd->Dispatch(groupsXZ, (slicesCells + 7) / 8, n);
    barrierUav(scalars[dst].Get());

    gpu->cmd->SetPipelineState(psoBuoyancy.Get());
    gpu->cmd->Dispatch(groupsXZ, (slicesVelocity + 7) / 8, n);
    barrierUav(velocity[1][dst].Get());

    gpu->cmd->SetPipelineState(psoMicrophysics.Get());
    gpu->cmd->Dispatch(groupsXZ, (slicesCells + 7) / 8, n);
    barrierUav(scalars[dst].Get());

    gpu->cmd->SetPipelineState(psoDivergence.Get());
    gpu->cmd->Dispatch(groupsXZ, (slicesCells + 7) / 8, n);
    barrierUav(divergence.Get());

    // Jacobi, ping-ponging between the two pressure buffers.
    gpu->cmd->SetPipelineState(psoJacobi.Get());
    for (int i = 0; i < jacobiIterations; ++i)
    {
        bindPhase(src, i & 1);
        gpu->cmd->Dispatch(groupsXZ, (slicesCells + 7) / 8, n);
        barrierUav(pressure[(i & 1) ^ 1].Get());
    }

    // The last iteration wrote pressure[(iterations-1 & 1) ^ 1].
    const int finalPressure = ((jacobiIterations - 1) & 1) ^ 1;
    bindPhase(src, finalPressure);
    gpu->cmd->SetPipelineState(psoProject.Get());
    gpu->cmd->Dispatch(groupsXZ, (slicesVelocity + 7) / 8, n);

    phase ^= 1;
    simulatedTime += stepSeconds;
}

bool Simulation::advance(float elapsedSeconds)
{
    gpu->cmd->SetComputeRootSignature(m_rootSignature);
    gpu->cmd->SetComputeRootDescriptorTable(kRootTable, gpu->srvHeap.gpu(0));
    gpu->cmd->SetComputeRootConstantBufferView(kRootSim, constantsAddress());

    SimConstants constants = {};
    fillConstants(constants);
    std::memcpy(constantsMapped, &constants, sizeof(constants));

    bool changed = false;

    if (!initialised)
    {
        setSetState(0, m_setState[0], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        setSetState(1, m_setState[1], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        bindPhase(0, 0);
        gpu->cmd->SetPipelineState(psoInitialise.Get());
        gpu->cmd->Dispatch((resolution + 7) / 8, (resolution + 8) / 8, resolution);
        barrierUav(scalars[0].Get());
        initialised = true;
        changed = true;
    }

    accumulator += elapsedSeconds;
    const float interval = 1.0f / (float)stepsPerSecond;

    // Cap the catch-up. If the process was suspended - which for a screensaver
    // means the machine slept - replaying minutes of simulation in one frame
    // would stall for seconds.
    int budget = 3;
    while (accumulator >= interval && budget-- > 0)
    {
        accumulator -= interval;
        // Constants carry the forcing ramp, which changes with simulated time,
        // so refresh them between steps.
        fillConstants(constants);
        std::memcpy(constantsMapped, &constants, sizeof(constants));
        step();
        changed = true;
    }
    if (accumulator > interval) accumulator = interval;

    return changed;
}

void Simulation::transitionForReading()
{
    setSetState(phase, m_setState[phase], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    currentIsReadable = true;
}
