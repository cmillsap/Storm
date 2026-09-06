#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static const float kPi = 3.14159265f;

// A full sun sweep. Slow enough not to distract, fast enough that the sky is
// visibly different if you glance back a minute later. It deliberately never
// sets: letting it drop below the horizon turns the display black for part of
// every cycle, which reads as a switched-off monitor.
static const float kSunCycleSeconds = 300.0f;

static const UINT kBaseNoiseRes   = 128;
static const UINT kDetailNoiseRes = 32;
// Matched against the simulation cell size: at 64 the light volume was
// coarser than the density it shades and left visible facets. Raised again in
// Phase 03 - the domain went from a 6.4 km cube to 15.8 x 14.4 x 15.8 km, so
// 96 cells that used to be 67 m across would now be 165 m, coarser than the
// cell size the density itself is stored at.
static const UINT kLightVolumeRes = 128;

// The cloud march is the expensive pass and it is the one the temporal resolve
// exists to amortise, so it runs at half the output resolution.
static const UINT kResolutionDivisor = 2;

// ---- lightning ------------------------------------------------------------
//
// A flash is scheduled in wall time, not storm time. The storm runs twenty
// times faster than the weather, and a flash that lasted a fifth of a storm
// second would be gone inside one displayed frame; what the eye has to read is
// a fifth of a *second*, so the schedule lives on the display's clock and only
// its rate is taken from where the storm has got to.
static const float kFlashSlotSeconds = 1.15f;

// Three strokes with a decaying envelope, which is what separates lightning
// from a lamp being switched on: a single exponential reads as a camera flash.
static float FlashEnvelope(float since)
{
    float e = std::exp(-since * 24.0f);
    if (since > 0.075f) e += 0.72f * std::exp(-(since - 0.075f) * 28.0f);
    if (since > 0.160f) e += 0.44f * std::exp(-(since - 0.160f) * 34.0f);
    return e;
}

static float FlashHash(int slot, int salt)
{
    uint32_t h = (uint32_t)slot * 374761393u + (uint32_t)salt * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}

// Halton(2,3), used to jitter the primary ray a sub-pixel amount each frame.
// Accumulated by the resolve, this is what turns a cheap march into a clean
// image rather than a noisy one.
static float halton(int index, int base)
{
    float result = 0.0f, f = 1.0f / (float)base;
    for (int i = index; i > 0; i /= base) { result += f * (float)(i % base); f /= (float)base; }
    return result;
}

// -------------------------------------------------------------- resources

static ComPtr<ID3D12Resource> CreateTexture(Gpu& gpu, D3D12_RESOURCE_DIMENSION dimension,
                                            DXGI_FORMAT format, UINT64 width, UINT height,
                                            UINT16 depth, const char* what)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = dimension;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = depth;
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

static void MakeUav(Gpu& gpu, ID3D12Resource* r, DXGI_FORMAT format, UINT slot, UINT depth = 0)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = format;
    if (depth > 0)
    {
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uav.Texture3D.WSize = depth;
    }
    else
    {
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    }
    gpu.device->CreateUnorderedAccessView(r, nullptr, &uav, gpu.srvHeap.cpu(slot));
}

static void MakeSrv(Gpu& gpu, ID3D12Resource* r, DXGI_FORMAT format, UINT slot, bool volume)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = format;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (volume) { srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D; srv.Texture3D.MipLevels = 1; }
    else        { srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels = 1; }
    gpu.device->CreateShaderResourceView(r, &srv, gpu.srvHeap.cpu(slot));
}

// ------------------------------------------------------------------- setup

