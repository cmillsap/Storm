#include "renderer.h"

#include <algorithm>
#include <cmath>

static const float kPi = 3.14159265f;

// A full sun sweep. Slow enough not to distract, fast enough that the sky is
// visibly different if you glance back a minute later.
static const float kSunCycleSeconds = 300.0f;

bool Renderer::initialise(Gpu& g)
{
    gpu = &g;

    // One table covering the shared target as UAV and as SRV, plus the frame
    // constants as root constants and a clamped linear sampler for the blit.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 1;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(FrameConstants) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 1;
    desc.pStaticSamplers = &sampler;
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

    Shader sky  = gpu->compile(L"sky.hlsl",  L"CSSky",         L"cs_6_0");
    Shader vs   = gpu->compile(L"blit.hlsl", L"VSFullscreen",  L"vs_6_0");
    Shader ps   = gpu->compile(L"blit.hlsl", L"PSBlit",        L"ps_6_0");

    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
    cd.pRootSignature = rootSignature.Get();
    cd.CS = sky.bytecode();
    STORM_CHECK(gpu->device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&skyPso)), "sky PSO");

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
    STORM_CHECK(gpu->device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&blitPso)), "blit PSO");

    return true;
}

void Renderer::shutdown()
{
    targets.clear();
    blitPso.Reset();
    skyPso.Reset();
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

    // Guard against a pathological arrangement asking for an enormous target.
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

    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = target.width;
    rd.Height = target.height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;

    if (FAILED(gpu->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                    nullptr, IID_PPV_ARGS(&target.texture))))
        return false;

    target.uavIndex = gpu->srvHeap.allocate();
    target.srvIndex = gpu->srvHeap.allocate();

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    gpu->device->CreateUnorderedAccessView(target.texture.Get(), nullptr, &uav,
                                           gpu->srvHeap.cpu(target.uavIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    gpu->device->CreateShaderResourceView(target.texture.Get(), &srv,
                                          gpu->srvHeap.cpu(target.srvIndex));
    return true;
}

void Renderer::fillConstants(FrameConstants& c, const RenderTarget& target, float t) const
{
    const Camera& cam = target.camera;

    const float cy = std::cos(cam.yaw),   sy = std::sin(cam.yaw);
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);

    c.camPos[0] = cam.position[0];
    c.camPos[1] = cam.position[1];
    c.camPos[2] = cam.position[2];
    c.camForward[0] = sy * cp;  c.camForward[1] = sp;  c.camForward[2] = cy * cp;
    c.camRight[0]   = cy;       c.camRight[1]   = 0.0f; c.camRight[2]  = -sy;
    c.camUp[0]      = -sy * sp; c.camUp[1]      = cp;   c.camUp[2]     = -cy * sp;

    c.time = t;
    c.tanHalfFov = std::tan(0.5f * cam.fovDegrees * kPi / 180.0f);
    c.aspect = target.aspect();
    c.exposure = 0.5f;
    c.outSize[0] = (float)target.width;
    c.outSize[1] = (float)target.height;
    c.texSize[0] = (float)target.width;
    c.texSize[1] = (float)target.height;

    // The sun sweeps a slow arc between low golden light and mid-morning, and
    // deliberately never sets. Letting it dip below the horizon turns the whole
    // display black for part of every cycle, which reads as a broken or
    // switched-off monitor rather than as night - and the storm this is being
    // built for is a daytime one.
    const float phase = t / kSunCycleSeconds * 2.0f * kPi;
    const float elevation = (24.0f + 18.0f * std::sin(phase)) * kPi / 180.0f;
    const float azimuth = 1.15f + 0.06f * std::sin(phase * 0.5f);

    c.sunDirection[0] = std::sin(azimuth) * std::cos(elevation);
    c.sunDirection[1] = std::sin(elevation);
    c.sunDirection[2] = std::cos(azimuth) * std::cos(elevation);
    c.sunIntensity = 22.0f;

    c.cropScale[0] = c.cropScale[1] = 1.0f;
    c.cropOffset[0] = c.cropOffset[1] = 0.0f;
}

void Renderer::renderTargets(float timeSeconds)
{
    for (RenderTarget& target : targets)
    {
        // The table's two ranges are appended, so the base must be the UAV slot
        // with the SRV immediately after it - which is how createSharedTarget
        // allocates them.
        D3D12_GPU_DESCRIPTOR_HANDLE table = gpu->srvHeap.gpu(target.uavIndex);

        // The camera drifts slowly. With the sun on its own cycle this gives
        // the idle scene two independent rhythms, which reads as less
        // mechanical than either alone.
        target.camera.yaw = timeSeconds * 0.0075f;

        FrameConstants constants = {};
        fillConstants(constants, target, timeSeconds);

        gpu->cmd->SetComputeRootSignature(rootSignature.Get());
        gpu->cmd->SetComputeRootDescriptorTable(0, table);
        gpu->cmd->SetComputeRoot32BitConstants(1, sizeof(FrameConstants) / 4, &constants, 0);
        gpu->cmd->SetPipelineState(skyPso.Get());
        gpu->cmd->Dispatch((target.width + 7) / 8, (target.height + 7) / 8, 1);
    }

    // One barrier for every target, then read them all from the blits.
    for (RenderTarget& target : targets)
    {
        auto toRead = Gpu::transition(target.texture.Get(),
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gpu->cmd->ResourceBarrier(1, &toRead);
    }
}

void Renderer::presentView(View& view, float timeSeconds)
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

    FrameConstants constants = {};
    fillConstants(constants, *view.source, timeSeconds);
    view.cropToFill(constants.cropScale, constants.cropOffset);

    gpu->cmd->SetGraphicsRootSignature(rootSignature.Get());
    gpu->cmd->SetGraphicsRootDescriptorTable(0, gpu->srvHeap.gpu(view.source->uavIndex));
    gpu->cmd->SetGraphicsRoot32BitConstants(1, sizeof(FrameConstants) / 4, &constants, 0);
    gpu->cmd->SetPipelineState(blitPso.Get());
    gpu->cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu->cmd->DrawInstanced(3, 1, 0, 0);

    auto toPresent = Gpu::transition(view.backBuffers[back].Get(),
                                     D3D12_RESOURCE_STATE_RENDER_TARGET,
                                     D3D12_RESOURCE_STATE_PRESENT);
    gpu->cmd->ResourceBarrier(1, &toPresent);
}

void Renderer::finishFrame()
{
    // Every view has now sampled the targets, so hand them back to the compute
    // pass. Without this the next frame's dispatch writes to a resource still
    // in PIXEL_SHADER_RESOURCE state.
    for (RenderTarget& target : targets)
    {
        auto toWrite = Gpu::transition(target.texture.Get(),
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        gpu->cmd->ResourceBarrier(1, &toWrite);
    }
}
