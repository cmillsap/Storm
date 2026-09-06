#include "gpu.h"

#include <dxcapi.h>
#include <cstdio>

// ------------------------------------------------------------------ helpers

void FailHard(const char* what, HRESULT hr)
{
    char buffer[512];
    if (hr != S_OK)
        std::snprintf(buffer, sizeof(buffer), "%s\n\nHRESULT 0x%08lX", what, (unsigned long)hr);
    else
        std::snprintf(buffer, sizeof(buffer), "%s", what);

    MessageBoxA(nullptr, buffer, "Storm", MB_OK | MB_ICONERROR);
    std::exit(1);
}

std::string Narrow(const wchar_t* w)
{
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const char* s)
{
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

const std::wstring& ShaderDirectory()
{
    static std::wstring cached;
    if (!cached.empty()) return cached;

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t cut = dir.find_last_of(L"\\/");
    dir = (cut == std::wstring::npos) ? L"." : dir.substr(0, cut);

    for (int i = 0; i < 5; ++i)
    {
        std::wstring candidate = dir + L"\\shaders";
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            cached = candidate;
            return cached;
        }
        size_t p = dir.find_last_of(L"\\/");
        if (p == std::wstring::npos) break;
        dir = dir.substr(0, p);
    }
    FailHard("Could not find the shaders folder.\n\n"
             "Storm.scr, dxcompiler.dll, dxil.dll and the shaders folder must stay together.");
    return cached;
}

// ----------------------------------------------------------- DescriptorHeap

void DescriptorHeap::create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                            UINT count, bool visible, const char* what)
{
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.Type = type;
    desc.NumDescriptors = count;
    desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                         : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    STORM_CHECK(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), what);

    stride = device->GetDescriptorHandleIncrementSize(type);
    capacity = count;
    used = 0;
    shaderVisible = visible;
}

UINT DescriptorHeap::allocate()
{
    if (used >= capacity) FailHard("Descriptor heap exhausted.");
    return used++;
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeap::cpu(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * stride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeap::gpu(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)index * stride;
    return h;
}

// ---------------------------------------------------------------------- Gpu

bool Gpu::initialise(bool enableDebugLayer)
{
    if (enableDebugLayer)
    {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    }

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;

    // Highest-performance adapter that can actually make a device. On a machine
    // with a discrete card and an integrated one this reliably picks the card.
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                             IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         ++i)
    {
        DXGI_ADAPTER_DESC1 desc = {};
        adapter->GetDesc1(&desc);
        if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))))
        {
            adapterName = desc.Description;
            break;
        }
        adapter.Reset();
    }
    if (!device) return false;

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&allocator)))) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&cmd)))) return false;
    cmd->Close();

    // Two SRV/UAV slots for the shared render target, plus room for the
    // volumes Phase 02 will add. RTVs are two per monitor.
    srvHeap.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 32, true, "srv heap");
    rtvHeap.create(device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 32, false, "rtv heap");

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return fenceEvent != nullptr;
}

void Gpu::shutdown()
{
    if (queue && fence)
    {
        const UINT64 target = ++fenceValue;
        if (SUCCEEDED(queue->Signal(fence.Get(), target)) && fence->GetCompletedValue() < target)
        {
            fence->SetEventOnCompletion(target, fenceEvent);
            WaitForSingleObject(fenceEvent, 2000);
        }
    }
    if (fenceEvent) { CloseHandle(fenceEvent); fenceEvent = nullptr; }
}

void Gpu::beginFrame()
{
    STORM_CHECK(allocator->Reset(), "command allocator reset");
    STORM_CHECK(cmd->Reset(allocator.Get(), nullptr), "command list reset");
    ID3D12DescriptorHeap* heaps[] = { srvHeap.heap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
}

void Gpu::submitAndWait()
{
    STORM_CHECK(cmd->Close(), "command list close");
    ID3D12CommandList* lists[] = { cmd.Get() };
    queue->ExecuteCommandLists(1, lists);

    const UINT64 target = ++fenceValue;
    STORM_CHECK(queue->Signal(fence.Get(), target), "fence signal");
    if (fence->GetCompletedValue() < target)
    {
        STORM_CHECK(fence->SetEventOnCompletion(target, fenceEvent), "fence event");
        WaitForSingleObject(fenceEvent, INFINITE);
    }
}

D3D12_RESOURCE_BARRIER Gpu::transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from,
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

// ------------------------------------------------------------ DXC front end

Shader Gpu::compile(const wchar_t* file, const wchar_t* entry, const wchar_t* target)
{
    static DxcCreateInstanceProc create = nullptr;
    if (!create)
    {
        HMODULE dll = LoadLibraryW(L"dxcompiler.dll");
        if (!dll) FailHard("dxcompiler.dll not found - it must sit beside Storm.scr.");
        create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
        if (!create) FailHard("DxcCreateInstance missing from dxcompiler.dll.");
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcCompiler3> compiler;
    ComPtr<IDxcIncludeHandler> includes;
    STORM_CHECK(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)), "DxcUtils");
    STORM_CHECK(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler)), "DxcCompiler");
    STORM_CHECK(utils->CreateDefaultIncludeHandler(&includes), "include handler");

    std::wstring path = ShaderDirectory() + L"\\" + file;
    ComPtr<IDxcBlobEncoding> source;
    if (FAILED(utils->LoadFile(path.c_str(), nullptr, &source)))
        FailHard(("Could not read shader: " + Narrow(path.c_str())).c_str());

    DxcBuffer buffer{ source->GetBufferPointer(), source->GetBufferSize(), DXC_CP_ACP };

    // The source arrives as a buffer with no path attached, so #include has no
    // directory to resolve against unless one is supplied explicitly.
    const std::wstring& dir = ShaderDirectory();
    const wchar_t* args[] = {
        L"-E", entry,
        L"-T", target,
        L"-O3",
        L"-Qstrip_debug",
        L"-Qstrip_reflect",
        L"-I", dir.c_str(),
    };

    ComPtr<IDxcResult> result;
    STORM_CHECK(compiler->Compile(&buffer, args, (UINT32)std::size(args), includes.Get(),
                                  IID_PPV_ARGS(&result)), "DXC compile");

    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status))
    {
        std::string message = "Shader compilation failed: " + Narrow(file) + " / " + Narrow(entry);
        if (errors && errors->GetStringLength() > 0)
            message += "\n\n" + std::string(errors->GetStringPointer());
        FailHard(message.c_str());
    }

    ComPtr<IDxcBlob> object;
    STORM_CHECK(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), nullptr), "DXC object");

    Shader shader;
    shader.data = object->GetBufferPointer();
    shader.size = object->GetBufferSize();
    shader.owner = object;
    return shader;
}
