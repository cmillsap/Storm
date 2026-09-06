// Storm - Spike 02: can a single frame of cumulus be made to look good?
//
// Spike 01 proved the frame budget. This one answers the question that actually
// decides the project: whether a raymarched density field can be tuned into an
// image worth looking at, in hours rather than months. If it can, the plan is
// safe. If it cannot, no amount of simulation work downstream rescues it.
//
// The shaders are compiled at startup and every look constant lives in
// cloud.hlsl, so most iterations need no rebuild. The parameters worth sweeping
// from outside - sun angle, coverage, exposure - are command-line flags.
//
// Usage:  spike02.exe [--capture out.bmp] [--steps N] [--sun-elev DEG]
//                     [--sun-azim DEG] [--coverage F] [--density F]
//                     [--exposure F] [--width N] [--height N]
//                     [--adapter N] [--list-adapters] [--debug]

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

// Mirrors the cbuffer in cloud.hlsl and blit.hlsl. 36 root constants; HLSL
// packs float3+float into one 16-byte row, so this is row-for-row identical.
struct alignas(16) Params
{
    float   camPos[3];      float time;
    float   camFwd[3];      float tanHalfFov;
    float   camRight[3];    float aspect;
    float   camUp[3];       float coverage;
    float   outSize[2];     float texSize[2];
    int32_t numSteps;       int32_t lightSteps;  int32_t flags;    int32_t frameIndex;
    float   densityScale;   float cloudBottom;   float cloudTop;   float cloudRadius;
    float   sunDir[3];      float sunIntensity;
    float   cloudCentre[3]; float exposure;
};
static_assert(sizeof(Params) == 144, "Params must be exactly 36 root constants");

// Everything the host controls about the shot.
struct Scene
{
    float sunElevationDeg = 22.0f;
    float sunAzimuthDeg   = 55.0f;   // 0 = straight ahead (fully backlit)
    float coverage        = 0.72f;
    float densityScale    = 1.0f;
    float exposure        = 1.0f;
    float sunIntensity    = 22.0f;

    float cloudBottom = 1100.0f;
    float cloudTop    = 7200.0f;
    float cloudRadius = 2100.0f;
    float cloudZ      = 6500.0f;

    float camHeight = 40.0f;
    float pitch     = 0.42f;
    float fovDeg    = 50.0f;

    int   steps = 128;
};

// ------------------------------------------------------------------ globals

static const UINT kBaseRes      = 128;
static const UINT kDetailRes    = 32;
static const UINT kLightVolume  = 64;
static const UINT kBackBuffers  = 2;

static UINT kRenderWidth  = 1720;
static UINT kRenderHeight = 720;
static UINT kWindowWidth  = 1280;
static UINT kWindowHeight = 536;

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

struct App
{
    HWND                        hwnd = nullptr;
    ComPtr<IDXGIFactory6>       factory;
    ComPtr<ID3D12Device>        device;
    ComPtr<ID3D12CommandQueue>  queue;
    ComPtr<IDXGISwapChain3>     swapChain;
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;

    ComPtr<ID3D12DescriptorHeap> srvHeap, rtvHeap;
    UINT srvStride = 0, rtvStride = 0;

    ComPtr<ID3D12Resource> backBuffers[kBackBuffers];
    ComPtr<ID3D12Resource> outputTex, baseNoise, detailNoise, lightVolume;

    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> psoGenBase, psoGenDetail, psoCloud, psoLightVolume, psoBlit;

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

    // The source is handed over as a buffer with no path attached, so #include
    // has no directory to resolve against unless one is supplied explicitly.
    std::vector<const wchar_t*> args =
    {
        L"-E", entry,
        L"-T", target,
        L"-O3",
        L"-Qstrip_debug",
        L"-Qstrip_reflect",
        L"-I", g.shaderDir.c_str(),
    };