bool Renderer::initialise(Gpu& g)
{
    gpu = &g;

    // The heap layout is fixed, so reserve every slot up front and let the
    // resource creation below fill them in. Shaders name registers directly
    // and the table maps onto them one for one.
    for (UINT i = 0; i < kSlotCount; ++i) gpu->srvHeap.allocate();

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = kUavCount;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = kSrvCount;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[4] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Frame constants outgrew root constants once reprojection and cloud shape
    // arrived, so they live in a real constant buffer.
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 0;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // The crop rectangle differs per monitor within one command list, so it
    // cannot live in the shared buffer.
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[2].Constants.ShaderRegister = 1;
    params[2].Constants.Num32BitValues = sizeof(PushConstants) / 4;
    params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    // Simulation constants, constant for the frame.
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[3].Descriptor.ShaderRegister = 2;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[3] = {};
    for (int i = 0; i < 3; ++i)
    {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    // The simulation is periodic horizontally and rigid at the ground and lid,
    // so its sampler wraps in x and z and clamps in y.
    samplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 4;
    desc.pParameters = params;
    desc.NumStaticSamplers = 3;
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        std::string message = "Root signature serialisation failed.";
        if (error) message += "\n\n" + std::string((const char*)error->GetBufferPointer(),
                                                   error->GetBufferSize());
        FailHard(message.c_str(), hr);
    }
    STORM_CHECK(gpu->device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                 IID_PPV_ARGS(&rootSignature)), "root signature");

    struct { ComPtr<ID3D12PipelineState>* pso; const wchar_t* file; const wchar_t* entry; const char* what; }
    computePasses[] = {
        { &psoGenBase,     L"noise_gen.hlsl", L"CSGenBase",     "noise base PSO" },
        { &psoGenDetail,   L"noise_gen.hlsl", L"CSGenDetail",   "noise detail PSO" },
        { &psoCloudMax,    L"cloud.hlsl",     L"CSCloudMax",    "cloud max PSO" },
        { &psoLightVolume, L"cloud.hlsl",     L"CSLightVolume", "light volume PSO" },
        { &psoCloud,       L"cloud.hlsl",     L"CSCloud",       "cloud PSO" },
        { &psoResolve,     L"resolve.hlsl",   L"CSResolve",     "resolve PSO" },
        { &psoComposite,   L"composite.hlsl", L"CSComposite",   "composite PSO" },
        { &psoSlice,       L"slice.hlsl",     L"CSSlice",       "cross-section PSO" },
    };
    for (auto& pass : computePasses)
    {
        Shader cs = gpu->compile(pass.file, pass.entry, L"cs_6_0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = rootSignature.Get();
        cd.CS = cs.bytecode();
        STORM_CHECK(gpu->device->CreateComputePipelineState(
                        &cd, IID_PPV_ARGS(pass.pso->ReleaseAndGetAddressOf())), pass.what);
    }

    Shader vs = gpu->compile(L"blit.hlsl", L"VSFullscreen", L"vs_6_0");
    Shader ps = gpu->compile(L"blit.hlsl", L"PSBlit", L"ps_6_0");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = {};
    gd.pRootSignature = rootSignature.Get();
    gd.VS = vs.bytecode();
    gd.PS = ps.bytecode();
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    gd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    gd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    gd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    gd.SampleMask = UINT_MAX;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gd.SampleDesc.Count = 1;
    STORM_CHECK(gpu->device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&psoBlit)), "blit PSO");

    // Noise volumes and the light volume do not depend on output size.
    baseNoise = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM,
                              kBaseNoiseRes, kBaseNoiseRes, (UINT16)kBaseNoiseRes, "base noise");
    detailNoise = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                kDetailNoiseRes, kDetailNoiseRes, (UINT16)kDetailNoiseRes, "detail noise");
    lightVolume = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R16_FLOAT,
                                kLightVolumeRes, kLightVolumeRes, (UINT16)kLightVolumeRes, "light volume");

    MakeUav(*gpu, baseNoise.Get(),   DXGI_FORMAT_R8G8B8A8_UNORM, kUavBaseNoise,   kBaseNoiseRes);
    MakeUav(*gpu, detailNoise.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kUavDetailNoise, kDetailNoiseRes);
    MakeUav(*gpu, lightVolume.Get(), DXGI_FORMAT_R16_FLOAT,      kUavLightVolume, kLightVolumeRes);
    MakeSrv(*gpu, baseNoise.Get(),   DXGI_FORMAT_R8G8B8A8_UNORM, kSrvBaseNoise,   true);
    MakeSrv(*gpu, detailNoise.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvDetailNoise, true);
    MakeSrv(*gpu, lightVolume.Get(), DXGI_FORMAT_R16_FLOAT,      kSrvLightVolume, true);

    // One constant buffer, written each frame. Safe without multi-buffering
    // because the frame loop waits for the GPU before reusing it.
    const UINT constantsSize = (sizeof(FrameConstants) + 255) & ~255u;
    D3D12_RESOURCE_DESC cbDesc = {};
    cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    cbDesc.Width = constantsSize;
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
                                                     IID_PPV_ARGS(&constantBuffer)), "frame constants");
    D3D12_RANGE noRead = { 0, 0 };
    STORM_CHECK(constantBuffer->Map(0, &noRead, (void**)&constantsMapped), "map frame constants");

    if (!simulation.create(*gpu, rootSignature.Get())) return false;

    // Sized off the simulation, so it has to come after it. One cell per
    // 4x4x4 block; the domain divides exactly, and CSCloudMax derives the same
    // dimensions from gSimRes rather than being told them.
    for (int i = 0; i < 3; ++i) cloudMaxRes[i] = (simulation.resolution[i] + 3) / 4;
    cloudMax = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R16_FLOAT,
                             cloudMaxRes[0], cloudMaxRes[1], (UINT16)cloudMaxRes[2], "cloud max");
    MakeUav(*gpu, cloudMax.Get(), DXGI_FORMAT_R16_FLOAT, kUavCloudMax, cloudMaxRes[2]);
    MakeSrv(*gpu, cloudMax.Get(), DXGI_FORMAT_R16_FLOAT, kSrvCloudMax, true);
    return true;
}

