// Storm - Spike 01: what does a raymarch step actually cost?
//
// Answers the question the feasibility plan left open: is an 8-15 ms frame at
// 3440x1440 realistic for a volumetric cloud renderer on this hardware?
//
// Method: a compute shader marches a procedural cloud layer into a UAV. GPU
// timestamps bracket that one dispatch and nothing else - no present, no blit,
// no CPU time. Five density-sampler modes and four step counts are swept at two
// resolutions, so the deltas between adjacent configs isolate individual costs.
//
// Usage:  spike01.exe [--list-adapters] [--adapter N] [--frames N]
//                     [--csv path] [--bench-only] [--interactive-only] [--debug]

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <dxcapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- utilities

static void Fail(const char* what, HRESULT hr = S_OK)
{
    if (hr != S_OK) std::fprintf(stderr, "\nFATAL: %s (hr = 0x%08lX)\n", what, (unsigned long)hr);
    else            std::fprintf(stderr, "\nFATAL: %s\n", what);
    std::exit(1);
}

#define HR(expr, what) do { HRESULT _hr = (expr); if (FAILED(_hr)) Fail(what, _hr); } while (0)

static std::string Narrow(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static double Percentile(std::vector<double> v, double p)
{
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    return v[std::min(i, v.size() - 1)];
}

// Locates shaders\ by walking up from the executable, so the shaders can be
// edited and re-run without a rebuild.
static std::wstring FindShaderDir()
{
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t cut = dir.find_last_of(L"\\/");
    dir = (cut == std::wstring::npos) ? L"." : dir.substr(0, cut);

    for (int i = 0; i < 5; ++i)
    {
        std::wstring candidate = dir + L"\\shaders";
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
        size_t p = dir.find_last_of(L"\\/");
        if (p == std::wstring::npos) break;
        dir = dir.substr(0, p);
    }
    Fail("could not locate the shaders directory");
    return {};
}

// ------------------------------------------------------------ shader params

// Mirrors the cbuffer in every shader. 32 root constants, no constant buffer
// to manage. HLSL packs float3+float into one 16-byte row, so the layout below
// is row-for-row identical to the declaration on the GPU side.
struct alignas(16) Params
{
    float   camPos[3];     float time;
    float   camFwd[3];     float tanHalfFov;
    float   camRight[3];   float aspect;
    float   camUp[3];      float coverage;
    float   outSize[2];    float texSize[2];
    int32_t numSteps;      int32_t mode;       int32_t lightSteps;  int32_t flags;
    float   densityScale;  float cloudBottom;  float cloudTop;      float edgeSoftness;
    float   sunDir[3];     float sigmaT;
    float   boxCentre[3];  float boxHalfXZ;
};
static_assert(sizeof(Params) == 144, "Params must be exactly 36 root constants");

// --------------------------------------------------------------- benchmark

// FLAG_NO_EARLY_OUT must match the constant in raymarch.hlsl.
static const int kFlagNoEarlyOut    = 1;
static const int kFlagLightBaseOnly = 2;

// framing 0: storm seen whole from ~6.5 km, the way the screensaver frames it.
// framing 1: camera pressed against the near face so the volume covers every
//            pixel. No ray misses, so this is the true upper bound on cost.
enum Framing { kFramingStandard = 0, kFramingFillFrame = 1 };

struct Config
{
    int mode;
    int steps;
    int width;
    int height;
    int flags = 0;
    int framing = kFramingStandard;
    int lightSteps = 6;
};

struct Result
{
    Config config;
    double mean, minimum, median, p95;
    double buildMedian;      // light-volume build, 0 when the config has none
};

// GPU time for the two separately-bracketed dispatches in a frame.
struct FrameTimes
{
    double buildMs;
    double marchMs;
};

static const char* kModeNames[] =
{
    "analytic (no texture)",
    "base only (1 sample)",
    "base + detail (2 samples)",
    "adaptive (empty-space skip)",
    "adaptive + 6-tap light march",
    "adaptive + light volume fetch",
};
static const int kModeCount = 5;      // modes swept by the main table
static const int kModeLightVolume = 5;

// ------------------------------------------------------------------ globals

static const UINT   kBaseRes     = 128;
static const UINT   kDetailRes   = 32;
static const UINT   kMaxWidth    = 3440;
static const UINT   kMaxHeight   = 1440;
static const UINT   kWindowWidth = 1280;
static const UINT   kWindowHeight= 536;    // matches the 3440x1440 aspect
static const UINT   kBackBuffers = 2;

// Descriptor heap layout (one shader-visible CBV/SRV/UAV heap).
enum Descriptor
{
    kUavOutput = 0,     // u0
    kUavBase,           // u1
    kUavDetail,         // u2
    kUavLightVolume,    // u3
    kSrvBase,           // t0
    kSrvDetail,         // t1
    kSrvOutput,         // t2
    kSrvLightVolume,    // t3
    kDescriptorCount
};

static const UINT kLightVolumeX = 64;
static const UINT kLightVolumeY = 48;
static const UINT kLightVolumeZ = 64;

struct App
{
    HWND                        hwnd = nullptr;
    ComPtr<IDXGIFactory6>       factory;
    ComPtr<ID3D12Device>        device;
    ComPtr<ID3D12CommandQueue>  queue;
    ComPtr<IDXGISwapChain3>     swapChain;
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;

    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT srvStride = 0, rtvStride = 0;

    ComPtr<ID3D12Resource> backBuffers[kBackBuffers];
    ComPtr<ID3D12Resource> outputTex;
    ComPtr<ID3D12Resource> baseNoise;
    ComPtr<ID3D12Resource> detailNoise;
    ComPtr<ID3D12Resource> lightVolume;

    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoGenBase, psoGenDetail, psoRaymarch, psoBlit, psoLightVolume;

    ComPtr<ID3D12QueryHeap> queryHeap;
    ComPtr<ID3D12Resource>  queryReadback;
    UINT64 timestampFrequency = 1;

    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    std::wstring shaderDir;
    std::string  adapterName;
    bool         windowClosed = false;
};

static App g;

// ------------------------------------------------------------ DXC front end

static DxcCreateInstanceProc gDxcCreateInstance = nullptr;

static void InitDxc()
{
    HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
    if (!dll) Fail("dxcompiler.dll not found - it must sit beside the executable");
    gDxcCreateInstance = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
    if (!gDxcCreateInstance) Fail("DxcCreateInstance missing from dxcompiler.dll");
}

static ComPtr<IDxcBlob> Compile(const wchar_t* file, const wchar_t* entry, const wchar_t* target)
{
    ComPtr<IDxcUtils>          utils;
    ComPtr<IDxcCompiler3>      compiler;
    ComPtr<IDxcIncludeHandler> includes;
    HR(gDxcCreateInstance(CLSID_DxcUtils,    IID_PPV_ARGS(&utils)),    "DxcUtils");
    HR(gDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCompiler");
    HR(utils->CreateDefaultIncludeHandler(&includes), "include handler");

    std::wstring path = g.shaderDir + L"\\" + file;
    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(path.c_str(), nullptr, &source)))
        Fail(("could not read shader " + Narrow(path.c_str())).c_str());

    DxcBuffer buffer{ source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };

    const wchar_t* args[] =
    {
        L"-E", entry,
        L"-T", target,
        L"-O3",
        L"-Qstrip_debug",
        L"-Qstrip_reflect",
    };

    ComPtr<IDxcResult> result;
    HR(compiler->Compile(&buffer, args, _countof(args), includes.Get(), IID_PPV_ARGS(&result)),
       "DXC Compile call");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0)
        std::fprintf(stderr, "%s\n", errors->GetStringPointer());

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status))
        Fail(("shader compilation failed: " + Narrow(file) + " / " + Narrow(entry)).c_str());

    ComPtr<IDxcBlob> object;
    HR(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr), "DXC object");
    return object;
}