    ComPtr<IDxcResult> result;
    HR(compiler->Compile(&buffer, args.data(), (UINT32)args.size(), includes.Get(),
                         IID_PPV_ARGS(&result)), "DXC Compile call");

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

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
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
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        __uuidof(ID3D12Device), nullptr)))
        {
            candidates.push_back({ adapter, desc });
        }
        adapter.Reset();
    }
    if (candidates.empty()) Fail("no Direct3D 12 capable adapter found");

    int best = 0;
    for (int i = 1; i < (int)candidates.size(); ++i)
        if (candidates[i].desc.DedicatedVideoMemory > candidates[best].desc.DedicatedVideoMemory)
            best = i;

    int chosen = (requested >= 0 && requested < (int)candidates.size()) ? requested : best;

    if (listOnly)
    {
        std::printf("Adapters:\n");
        for (int i = 0; i < (int)candidates.size(); ++i)
            std::printf("  [%d]%s %-34s %5.1f GB\n", i, (i == chosen ? " *" : "  "),
                        Narrow(candidates[i].desc.Description).c_str(),
                        (double)candidates[i].desc.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0));
        std::exit(0);
    }

    g.adapterName = Narrow(candidates[chosen].desc.Description);
    HR(D3D12CreateDevice(candidates[chosen].adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                         IID_PPV_ARGS(&g.device)), "D3D12CreateDevice");
}