void Renderer::shutdown()
{
    if (constantBuffer && constantsMapped)
    {
        constantBuffer->Unmap(0, nullptr);
        constantsMapped = nullptr;
    }
    simulation.shutdown();
    targets.clear();
    cloudCurrent.Reset();
    cloudHistory[0].Reset();
    cloudHistory[1].Reset();
    lightVolume.Reset();
    cloudMax.Reset();
    baseNoise.Reset();
    detailNoise.Reset();
    constantBuffer.Reset();
    psoBlit.Reset(); psoComposite.Reset(); psoResolve.Reset(); psoCloud.Reset();
    psoLightVolume.Reset(); psoGenDetail.Reset(); psoGenBase.Reset();
    rootSignature.Reset();
}

void ComputeSharedTargetSize(const std::vector<View>& views, UINT& outWidth, UINT& outHeight)
{
    outWidth = outHeight = 0;
    if (views.empty()) return;

    // The tallest view sets the height and the widest aspect sets the width, so
    // that after cropping every view still has at least its native pixel count.
    // A portrait panel alongside an ultrawide is what makes this necessary: the
    // portrait one needs the height, the ultrawide needs the width, and taking
    // the maximum of each independently satisfies both.
    UINT height = 0;
    float widestAspect = 0.0f;
    for (const View& v : views)
    {
        height = std::max(height, v.height);
        widestAspect = std::max(widestAspect, v.aspect());
    }

    UINT width = (UINT)std::lround((float)height * widestAspect);

    const UINT kMaxDimension = 8192;
    if (width > kMaxDimension)
    {
        height = (UINT)std::lround((float)height * (float)kMaxDimension / (float)width);
        width = kMaxDimension;
    }

    outWidth = std::max(1u, width);
    outHeight = std::max(1u, height);
}

bool Renderer::createSharedTarget(const std::vector<View>& views)
{
    UINT width = 0, height = 0;
    ComputeSharedTargetSize(views, width, height);
    if (width == 0 || height == 0) return false;
    return createTarget(width, height);
}