// ---------------------------------------------------------------- resources

static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES p = {};
    p.Type = type;
    p.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    p.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    p.CreationNodeMask = 1;
    p.VisibleNodeMask = 1;
    return p;
}

static ComPtr<ID3D12Resource> CreateTexture(D3D12_RESOURCE_DIMENSION dim, DXGI_FORMAT fmt,
                                            UINT64 width, UINT height, UINT16 depth,
                                            D3D12_RESOURCE_STATES state, const char* what)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = dim;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    auto props = HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    ComPtr<ID3D12Resource> res;
    HR(g.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, state,
                                         nullptr, IID_PPV_ARGS(&res)), what);
    return res;
}

static D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = g.srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)i * g.srvStride;
    return h;
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,
                                         D3D12_RESOURCE_STATES from,
                                         D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}

// ------------------------------------------------------------------- window

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CLOSE || msg == WM_DESTROY) { g.windowClosed = true; return 0; }
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { g.windowClosed = true; return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void PumpMessages()
{
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// --------------------------------------------------------------------- init

static void SelectAdapter(int requested, bool listOnly)
{
    HR(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory)), "CreateDXGIFactory2");

    struct Candidate { ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 desc; };
    std::vector<Candidate> candidates;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; g.factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter.Reset(); continue; }
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), nullptr)))
        {
            adapter.Reset();
            continue;
        }
        candidates.push_back({ adapter, desc });
        adapter.Reset();
    }

    if (candidates.empty()) Fail("no Direct3D 12 capable adapter found");

    // Default to the most VRAM, which on a multi-GPU machine is the card you
    // actually care about benchmarking.
    int best = 0;
    for (int i = 1; i < (int)candidates.size(); ++i)
        if (candidates[i].desc.DedicatedVideoMemory > candidates[best].desc.DedicatedVideoMemory)
            best = i;

    int chosen = (requested >= 0 && requested < (int)candidates.size()) ? requested : best;

    std::printf("Adapters:\n");
    for (int i = 0; i < (int)candidates.size(); ++i)
    {
        std::printf("  [%d]%s %-34s %5.1f GB\n",
                    i, (i == chosen ? " *" : "  "),
                    Narrow(candidates[i].desc.Description).c_str(),
                    (double)candidates[i].desc.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
    }
    std::printf("\n");

    if (listOnly) std::exit(0);

    g.adapterName = Narrow(candidates[chosen].desc.Description);
    HR(D3D12CreateDevice(candidates[chosen].adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&g.device)), "D3D12CreateDevice");
}

static void CreateRootSignature()
{
    // One table covering every resource: UAVs u0-u2 then SRVs t0-t2, laid out
    // consecutively in the heap. Root constants carry all parameters.
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 4;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 4;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.ShaderRegister = 0;
    params[1].Constants.Num32BitValues = sizeof(Params) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].MipLODBias = 0.0f;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        samplers[i].MinLOD = 0.0f;
        samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
        samplers[i].ShaderRegister = i;
        samplers[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;

    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = params;
    desc.NumStaticSamplers = 2;
    desc.pStaticSamplers = samplers;
    desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> blob, error;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
    if (FAILED(hr))
    {
        if (error) std::fprintf(stderr, "%.*s\n", (int)error->GetBufferSize(), (const char*)error->GetBufferPointer());
        Fail("root signature serialisation", hr);
    }
    HR(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                     IID_PPV_ARGS(&g.rootSig)), "CreateRootSignature");
}