static void CreateRootSignature()
{
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 4;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 4;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = sizeof(Params) / 4;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    for (int i = 0; i < 2; ++i)
    {
        samplers[i].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[i].MaxAnisotropy = 1;
        samplers[i].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
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
        if (error) std::fprintf(stderr, "%.*s\n", (int)error->GetBufferSize(),
                                (const char*)error->GetBufferPointer());
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

    auto csBase   = Compile(L"noise_gen.hlsl", L"CSGenBase",     L"cs_6_0");
    auto csDetail = Compile(L"noise_gen.hlsl", L"CSGenDetail",   L"cs_6_0");
    auto csCloud  = Compile(L"cloud.hlsl",     L"CSCloud",       L"cs_6_0");
    auto csLight  = Compile(L"cloud.hlsl",     L"CSLightVolume", L"cs_6_0");
    auto vsBlit   = Compile(L"blit.hlsl",      L"VSFullscreen",  L"vs_6_0");
    auto psBlit   = Compile(L"blit.hlsl",      L"PSBlit",        L"ps_6_0");

    g.psoGenBase     = MakeComputePso(csBase.Get(),   "noise base PSO");
    g.psoGenDetail   = MakeComputePso(csDetail.Get(), "noise detail PSO");
    g.psoCloud       = MakeComputePso(csCloud.Get(),  "cloud PSO");
    g.psoLightVolume = MakeComputePso(csLight.Get(),  "light volume PSO");

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gd = {};
    gd.pRootSignature = g.rootSig.Get();
    gd.VS = { vsBlit->GetBufferPointer(), vsBlit->GetBufferSize() };
    gd.PS = { psBlit->GetBufferPointer(), psBlit->GetBufferSize() };
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
    HR(g.device->CreateGraphicsPipelineState(&gd, IID_PPV_ARGS(&g.psoBlit)), "blit PSO");
}

static void CreateResourcesAndViews()
{
    g.outputTex = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                kRenderWidth, kRenderHeight, 1,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "render target");
    g.baseNoise = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                kBaseRes, kBaseRes, (UINT16)kBaseRes,
                                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "base noise");
    g.detailNoise = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM,
                                  kDetailRes, kDetailRes, (UINT16)kDetailRes,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "detail noise");
    g.lightVolume = CreateTexture(D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R16_FLOAT,
                                  kLightVolume, kLightVolume, (UINT16)kLightVolume,
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
    uav.Texture3D.WSize = kLightVolume;
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
    }

    SelectAdapter(adapterIndex, listAdapters);

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HR(g.device->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue");

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"StormSpike02";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, (LONG)kWindowWidth, (LONG)kWindowHeight };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g.hwnd = CreateWindowExW(0, L"StormSpike02", L"Storm - Spike 02: look development",
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

static Params MakeParams(const Scene& s, float time, int frameIndex)
{
    Params p = {};

    float cp = std::cos(s.pitch), sp = std::sin(s.pitch);
    p.camPos[0] = 0.0f; p.camPos[1] = s.camHeight; p.camPos[2] = 0.0f;
    p.camFwd[0] = 0.0f;   p.camFwd[1] = sp;   p.camFwd[2] = cp;
    p.camRight[0] = 1.0f; p.camRight[1] = 0.0f; p.camRight[2] = 0.0f;
    p.camUp[0] = 0.0f;    p.camUp[1] = cp;    p.camUp[2] = -sp;

    p.time       = time;
    p.tanHalfFov = std::tan(0.5f * s.fovDeg * 3.14159265f / 180.0f);
    p.aspect     = (float)kRenderWidth / (float)kRenderHeight;
    p.coverage   = s.coverage;
    p.outSize[0] = (float)kRenderWidth;
    p.outSize[1] = (float)kRenderHeight;
    p.texSize[0] = (float)kRenderWidth;
    p.texSize[1] = (float)kRenderHeight;
    p.numSteps   = s.steps;
    p.lightSteps = 6;
    p.flags      = 0;
    p.frameIndex = frameIndex;

    p.densityScale = s.densityScale;
    p.cloudBottom  = s.cloudBottom;
    p.cloudTop     = s.cloudTop;
    p.cloudRadius  = s.cloudRadius;

    // Azimuth is measured from straight ahead, so 0 puts the sun directly
    // behind the cloud (full silver lining) and 90 puts it side-on.
    float el = s.sunElevationDeg * 3.14159265f / 180.0f;
    float az = s.sunAzimuthDeg   * 3.14159265f / 180.0f;
    p.sunDir[0] = std::sin(az) * std::cos(el);
    p.sunDir[1] = std::sin(el);
    p.sunDir[2] = std::cos(az) * std::cos(el);
    p.sunIntensity = s.sunIntensity;

    p.cloudCentre[0] = 0.0f;
    p.cloudCentre[1] = 0.0f;
    p.cloudCentre[2] = s.cloudZ;
    p.exposure = s.exposure;

    return p;
}

static void RenderFrame(const Scene& s, float time, int frameIndex)
{
    BeginCommands();

    Params p = MakeParams(s, time, frameIndex);
    D3D12_GPU_DESCRIPTOR_HANDLE table = g.srvHeap->GetGPUDescriptorHandleForHeapStart();

    g.cmd->SetComputeRootSignature(g.rootSig.Get());
    g.cmd->SetComputeRootDescriptorTable(0, table);
    g.cmd->SetComputeRoot32BitConstants(1, sizeof(Params) / 4, &p, 0);

    g.cmd->SetPipelineState(g.psoLightVolume.Get());
    g.cmd->Dispatch(kLightVolume / 4, kLightVolume / 4, kLightVolume / 4);
    auto lvRead = Transition(g.lightVolume.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g.cmd->ResourceBarrier(1, &lvRead);

    g.cmd->SetPipelineState(g.psoCloud.Get());
    g.cmd->Dispatch((kRenderWidth + 7) / 8, (kRenderHeight + 7) / 8, 1);

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
        Transition(g.lightVolume.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    g.cmd->ResourceBarrier(3, toWrite);

    SubmitAndWait();
    g.swapChain->Present(0, 0);
    PumpMessages();
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

// ------------------------------------------------------------------ capture

static void CaptureFrame(const Scene& s, const char* path)
{
    RenderFrame(s, 0.0f, 0);

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
    D3D12_BOX box = { 0, 0, 0, kRenderWidth, kRenderHeight, 1 };
    g.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    auto back = Transition(g.outputTex.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g.cmd->ResourceBarrier(1, &back);
    SubmitAndWait();

    const uint8_t* pixels = nullptr;
    D3D12_RANGE readRange = { 0, (SIZE_T)totalBytes };
    HR(readback->Map(0, &readRange, (void**)&pixels), "map capture readback");

    const int w = (int)kRenderWidth, h = (int)kRenderHeight;
    const int rowPadded = ((w * 3) + 3) & ~3;
    const UINT pitch = footprint.Footprint.RowPitch;

    std::vector<uint8_t> image((size_t)rowPadded * h, 0);
    for (int y = 0; y < h; ++y)
    {
        const uint8_t* srcRow = pixels + (size_t)y * pitch;
        uint8_t* dstRow = image.data() + (size_t)(h - 1 - y) * rowPadded;
        for (int x = 0; x < w; ++x)
        {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 2];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 0];
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
    if (fopen_s(&f, path, "wb") != 0 || !f) { std::fprintf(stderr, "could not write %s\n", path); return; }
    std::fwrite(&fh, sizeof(fh), 1, f);
    std::fwrite(&ih, sizeof(ih), 1, f);
    std::fwrite(image.data(), 1, image.size(), f);
    std::fclose(f);

    std::printf("Captured %dx%d to %s\n", w, h, path);
}

// -------------------------------------------------------------------- main

int main(int argc, char** argv)
{
    Scene s;
    int  adapterIndex = -1;
    bool listAdapters = false, debugLayer = false, timeIt = false;
    std::string capturePath;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto nextF = [&]() { return (float)std::atof(argv[++i]); };
        auto nextI = [&]() { return std::atoi(argv[++i]); };

        if      (a == "--list-adapters") listAdapters = true;
        else if (a == "--debug")         debugLayer = true;
        else if (a == "--time")          timeIt = true;
        else if (a == "--adapter"   && i + 1 < argc) adapterIndex = nextI();
        else if (a == "--capture"   && i + 1 < argc) capturePath = argv[++i];
        else if (a == "--steps"     && i + 1 < argc) s.steps = nextI();
        else if (a == "--sun-elev"  && i + 1 < argc) s.sunElevationDeg = nextF();
        else if (a == "--sun-azim"  && i + 1 < argc) s.sunAzimuthDeg = nextF();
        else if (a == "--coverage"  && i + 1 < argc) s.coverage = nextF();
        else if (a == "--density"   && i + 1 < argc) s.densityScale = nextF();
        else if (a == "--exposure"  && i + 1 < argc) s.exposure = nextF();
        else if (a == "--sun-power" && i + 1 < argc) s.sunIntensity = nextF();
        else if (a == "--cloud-top" && i + 1 < argc) s.cloudTop = nextF();
        else if (a == "--cloud-base"&& i + 1 < argc) s.cloudBottom = nextF();
        else if (a == "--cloud-radius" && i + 1 < argc) s.cloudRadius = nextF();
        else if (a == "--cloud-z"   && i + 1 < argc) s.cloudZ = nextF();
        else if (a == "--pitch"     && i + 1 < argc) s.pitch = nextF();
        else if (a == "--fov"       && i + 1 < argc) s.fovDeg = nextF();
        else if (a == "--width"     && i + 1 < argc) kRenderWidth = (UINT)nextI();
        else if (a == "--height"    && i + 1 < argc) kRenderHeight = (UINT)nextI();
        else { std::printf("Unknown argument: %s\n", a.c_str()); return 1; }
    }

    kWindowWidth  = std::min<UINT>(1280, kRenderWidth);
    kWindowHeight = (UINT)(kWindowWidth * (double)kRenderHeight / (double)kRenderWidth);

    std::printf("\nStorm / Spike 02 - look development\n");
    std::printf("===================================\n\n");

    Init(adapterIndex, listAdapters, debugLayer);

    std::printf("Device:   %s\n", g.adapterName.c_str());
    std::printf("Render:   %ux%u, %d steps\n", kRenderWidth, kRenderHeight, s.steps);
    std::printf("Sun:      %.0f deg elevation, %.0f deg azimuth\n", s.sunElevationDeg, s.sunAzimuthDeg);
    std::printf("Cloud:    base %.0f m, top %.0f m, radius %.0f m at %.0f m\n\n",
                s.cloudBottom, s.cloudTop, s.cloudRadius, s.cloudZ);

    GenerateNoise();

    if (!capturePath.empty())
    {
        CaptureFrame(s, capturePath.c_str());
        if (!timeIt) return 0;
    }

    if (timeIt)
    {
        // Wall clock around a fully synchronised frame, so this is an upper
        // bound that includes the light-volume rebuild and the present.
        for (int i = 0; i < 5; ++i) RenderFrame(s, 0.0f, i);
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        const int N = 40;
        for (int i = 0; i < N; ++i) RenderFrame(s, 0.0f, i);
        QueryPerformanceCounter(&t1);
        double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)f.QuadPart / N;
        std::printf("Frame: %.1f ms at %ux%u, %d steps (%.1f fps)\n",
                    ms, kRenderWidth, kRenderHeight, s.steps, 1000.0 / ms);
        return 0;
    }

    std::printf("Interactive. Close the window or press Escape to exit.\n");
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    int frame = 0;
    while (!g.windowClosed)
    {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float elapsed = (float)((double)(now.QuadPart - start.QuadPart) / (double)freq.QuadPart);
        RenderFrame(s, elapsed, frame++);
    }
    return 0;
}