bool Renderer::createTarget(UINT width, UINT height)
{
    targets.clear();
    targets.resize(1);
    RenderTarget& target = targets[0];
    target.width = std::max(1u, width);
    target.height = std::max(1u, height);

    target.texture = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                   DXGI_FORMAT_R8G8B8A8_UNORM,
                                   target.width, target.height, 1, "shared target");
    target.uavIndex = kUavSharedTarget;
    target.srvIndex = kSrvSharedTarget;
    MakeUav(*gpu, target.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kUavSharedTarget);
    MakeSrv(*gpu, target.texture.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, kSrvSharedTarget, false);

    return createCloudBuffers(target.width, target.height);
}

bool Renderer::createCloudBuffers(UINT fullWidth, UINT fullHeight)
{
    halfWidth  = std::max(1u, (fullWidth  + kResolutionDivisor - 1) / kResolutionDivisor);
    halfHeight = std::max(1u, (fullHeight + kResolutionDivisor - 1) / kResolutionDivisor);

    // RGBA16F: in-scattered radiance is linear HDR and transmittance needs more
    // than eight bits to accumulate without banding.
    cloudCurrent = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                 DXGI_FORMAT_R16G16B16A16_FLOAT, halfWidth, halfHeight, 1,
                                 "cloud buffer");
    MakeUav(*gpu, cloudCurrent.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUavCloudCurrent);
    MakeSrv(*gpu, cloudCurrent.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvCloudCurrent, false);

    for (int i = 0; i < 2; ++i)
    {
        cloudHistory[i] = CreateTexture(*gpu, D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                        DXGI_FORMAT_R16G16B16A16_FLOAT, halfWidth, halfHeight, 1,
                                        "cloud history");
        MakeUav(*gpu, cloudHistory[i].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUavCloudHistory0 + i);
        MakeSrv(*gpu, cloudHistory[i].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvCloudHistory0 + i, false);
    }

    historyValid = false;
    return true;
}

// ------------------------------------------------------------------ frame