static ComPtr<ID3D12PipelineState> MakeComputePso(IDxcBlob* cs, const char* what)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
    d.pRootSignature = g.rootSig.Get();
    d.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ComPtr<ID3D12PipelineState> pso;
    HR(g.device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)), what);
    return pso;
}

static void CreatePipelines()
{
    CreateRootSignature();

    auto csBase   = Compile(L"noise_gen.hlsl", L"CSGenBase",   L"cs_6_0");
    auto csDetail = Compile(L"noise_gen.hlsl", L"CSGenDetail", L"cs_6_0");
    auto csMarch  = Compile(L"raymarch.hlsl",  L"CSRaymarch",   L"cs_6_0");
    auto csLight  = Compile(L"raymarch.hlsl",  L"CSLightVolume", L"cs_6_0");
    auto vsBlit   = Compile(L"blit.hlsl",      L"VSFullscreen", L"vs_6_0");
    auto psBlit   = Compile(L"blit.hlsl",      L"PSBlit",       L"ps_6_0");

    g.psoGenBase   = MakeComputePso(csBase.Get(),   "noise base PSO");
    g.psoGenDetail = MakeComputePso(csDetail.Get(), "noise detail PSO");
    g.psoRaymarch  = MakeComputePso(csMarch.Get(),  "raymarch PSO");
    g.psoLightVolume = MakeComputePso(csLight.Get(), "light volume PSO");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = {};
    gd.pRootSignature = g.rootSig.Get();
    gd.VS = { vsBlit->GetBufferPointer(), vsBlit->GetBufferSize() };
    gd.PS = { psBlit->GetBufferPointer(), psBlit->GetBufferSize() };
    gd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gd.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    gd.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    gd.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    gd.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    gd.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    gd.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    gd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    gd.SampleMask = UINT_MAX;
    gd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gd.RasterizerState.DepthClipEnable = TRUE;
    gd.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    gd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gd.NumRenderTargets = 1;
    gd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    gd.SampleDesc.Count = 1;
    HR(g.device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&g.psoBlit)), "blit PSO");
}

static void CreateResourcesAndViews()
{
    g.outputTex = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                kMaxWidth, kMaxHeight, 1,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "raymarch target");
    g.baseNoise = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                kBaseRes, kBaseRes, (UINT16)kBaseRes,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "base noise");
    g.detailNoise = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  kDetailRes, kDetailRes, (UINT16)kDetailRes,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "detail noise");
    g.lightVolume = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R16_FLOAT,
                                  kLightVolumeX, kLightVolumeY, (UINT16)kLightVolumeZ,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "light volume");

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(g.outputTex.Get(), nullptr, &uav, SrvCpu(kUavOutput));

    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    uav.Texture3D.WSize = kBaseRes;
    g.device->CreateUnorderedAccessView(g.baseNoise.Get(), nullptr, &uav, SrvCpu(kUavBase));
    uav.Texture3D.WSize = kDetailRes;
    g.device->CreateUnorderedAccessView(g.detailNoise.Get(), nullptr, &uav, SrvCpu(kUavDetail));

    uav.Format = DXGI_FORMAT_R16_FLOAT;
    uav.Texture3D.WSize = kLightVolumeZ;
    g.device->CreateUnorderedAccessView(g.lightVolume.Get(), nullptr, &uav, SrvCpu(kUavLightVolume));

    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = 1;
    g.device->CreateShaderResourceView(g.baseNoise.Get(), &srv, SrvCpu(kSrvBase));
    g.device->CreateShaderResourceView(g.detailNoise.Get(), &srv, SrvCpu(kSrvDetail));

    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    g.device->CreateShaderResourceView(g.outputTex.Get(), &srv, SrvCpu(kSrvOutput));

    srv.Format = DXGI_FORMAT_R16_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    srv.Texture3D.MipLevels = 1;
    g.device->CreateShaderResourceView(g.lightVolume.Get(), &srv, SrvCpu(kSrvLightVolume));
}

static void Init(int adapterIndex, bool listAdapters, bool debugLayer)
{
    g.shaderDir = FindShaderDir();
    InitDxc();

    if (debugLayer)
    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
        std::printf("NOTE: debug layer enabled - timings are NOT representative.\n\n");
    }

    SelectAdapter(adapterIndex, listAdapters);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue");
    HR(g.queue->GetTimestampFrequency(&g.timestampFrequency), "GetTimestampFrequency");

    // Window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"StormSpike01";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, (LONG)kWindowWidth, (LONG)kWindowHeight };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g.hwnd = CreateWindowExW(0, L"StormSpike01", L"Storm - Spike 01: raymarch cost",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g.hwnd) Fail("CreateWindowEx");
    ShowWindow(g.hwnd, SW_SHOW);

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = kWindowWidth;
    sd.Height = kWindowHeight;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBackBuffers;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> sc1;
    HR(g.factory->CreateSwapChainForHwnd(g.queue.Get(), g.hwnd, &sd, nullptr, nullptr, &sc1), "swap chain");
    HR(sc1.As(&g.swapChain), "swap chain QI");
    g.factory->MakeWindowAssociation(g.hwnd, DXGI_MWA_NO_ALT_ENTER);

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kDescriptorCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.srvHeap)), "srv heap");
    g.srvStride = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBackBuffers;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    HR(g.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g.rtvHeap)), "rtv heap");
    g.rtvStride = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < kBackBuffers; ++i)
    {
        HR(g.swapChain->GetBuffer(i, IID_PPV_ARGS(&g.backBuffers[i])), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += (SIZE_T)i * g.rtvStride;
        g.device->CreateRenderTargetView(g.backBuffers[i].Get(), nullptr, h);
    }

    HR(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator)), "allocator");
    HR(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator.Get(),
                                   nullptr, IID_PPV_ARGS(&g.cmd)), "command list");
    HR(g.cmd->Close(), "close command list");

    D3D12_QUERY_HEAP_DESC qhd = {};
    qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qhd.Count = 4;      // [0,1] light-volume build, [2,3] raymarch
    HR(g.device->CreateQueryHeap(&qhd, IID_PPV_ARGS(&g.queryHeap)), "query heap");

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = sizeof(UINT64) * 4;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto rbProps = HeapProps(D3D12_HEAP_TYPE_READBACK);
    HR(g.device->CreateCommittedResource(&rbProps, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&g.queryReadback)), "query readback");

    HR(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "fence");
    g.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    CreateResourcesAndViews();
    CreatePipelines();
}