void Renderer::fillConstants(FrameConstants& c, const RenderTarget& target, float t)
{
    const Camera& cam = target.camera;

    const float cy = std::cos(cam.yaw),   sy = std::sin(cam.yaw);
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);

    c.camPos[0] = cam.position[0];
    c.camPos[1] = cam.position[1];
    c.camPos[2] = cam.position[2];
    c.camForward[0] = sy * cp;  c.camForward[1] = sp;   c.camForward[2] = cy * cp;
    c.camRight[0]   = cy;       c.camRight[1]   = 0.0f; c.camRight[2]   = -sy;
    c.camUp[0]      = -sy * sp; c.camUp[1]      = cp;   c.camUp[2]      = -cy * sp;

    c.time = t;
    c.tanHalfFov = std::tan(0.5f * cam.fovDegrees * kPi / 180.0f);
    c.aspect = target.aspect();
    c.exposure = 0.5f;

    for (int i = 0; i < 3; ++i)
    {
        c.prevForward[i] = m_prevForward[i];
        c.prevRight[i]   = m_prevRight[i];
        c.prevUp[i]      = m_prevUp[i];
    }
    c.prevTanHalfFov = m_prevTanHalfFov;
    c.historyValid = historyValid ? 1.0f : 0.0f;
    // How much of the accumulated history survives each frame. High enough to
    // average many jittered samples, low enough that the image still settles
    // within about half a second after a change.
    //
    // Except during a flash. A flash lasts about six frames at 30 fps, and at
    // a 0.90 blend six frames is not enough for it to reach the screen: it
    // arrives dim, smeared, and still fading two flashes later. The trade is
    // more noise for the fifth of a second when nobody is looking at noise.
    c.historyBlend = 0.90f;

    const float phase = t / kSunCycleSeconds * 2.0f * kPi;
    const float elevation = (24.0f + 18.0f * std::sin(phase)) * kPi / 180.0f;
    const float azimuth = 1.15f + 0.06f * std::sin(phase * 0.5f);
    c.sunDirection[0] = std::sin(azimuth) * std::cos(elevation);
    c.sunDirection[1] = std::sin(elevation);
    c.sunDirection[2] = std::cos(azimuth) * std::cos(elevation);
    c.sunIntensity = 22.0f;

    // The cloud volume is the simulation domain. cloudBottom and cloudTop are
    // still used for the height-dependent detail flip and ambient term.
    for (int i = 0; i < 3; ++i) c.cloudCentre[i] = simulation.centre(i);
    c.cloudRadius = simulation.extent(0) * 0.5f;
    c.cloudBottom = simulation.origin[1];
    c.cloudTop = simulation.origin[1] + simulation.extent(1);
    c.coverage = 0.55f;
    c.densityScale = 1.0f;

    c.fullSize[0] = (float)target.width;
    c.fullSize[1] = (float)target.height;
    c.halfSize[0] = (float)halfWidth;
    c.halfSize[1] = (float)halfHeight;

    c.jitter[0] = halton(frameIndex + 1, 2) - 0.5f;
    c.jitter[1] = halton(frameIndex + 1, 3) - 0.5f;
    c.pad0[0] = c.pad0[1] = 0.0f;

    // Sized to the box, not picked. 96 steps across Phase 02's 6.4 km cube was
    // a 67 m step; the same count across the Phase 03 domain would be 210 m,
    // which at an extinction of 0.055 per metre is an optical depth of 11 in a
    // single sample and lays down visible shells. This holds the step near
    // 70 m for a ray crossing the box corner to corner.
    // Lightning. The rate follows the arc rather than the condensate field,
    // because reading the condensate field back would cost a fence: the storm
    // is electrified once the tower is deep and stops being so as it decays,
    // and the arc already knows both of those without asking the GPU.
    {
        const StormArc& arc = simulation.arc;
        const float storm = simulation.simulatedTime;
        const float ending = arc.mature + arc.sustain + arc.decay * 0.5f;
        const float electrified =
            std::min(std::max((storm - arc.congestus) / (arc.mature - arc.congestus), 0.0f), 1.0f)
          * std::min(std::max((ending - storm) / (arc.decay * 0.5f), 0.0f), 1.0f);

        const int   slot  = (int)std::floor(t / kFlashSlotSeconds);
        const float since = t - (float)slot * kFlashSlotSeconds;

        float intensity = 0.0f;
        if (FlashHash(slot, 0) < electrified * 0.85f)
            intensity = FlashEnvelope(since) * (0.55f + 0.45f * FlashHash(slot, 1)) * electrified;

        // Inside the cloud, below the glaciation level - lightning comes from
        // the mixed-phase region, and putting it in the anvil lights the wrong
        // part of the storm.
        const float spread = simulation.sounding.forceRadius * 1.8f;
        c.flashPosition[0] = simulation.centre(0) + simulation.sounding.forceOffset[0]
                           + (FlashHash(slot, 2) - 0.3f) * spread * 2.0f;
        c.flashPosition[1] = 2200.0f + FlashHash(slot, 3) * 3600.0f;
        c.flashPosition[2] = simulation.centre(2) + (FlashHash(slot, 4) - 0.5f) * spread;
        c.flashIntensity = intensity;

        // Let the resolve respond. Set above, and overridden here because the
        // flash is not known until now.
        if (intensity > 0.02f) c.historyBlend = 0.55f;
    }

    // ---- the tornado
    //
    // Positioned on the mesocyclone axis at the cloud base, which is where a
    // tornado hangs from, and given its swirl on the display's clock: the
    // storm runs twenty times faster than the weather, and a funnel spun at
    // storm rate is a blur.
    {
        const Sounding& sounding = simulation.sounding;
        const StormArc& arc = simulation.arc;
        const float storm = simulation.simulatedTime;

        const float intensity = arc.funnelIntensity(storm);
        const float descent   = arc.funnelDescent(storm);

        // The funnel hangs from the cloud base, on the rotation axis, and the
        // axis leans with the storm - so the tornado is displaced downstream
        // of the surface forcing by however far the tower has leaned.
        const float base = 1035.0f;   // the measured cloud base
        c.tornadoAxis[0] = simulation.centre(0) + sounding.forceOffset[0]
                         + sounding.rotationTilt * sounding.rotationBase;
        c.tornadoAxis[1] = simulation.origin[1];
        c.tornadoAxis[2] = simulation.centre(2) + sounding.forceOffset[1];
        c.tornadoTilt    = sounding.rotationTilt * 0.5f;
        c.tornadoTop     = base;
        c.tornadoDescent = descent;
        c.tornadoRadius  = 215.0f;
        c.tornadoIntensity = intensity;

        // Roughly one turn a second on screen. Faster reads as a special
        // effect; slower does not read as a tornado at all.
        c.tornadoSwirl = t * 6.0f;

        c.debrisHeight = 170.0f * descent;
        c.debrisRadius = 360.0f;

        // The wall cloud: a lowered collar the funnel hangs out of, rather
        // than a funnel emerging from a flat base. It arrives before the
        // funnel does, which is the order a storm does it in.
        const float walling = arc.rotation(storm);
        c.wallRadius = 760.0f * walling;
        c.wallDrop   = 540.0f * walling;

        // The rear-flank clear slot. Authored, and it has to be: Spike 04
        // measured imposed swirl producing a rotating updraft and none of the
        // asymmetry that goes with one. Without it the funnel hangs inside an
        // opaque rain base 18 km away and is not visible at all - which is
        // exactly what the first tornado did.
        c.slotAzimuth  = 2.60f;              // radians, toward the camera's left
        c.slotWidth    = 1.45f;
        c.slotRadius   = 5200.0f;
        c.slotTop      = 3800.0f;
        c.slotStrength = walling;
        c.pad1[0] = c.pad1[1] = 0.0f;
    }

    c.numSteps = 256;
    c.frameIndex = frameIndex;
    c.historyIndex = historyIndex;
    c.lightVolumeRes = (int32_t)kLightVolumeRes;
}

void Renderer::generateNoise()
{
    if (noiseReady) return;

    gpu->beginFrame();
    gpu->cmd->SetComputeRootSignature(rootSignature.Get());
    gpu->cmd->SetComputeRootDescriptorTable(kRootTable, gpu->srvHeap.gpu(0));
    gpu->cmd->SetComputeRootConstantBufferView(kRootFrame, constantBuffer->GetGPUVirtualAddress());
    gpu->cmd->SetComputeRootConstantBufferView(kRootSim, simulation.constantsAddress());

    PushConstants push = {};
    push.cropScale[0] = push.cropScale[1] = 1.0f;
    gpu->cmd->SetComputeRoot32BitConstants(kRootPush, sizeof(PushConstants) / 4, &push, 0);

    gpu->cmd->SetPipelineState(psoGenBase.Get());
    gpu->cmd->Dispatch(kBaseNoiseRes / 4, kBaseNoiseRes / 4, kBaseNoiseRes / 4);
    gpu->cmd->SetPipelineState(psoGenDetail.Get());
    gpu->cmd->Dispatch(kDetailNoiseRes / 4, kDetailNoiseRes / 4, kDetailNoiseRes / 4);

    D3D12_RESOURCE_BARRIER barriers[] = {
        Gpu::transition(baseNoise.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Gpu::transition(detailNoise.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    gpu->cmd->ResourceBarrier(2, barriers);
    gpu->submitAndWait();

    noiseReady = true;
}

void Renderer::renderTargets(float timeSeconds, float deltaSeconds)
{
    // The simulation owns the density the cloud pass reads, so it runs first.
    // It steps at its own rate and reports whether the volume actually changed.
    const bool simulationStepped = simulation.advance(deltaSeconds);
    simulation.transitionForReading();

    for (RenderTarget& target : targets)
    {
        // A slow oscillation rather than a continuous sweep, so the cloud stays
        // framed. Combined with the sun's own cycle this gives the idle scene
        // two independent rhythms, and it is what exercises the reprojection.
        target.camera.yaw = target.camera.baseYaw + 0.09f * std::sin(timeSeconds * 0.021f);

        FrameConstants constants = {};
        fillConstants(constants, target, timeSeconds);
        std::memcpy(constantsMapped, &constants, sizeof(constants));

        gpu->cmd->SetComputeRootSignature(rootSignature.Get());
        gpu->cmd->SetComputeRootDescriptorTable(kRootTable, gpu->srvHeap.gpu(0));
        gpu->cmd->SetComputeRootConstantBufferView(kRootFrame, constantBuffer->GetGPUVirtualAddress());
        gpu->cmd->SetComputeRootConstantBufferView(kRootSim, simulation.constantsAddress());

        PushConstants push = {};
        push.cropScale[0] = push.cropScale[1] = 1.0f;
        push.simPhase = simulation.phase;
        gpu->cmd->SetComputeRoot32BitConstants(kRootPush, sizeof(PushConstants) / 4, &push, 0);

        // 1. The coarse peak, then sun transmittance, both rebuilt only when
        //    the volume moved. This is the plan's amortisation: lighting at
        //    simulation rate, not frame rate. The order matters - every
        //    density sample the light volume takes reads the coarse peak as
        //    its reference, so the peak has to be current first.
        if (simulationStepped || !lightVolumeReady)
        {
            if (lightVolumeReady)
            {
                D3D12_RESOURCE_BARRIER toWrite[] = {
                    Gpu::transition(lightVolume.Get(),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                    Gpu::transition(cloudMax.Get(),
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                };
                gpu->cmd->ResourceBarrier(2, toWrite);
            }

            gpu->cmd->SetPipelineState(psoCloudMax.Get());
            gpu->cmd->Dispatch((cloudMaxRes[0] + 3) / 4, (cloudMaxRes[1] + 3) / 4,
                               (cloudMaxRes[2] + 3) / 4);

            auto peakToRead = Gpu::transition(cloudMax.Get(),
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gpu->cmd->ResourceBarrier(1, &peakToRead);

            gpu->cmd->SetPipelineState(psoLightVolume.Get());
            gpu->cmd->Dispatch(kLightVolumeRes / 4, kLightVolumeRes / 4, kLightVolumeRes / 4);

            auto toRead = Gpu::transition(lightVolume.Get(),
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            gpu->cmd->ResourceBarrier(1, &toRead);
            lightVolumeReady = true;
        }

        // 2. The cloud march, at half resolution.
        gpu->cmd->SetPipelineState(psoCloud.Get());
        gpu->cmd->Dispatch((halfWidth + 7) / 8, (halfHeight + 7) / 8, 1);

        D3D12_RESOURCE_BARRIER cloudWritten = {};
        cloudWritten.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cloudWritten.UAV.pResource = cloudCurrent.Get();
        gpu->cmd->ResourceBarrier(1, &cloudWritten);

        // 3. Reproject and accumulate. Reads last frame's history as an SRV.
        auto prevToRead = Gpu::transition(cloudHistory[historyIndex ^ 1].Get(),
                                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu->cmd->ResourceBarrier(1, &prevToRead);

        gpu->cmd->SetPipelineState(psoResolve.Get());
        gpu->cmd->Dispatch((halfWidth + 7) / 8, (halfHeight + 7) / 8, 1);

        // 4. Composite at full resolution. Reads the buffer just resolved.
        auto resolvedToRead = Gpu::transition(cloudHistory[historyIndex].Get(),
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu->cmd->ResourceBarrier(1, &resolvedToRead);

        gpu->cmd->SetPipelineState(psoComposite.Get());
        gpu->cmd->Dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);

        // Hand the cloud buffers back for the next frame. The light volume and
        // the simulation stay readable; they are rebuilt on their own schedule.
        D3D12_RESOURCE_BARRIER restore[] = {
            Gpu::transition(cloudHistory[historyIndex ^ 1].Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Gpu::transition(cloudHistory[historyIndex].Get(),
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Gpu::transition(target.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        };
        gpu->cmd->ResourceBarrier(3, restore);

        for (int i = 0; i < 3; ++i)
        {
            m_prevForward[i] = constants.camForward[i];
            m_prevRight[i]   = constants.camRight[i];
            m_prevUp[i]      = constants.camUp[i];
        }
        m_prevTanHalfFov = constants.tanHalfFov;
    }

    historyIndex ^= 1;
    historyValid = true;
    ++frameIndex;
}

void Renderer::renderCrossSection(float timeSeconds)
{
    simulation.transitionForReading();
    RenderTarget& target = targets[0];

    FrameConstants constants = {};
    fillConstants(constants, target, timeSeconds);
    std::memcpy(constantsMapped, &constants, sizeof(constants));

    gpu->cmd->SetComputeRootSignature(rootSignature.Get());
    gpu->cmd->SetComputeRootDescriptorTable(kRootTable, gpu->srvHeap.gpu(0));
    gpu->cmd->SetComputeRootConstantBufferView(kRootFrame, constantBuffer->GetGPUVirtualAddress());
    gpu->cmd->SetComputeRootConstantBufferView(kRootSim, simulation.constantsAddress());

    PushConstants push = {};
    push.cropScale[0] = push.cropScale[1] = 1.0f;
    push.simPhase = simulation.phase;
    gpu->cmd->SetComputeRoot32BitConstants(kRootPush, sizeof(PushConstants) / 4, &push, 0);

    gpu->cmd->SetPipelineState(psoSlice.Get());
    gpu->cmd->Dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);

    auto toRead = Gpu::transition(target.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    gpu->cmd->ResourceBarrier(1, &toRead);
}

void Renderer::presentView(View& view)
{
    if (!view.source || !view.swapChain) return;

    const UINT back = view.swapChain->GetCurrentBackBufferIndex();

    auto toTarget = Gpu::transition(view.backBuffers[back].Get(),
                                    D3D12_RESOURCE_STATE_PRESENT,
                                    D3D12_RESOURCE_STATE_RENDER_TARGET);
    gpu->cmd->ResourceBarrier(1, &toTarget);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = gpu->rtvHeap.cpu(view.rtvIndex[back]);
    gpu->cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)view.width, (float)view.height, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)view.width, (LONG)view.height };
    gpu->cmd->RSSetViewports(1, &vp);
    gpu->cmd->RSSetScissorRects(1, &scissor);

    PushConstants push = {};
    view.cropToFill(push.cropScale, push.cropOffset);
    push.simPhase = simulation.phase;

    gpu->cmd->SetGraphicsRootSignature(rootSignature.Get());
    gpu->cmd->SetGraphicsRootDescriptorTable(kRootTable, gpu->srvHeap.gpu(0));
    gpu->cmd->SetGraphicsRootConstantBufferView(kRootFrame, constantBuffer->GetGPUVirtualAddress());
    gpu->cmd->SetGraphicsRoot32BitConstants(kRootPush, sizeof(PushConstants) / 4, &push, 0);
    gpu->cmd->SetGraphicsRootConstantBufferView(kRootSim, simulation.constantsAddress());
    gpu->cmd->SetPipelineState(psoBlit.Get());
    gpu->cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu->cmd->DrawInstanced(3, 1, 0, 0);

    auto toPresent = Gpu::transition(view.backBuffers[back].Get(),
                                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_PRESENT);
    gpu->cmd->ResourceBarrier(1, &toPresent);
}

void Renderer::finishFrame()
{
    for (RenderTarget& target : targets)
    {
        auto toWrite = Gpu::transition(target.texture.Get(),
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu->cmd->ResourceBarrier(1, &toWrite);
    }
}