// ------------------------------------------------------------ frame plumbing

static void BeginCommands()
{
    HR(g.allocator->Reset(), "allocator reset");
    HR(g.cmd->Reset(g.allocator.Get(), nullptr), "command list reset");
    ID3D12DescriptorHeap* heaps[] = { g.srvHeap.Get() };
    g.cmd->SetDescriptorHeaps(1, heaps);
}

static void SubmitAndWait()
{
    HR(g.cmd->Close(), "close");
    ID3D12CommandList* lists[] = { g.cmd.Get() };
    g.queue->ExecuteCommandLists(1, lists);

    const UINT64 target = ++g.fenceValue;
    HR(g.queue->Signal(g.fence.Get(), target), "signal");
    if (g.fence->GetCompletedValue() < target)
    {
        HR(g.fence->SetEventOnCompletion(target, g.fenceEvent), "SetEventOnCompletion");
        WaitForSingleObject(g.fenceEvent, INFINITE);
    }
}

static void GenerateNoise()
{
    BeginCommands();
    g.cmd->SetComputeRootSignature(g.rootSig.Get());
    g.cmd->SetComputeRootDescriptorTable(0, g.srvHeap->GetGPUDescriptorHandleForHeapStart());

    Params p = {};
    g.cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &p, 0);

    g.cmd->SetPipelineState(g.psoGenBase.Get());
    g.cmd->Dispatch(kBaseRes / 4, kBaseRes / 4, kBaseRes / 4);

    g.cmd->SetPipelineState(g.psoGenDetail.Get());
    g.cmd->Dispatch(kDetailRes / 4, kDetailRes / 4, kDetailRes / 4);

    D3D12_RESOURCE_BARRIER barriers[] =
    {
        Transition(g.baseNoise.Get(),   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(g.detailNoise.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    g.cmd->ResourceBarrier(2, barriers);

    SubmitAndWait();
}

static Params MakeParams(const Config& c, float time, float yaw)
{
    Params p = {};

    // Ground-level observer looking up at a storm about 8 km away - the framing
    // the screensaver actually uses. Inside an unbounded layer every ray sees
    // kilometres of uniform cloud and the frame reads as fog; from below one it
    // reads as overcast. Only a bounded volume viewed from outside gives cloud
    // silhouetted against sky, which is both the target look and the only case
    // where empty-space skipping has anything to skip.
    const bool  fill  = (c.framing == kFramingFillFrame);
    const float pitch = fill ? 0.10f : 0.55f;
    float cy = std::cos(yaw),   sy = std::sin(yaw);
    float cp = std::cos(pitch), sp = std::sin(pitch);

    float fwd[3]   = { sy * cp, sp, cy * cp };
    float right[3] = { cy, 0.0f, -sy };
    float up[3]    = { -sy * sp, cp, -cy * sp };

    p.camPos[0] = 0.0f;
    p.camPos[1] = fill ? 5000.0f : 400.0f;
    p.camPos[2] = fill ? 3800.0f : -2000.0f;
    for (int i = 0; i < 3; ++i) { p.camFwd[i] = fwd[i]; p.camRight[i] = right[i]; p.camUp[i] = up[i]; }

    p.time        = time;
    p.tanHalfFov  = std::tan(0.5f * 1.0472f);        // 60 degree vertical field of view
    p.aspect      = (float)c.width / (float)c.height;
    p.coverage    = 0.55f;
    p.outSize[0]  = (float)c.width;
    p.outSize[1]  = (float)c.height;
    p.texSize[0]  = (float)kMaxWidth;
    p.texSize[1]  = (float)kMaxHeight;
    p.numSteps    = c.steps;
    p.mode        = c.mode;
    p.lightSteps  = c.lightSteps;
    p.flags       = c.flags;
    // Storm dimensions, not fair-weather-cumulus dimensions. A cumulonimbus
    // runs from roughly 1 km to the tropopause, and the far longer rays through
    // a 10.8 km deep volume are what the frame budget has to survive.
    p.densityScale = 1.0f;
    p.cloudBottom  = 1200.0f;
    p.cloudTop     = 12000.0f;
    p.edgeSoftness = 0.55f;

    // A 13 x 3.5 x 13 km volume of cloud sitting 8 km out - roughly the size a
    // single storm cell occupies, and the same shape as the simulation box
    // Phase 02 will fill.
    p.boxCentre[0] = 0.0f; p.boxCentre[1] = 0.0f; p.boxCentre[2] = 11000.0f;
    p.boxHalfXZ    = 6500.0f;

    // Off to the side and well up, so the field is cross-lit and the bright and
    // shadowed faces separate. A sun straight down the view axis backlights
    // everything into silhouette, which hides whether the sampler is working.
    float s[3] = { 0.78f, 0.42f, 0.20f };
    float len = std::sqrt(s[0]*s[0] + s[1]*s[1] + s[2]*s[2]);
    for (int i = 0; i < 3; ++i) p.sunDir[i] = s[i] / len;
    // Extinction is per metre. At the previous 0.045 a single 27 m step reached
    // an optical depth of ~1.2, so the layer went fully opaque within a handful
    // of samples and the sun march was extinguished before its first tap.
    p.sigmaT = 0.006f;

    return p;
}

// Records one frame and returns the GPU time of the raymarch dispatch alone,
// in milliseconds. The timestamps bracket the Dispatch and nothing else.
static FrameTimes RenderFrame(const Config& c, float time, float yaw)
{
    BeginCommands();

    Params p = MakeParams(c, time, yaw);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g.srvHeap->GetGPUDescriptorHandleForHeapStart();

    g.cmd->SetComputeRootSignature(g.rootSig.Get());
    g.cmd->SetComputeRootDescriptorTable(0, table);
    g.cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &p, 0);

    const bool usesLightVolume = (c.mode == kModeLightVolume);

    // Rebuilt every frame here, which is the pessimistic case. In the real
    // renderer this runs at the simulation rate, so its cost is divided by the
    // number of frames between simulation ticks.
    g.cmd->EndQuery(g.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    if (usesLightVolume)
    {
        g.cmd->SetPipelineState(g.psoLightVolume.Get());
        g.cmd->Dispatch(kLightVolumeX / 4, kLightVolumeY / 4, kLightVolumeZ / 4);

        auto toRead = Transition(g.lightVolume.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        g.cmd->ResourceBarrier(1, &toRead);
    }
    g.cmd->EndQuery(g.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);

    g.cmd->SetPipelineState(g.psoRaymarch.Get());
    g.cmd->EndQuery(g.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
    g.cmd->Dispatch((c.width + 7) / 8, (c.height + 7) / 8, 1);
    g.cmd->EndQuery(g.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3);
    g.cmd->ResolveQueryData(g.queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 4,
                            g.queryReadback.Get(), 0);

    if (usesLightVolume)
    {
        auto toWrite = Transition(g.lightVolume.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g.cmd->ResourceBarrier(1, &toWrite);
    }

    // Present the result so there is something to look at while it runs.
    UINT backIndex = g.swapChain->GetCurrentBackBufferIndex();
    D3D12_RESOURCE_BARRIER toRead[] =
    {
        Transition(g.outputTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(g.backBuffers[backIndex].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET),
    };
    g.cmd->ResourceBarrier(2, toRead);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)backIndex * g.rtvStride;
    g.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    D3D12_VIEWPORT vp = { 0.0f, 0.0f, (float)kWindowWidth, (float)kWindowHeight, 0.0f, 1.0f };
    D3D12_RECT scissor = { 0, 0, (LONG)kWindowWidth, (LONG)kWindowHeight };
    g.cmd->RSSetViewports(1, &vp);
    g.cmd->RSSetScissorRects(1, &scissor);

    g.cmd->SetGraphicsRootSignature(g.rootSig.Get());
    g.cmd->SetGraphicsRootDescriptorTable(0, table);
    g.cmd->SetGraphicsRoot32BitConstants(1, sizeof(Params) / 4, &p, 0);
    g.cmd->SetPipelineState(g.psoBlit.Get());
    g.cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g.cmd->DrawInstanced(3, 1, 0, 0);

    D3D12_RESOURCE_BARRIER toWrite[] =
    {
        Transition(g.backBuffers[backIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PRESENT),
        Transition(g.outputTex.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    g.cmd->ResourceBarrier(2, toWrite);

    SubmitAndWait();
    g.swapChain->Present(0, 0);

    UINT64* stamps = nullptr;
    D3D12_RANGE readRange = { 0, sizeof(UINT64) * 4 };
    HR(g.queryReadback->Map(0, &readRange, (void**)&stamps), "map query readback");
    const double toMs = 1000.0 / (double)g.timestampFrequency;
    FrameTimes t;
    t.buildMs = usesLightVolume ? (double)(stamps[1] - stamps[0]) * toMs : 0.0;
    t.marchMs = (double)(stamps[3] - stamps[2]) * toMs;
    D3D12_RANGE noWrite = { 0, 0 };
    g.queryReadback->Unmap(0, &noWrite);

    PumpMessages();
    return t;
}

// ------------------------------------------------------------------ capture

// Renders one frame and writes it out as a 24-bit BMP. A fast timing number
// from a shader that draws an empty sky would be worthless, so the spike has
// to be able to prove there are actually clouds in the frame.
static void CaptureFrame(const Config& c, const char* path)
{
    RenderFrame(c, 12.0f, 0.0f);

    D3D12_RESOURCE_DESC desc = g.outputTex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT   rowCount = 0;
    UINT64 rowBytes = 0, totalBytes = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowBytes, &totalBytes);

    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = totalBytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    auto props = HeapProps(D3D12_HEAP_TYPE_READBACK);
    ComPtr<ID3D12Resource> readback;
    HR(g.device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&readback)), "capture readback");

    BeginCommands();
    auto toCopy = Transition(g.outputTex.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.cmd->ResourceBarrier(1, &toCopy);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = footprint;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = g.outputTex.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;

    D3D12_BOX box = { 0, 0, 0, (UINT)c.width, (UINT)c.height, 1 };
    g.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    auto back = Transition(g.outputTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.cmd->ResourceBarrier(1, &back);
    SubmitAndWait();

    const uint8_t* pixels = nullptr;
    D3D12_RANGE readRange = { 0, (SIZE_T)totalBytes };
    HR(readback->Map(0, &readRange, (void**)&pixels), "map capture readback");

    const int  w = c.width, h = c.height;
    const int  rowPadded = ((w * 3) + 3) & ~3;
    const UINT pitch = footprint.Footprint.RowPitch;

    std::vector<uint8_t> image((size_t)rowPadded * h, 0);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* srcRow = pixels + (size_t)y * pitch;
        uint8_t* dstRow = image.data() + (size_t)(h - 1 - y) * rowPadded;   // BMP is bottom-up
        for (int x = 0; x < w; ++x)
        {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2];   // B
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];   // G
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0];   // R
        }
    }
    D3D12_RANGE noWrite = { 0, 0 };
    readback->Unmap(0, &noWrite);

#pragma pack(push, 1)
    struct FileHeader { uint16_t type; uint32_t size; uint16_t r1, r2; uint32_t offset; };
    struct InfoHeader { uint32_t size; int32_t w, h; uint16_t planes, bits;
                        uint32_t compression, imageSize; int32_t xppm, yppm;
                        uint32_t used, important; };
#pragma pack(pop)

    const uint32_t dataBytes = (uint32_t)image.size();
    FileHeader fh = { 0x4D42, (uint32_t)(54 + dataBytes), 0, 0, 54 };
    InfoHeader ih = { 40, w, h, 1, 24, 0, dataBytes, 2835, 2835, 0, 0 };

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
    {
        std::fprintf(stderr, "  WARNING: could not write %s\n", path);
        return;
    }
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(image.data(), 1, image.size(), f);
    std::fclose(f);

    std::printf("  Captured %dx%d frame (mode %d, %d steps) to %s\n",
                w, h, c.mode, c.steps, path);
}

// ---------------------------------------------------------------- benchmark

static Result Measure(const Config& c, int warmup, int frames)
{
    for (int i = 0; i < warmup; ++i) RenderFrame(c, 0.0f, 0.0f);

    std::vector<double> samples, builds;
    samples.reserve(frames);
    builds.reserve(frames);
    for (int i = 0; i < frames && !g.windowClosed; ++i)
    {
        FrameTimes t = RenderFrame(c, 0.0f, 0.0f);
        samples.push_back(t.marchMs);
        builds.push_back(t.buildMs);
    }

    Result r = {};
    r.config = c;
    if (samples.empty()) return r;

    double sum = 0.0;
    for (double s : samples) sum += s;
    r.mean        = sum / (double)samples.size();
    r.minimum     = *std::min_element(samples.begin(), samples.end());
    r.median      = Percentile(samples, 0.50);
    r.p95         = Percentile(samples, 0.95);
    r.buildMedian = Percentile(builds, 0.50);
    return r;
}

static void RunBenchmark(int frames, const std::string& csvPath)
{
    const int steps[] = { 32, 64, 96, 128 };
    const struct { int w, h; const char* label; } resolutions[] =
    {
        { 1720,  720, "1720x720   (half of 3440x1440, 1.24 Mpx)" },
        { 3440, 1440, "3440x1440  (native ultrawide, 4.95 Mpx)"  },
    };

    std::vector<Result> results;
    const int totalConfigs = (int)(_countof(resolutions) * kModeCount * _countof(steps));
    int done = 0;

    std::printf("Benchmark: %d frames per config, GPU timestamps around the raymarch\n"
                "dispatch only. Every frame is fully synchronised, so these are clean\n"
                "GPU times with no CPU or present cost folded in.\n\n", frames);

    for (const auto& res : resolutions)
    {
        std::printf("  %s\n", res.label);
        std::printf("  %-30s %6s %8s %8s %8s %8s\n", "mode", "steps", "mean", "min", "median", "p95");
        std::printf("  %s\n", std::string(72, '-').c_str());

        for (int mode = 0; mode < kModeCount; ++mode)
        {
            for (int s : steps)
            {
                if (g.windowClosed) return;

                Config c{ mode, s, res.w, res.h };
                Result r = Measure(c, 10, frames);
                results.push_back(r);
                ++done;

                std::printf("  %-30s %6d %8.3f %8.3f %8.3f %8.3f    [%d/%d]\n",
                            (s == steps[0] ? kModeNames[mode] : ""),
                            s, r.mean, r.minimum, r.median, r.p95, done, totalConfigs);
                std::fflush(stdout);
            }
        }
        std::printf("\n");
    }

    auto findResult = [&](int mode, int stepCount, int w) -> const Result*
    {
        for (const auto& r : results)
            if (r.config.mode == mode && r.config.steps == stepCount && r.config.width == w)
                return &r;
        return nullptr;
    };

    // Marginal cost of a single step, measured with the transmittance early-out
    // suppressed so every ray takes exactly gNumSteps samples. Without this the
    // figure is confounded: a denser density function terminates rays sooner and
    // therefore looks cheaper per step than a sparser one, which is why mode 1
    // reads as faster than the analytic mode 0 in the table above.
    std::printf("  Marginal cost of one raymarch step, early-out suppressed so every ray\n"
                "  takes the full step count. (median at 128 steps - median at 64) / 64.\n"
                "  Fixed-stepping modes only; adaptive modes choose their own step count.\n\n");
    std::printf("  %-30s %14s %14s\n", "mode", "1720x720", "3440x1440");
    std::printf("  %s\n", std::string(62, '-').c_str());

    for (int mode = 0; mode <= 2; ++mode)
    {
        double per[2] = { 0.0, 0.0 };
        double full[2] = { 0.0, 0.0 };
        for (int i = 0; i < 2; ++i)
        {
            if (g.windowClosed) return;
            Config lo{ mode,  64, resolutions[i].w, resolutions[i].h, kFlagNoEarlyOut };
            Config hi{ mode, 128, resolutions[i].w, resolutions[i].h, kFlagNoEarlyOut };
            Result rlo = Measure(lo, 8, frames / 2);
            Result rhi = Measure(hi, 8, frames / 2);
            per[i]  = (rhi.median - rlo.median) / 64.0;
            full[i] = rhi.median;
        }
        std::printf("  %-30s %11.4f ms %11.4f ms   (128 steps uncapped: %.2f / %.2f ms)\n",
                    kModeNames[mode], per[0], per[1], full[0], full[1]);
    }
    std::printf("\n");

    if (!csvPath.empty())
    {
        FILE* f = nullptr;
        if (fopen_s(&f, csvPath.c_str(), "w") == 0 && f)
        {
            std::fprintf(f, "adapter,mode,mode_name,steps,width,height,mean_ms,min_ms,median_ms,p95_ms\n");
            for (const auto& r : results)
                std::fprintf(f, "\"%s\",%d,\"%s\",%d,%d,%d,%.4f,%.4f,%.4f,%.4f\n",
                             g.adapterName.c_str(), r.config.mode, kModeNames[r.config.mode],
                             r.config.steps, r.config.width, r.config.height,
                             r.mean, r.minimum, r.median, r.p95);
            std::fclose(f);
            std::printf("  Wrote %s\n\n", csvPath.c_str());
        }
        else
        {
            std::fprintf(stderr, "  WARNING: could not write %s\n\n", csvPath.c_str());
        }
    }

    // Worst case: camera against the volume so no ray misses. The sweep above
    // frames the storm the way the screensaver would, which leaves much of the
    // frame as sky; this bounds the other end.
    std::printf("  Worst case - volume covers every pixel, no ray misses (mode 4, 96 steps):\n\n");
    for (const auto& res : resolutions)
    {
        if (g.windowClosed) return;
        Config wide{ 4, 96, res.w, res.h, 0, kFramingFillFrame };
        Result rw = Measure(wide, 8, frames);
        const Result* normal = findResult(4, 96, res.w);
        std::printf("  %-42s %8.2f ms   (%.1fx the framed shot)\n",
                    res.label, rw.median,
                    (normal && normal->median > 0.0) ? rw.median / normal->median : 0.0);
    }
    std::printf("\n");

    // The production-shaped config, held against the plan's stated budget.
    const Result* production = findResult(4, 96, 1720);
    if (production)
    {
        double perFrameAt30 = 33.3, perFrameAt60 = 16.7;
        std::printf("  Production-shaped config (mode 4, 96 steps, 1720x720): %.2f ms median.\n",
                    production->median);
        std::printf("  That is %.0f%% of a 30 fps frame and %.0f%% of a 60 fps frame,\n"
                    "  before any temporal amortisation.\n",
                    production->median / perFrameAt30 * 100.0,
                    production->median / perFrameAt60 * 100.0);
        std::printf("  With a 4x4 Bayer pattern updating one pixel in sixteen: about %.2f ms.\n\n",
                    production->median / 16.0);
    }
}

// -------------------------------------------------- light march investigation

// The main sweep says the 6-tap sun march is ~60% of the frame. This pulls that
// apart: how the cost scales with tap count, what the detail octaves in the
// light march are worth, and whether precomputing transmittance into a volume
// at simulation rate removes the problem outright.
static void RunLightMarchStudy(int frames)
{
    const int W = 1720, H = 720, STEPS = 96;

    std::printf("================================================================\n");
    std::printf("  Light march study - %dx%d, %d steps, RTX-class timings\n", W, H, STEPS);
    std::printf("================================================================\n\n");

    Result noLight = Measure(Config{ 3, STEPS, W, H }, 10, frames);
    std::printf("  Baseline, no lighting at all (mode 3):      %6.2f ms\n\n", noLight.median);

    // How does cost scale with taps? Linear scaling means the taps themselves
    // are the cost; a large fixed offset would mean something else dominates.
    std::printf("  Cost against tap count (mode 4)\n");
    std::printf("  %6s %10s %14s %16s\n", "taps", "total", "lighting", "per tap");
    std::printf("  %s\n", std::string(50, '-').c_str());

    const int tapCounts[] = { 2, 4, 6, 8, 12 };
    for (int taps : tapCounts)
    {
        if (g.windowClosed) return;
        Config c{ 4, STEPS, W, H, 0, kFramingStandard, taps };
        Result r = Measure(c, 8, frames);
        double lighting = r.median - noLight.median;
        std::printf("  %6d %8.2f ms %11.2f ms %13.3f ms\n",
                    taps, r.median, lighting, lighting / (double)taps);
    }
    std::printf("\n");

    // Are the detail octaves worth anything in the light march? Shadowing is a
    // low-frequency effect, so probably not.
    Config full{ 4, STEPS, W, H, 0,                    kFramingStandard, 6 };
    Config base{ 4, STEPS, W, H, kFlagLightBaseOnly,   kFramingStandard, 6 };
    Result rFull = Measure(full, 8, frames);
    Result rBase = Measure(base, 8, frames);
    std::printf("  Light march sampling base + detail:         %6.2f ms\n", rFull.median);
    std::printf("  Light march sampling base only:             %6.2f ms   (%.0f%% saved)\n\n",
                rBase.median,
                (rFull.median > 0.0) ? (1.0 - rBase.median / rFull.median) * 100.0 : 0.0);

    // The architectural fix: compute transmittance once into a coarse volume
    // and read it with a single fetch.
    Result rVolume = Measure(Config{ kModeLightVolume, STEPS, W, H }, 10, frames);

    std::printf("  Precomputed light volume (%ux%ux%u)\n", kLightVolumeX, kLightVolumeY, kLightVolumeZ);
    std::printf("  %s\n", std::string(50, '-').c_str());
    std::printf("  raymarch, single volume fetch:             %6.2f ms\n", rVolume.median);
    std::printf("  volume build (24-tap march per voxel):     %6.2f ms\n", rVolume.buildMedian);
    std::printf("  both, rebuilt every frame:                 %6.2f ms\n",
                rVolume.median + rVolume.buildMedian);
    std::printf("  both, rebuilt at 20 Hz against 60 fps:     %6.2f ms\n",
                rVolume.median + rVolume.buildMedian / 3.0);
    std::printf("  both, rebuilt at 20 Hz against 30 fps:     %6.2f ms\n\n",
                rVolume.median + rVolume.buildMedian / 1.5);

    double amortised = rVolume.median + rVolume.buildMedian / 3.0;
    if (rFull.median > 0.0 && amortised > 0.0)
    {
        std::printf("  Against the 6-tap march (%.2f ms): %.2fx faster amortised,\n",
                    rFull.median, rFull.median / amortised);
        std::printf("  and lighting falls from %.0f%% to %.0f%% of the raymarch cost.\n\n",
                    (rFull.median - noLight.median) / rFull.median * 100.0,
                    (amortised - noLight.median) / amortised * 100.0);
    }
}

// -------------------------------------------------------------- interactive

static void RunInteractive()
{
    std::printf("Interactive view: mode 4 (adaptive + light march), 96 steps, 1720x720.\n"
                "Close the window or press Escape to exit.\n\n");

    Config c{ 4, 96, 1720, 720 };
    LARGE_INTEGER freq, start, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    last = start;

    int    framesSinceReport = 0;
    double gpuMsAccum = 0.0;

    while (!g.windowClosed)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float elapsed = (float)((double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart);

        FrameTimes ft = RenderFrame(c, elapsed, elapsed * 0.05f);
        gpuMsAccum += ft.buildMs + ft.marchMs;
        ++framesSinceReport;

        double sinceReport = (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart;
        if (sinceReport >= 1.0)
        {
            std::printf("\r  %6.1f fps   raymarch %6.2f ms GPU     ",
                        framesSinceReport / sinceReport, gpuMsAccum / framesSinceReport);
            std::fflush(stdout);
            framesSinceReport = 0;
            gpuMsAccum = 0.0;
            last = now;
        }
    }
    std::printf("\n\n");
}

// --------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    int  adapterIndex = -1;
    int  frames = 50;
    bool listAdapters = false, debugLayer = false, benchOnly = false, interactiveOnly = false;
    bool lightStudyOnly = false;
    int  captureMode = 4;
    std::string csvPath = "spike01-results.csv";
    std::string capturePath;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if      (a == "--list-adapters")   listAdapters = true;
        else if (a == "--debug")           debugLayer = true;
        else if (a == "--bench-only")      benchOnly = true;
        else if (a == "--interactive-only") interactiveOnly = true;
        else if (a == "--adapter" && i + 1 < argc) adapterIndex = std::atoi(argv[++i]);
        else if (a == "--frames"  && i + 1 < argc) frames = std::max(5, std::atoi(argv[++i]));
        else if (a == "--csv"     && i + 1 < argc) csvPath = argv[++i];
        else if (a == "--capture" && i + 1 < argc) capturePath = argv[++i];
        else if (a == "--light-study")     lightStudyOnly = true;
        else if (a == "--capture-mode" && i + 1 < argc) captureMode = std::atoi(argv[++i]);
        else { std::printf("Unknown argument: %s\n", a.c_str()); return 1; }
    }

    std::printf("\nStorm / Spike 01 - what does a raymarch step cost?\n");
    std::printf("=================================================\n\n");

    Init(adapterIndex, listAdapters, debugLayer);

    std::printf("Device:              %s\n", g.adapterName.c_str());
    std::printf("Timestamp frequency: %llu Hz\n", (unsigned long long)g.timestampFrequency);
    std::printf("Shaders:             %s\n\n", Narrow(g.shaderDir.c_str()).c_str());

    std::printf("Generating tileable noise volumes (%u^3 base, %u^3 detail)... ", kBaseRes, kDetailRes);
    std::fflush(stdout);
    GenerateNoise();
    std::printf("done\n\n");

    if (!capturePath.empty())
    {
        CaptureFrame(Config{ captureMode, 160, 1720, 720, 0 }, capturePath.c_str());
        std::printf("\n");
    }

    if (lightStudyOnly)
    {
        RunLightMarchStudy(frames);
        std::printf("Done.\n");
        return 0;
    }

    if (!interactiveOnly) RunBenchmark(frames, csvPath);
    if (!benchOnly && !g.windowClosed) RunLightMarchStudy(frames);
    if (!benchOnly && !g.windowClosed) RunInteractive();

    std::printf("Done.\n");
    return 0;
}
