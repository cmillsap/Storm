// Storm - Direct3D 12 device and the small amount of plumbing every pass needs.
//
// Deliberately single-threaded and fully synchronised: one command allocator,
// one list, a fence wait at the end of every frame. At a 30 fps cap with a
// couple of milliseconds of GPU work there is over 30 ms of headroom, so
// pipelining would buy nothing and cost a class of bug that is miserable to
// find inside a process Windows launches on its own.
#pragma once

#include "common.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <vector>

// Owns a compiled shader blob and hands out its bytecode. IDxcBlob and
// ID3DBlob are not the same interface despite the identical shape, so the blob
// is held as IUnknown rather than cast, and dxcapi.h stays out of this header.
struct Shader
{
    ComPtr<IUnknown> owner;
    const void* data = nullptr;
    SIZE_T size = 0;

    D3D12_SHADER_BYTECODE bytecode() const { return { data, size }; }
};

// A linear allocator over a descriptor heap. Slots are handed out once at
// startup and never freed.
struct DescriptorHeap
{
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT stride = 0;
    UINT capacity = 0;
    UINT used = 0;
    bool shaderVisible = false;

    void create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type,
                UINT count, bool visible, const char* what);
    UINT allocate();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT index) const;
};

struct Gpu
{
    ComPtr<IDXGIFactory6>             factory;
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        queue;
    ComPtr<ID3D12CommandAllocator>    allocator;
    ComPtr<ID3D12GraphicsCommandList> cmd;

    DescriptorHeap srvHeap;   // shader-visible CBV/SRV/UAV
    DescriptorHeap rtvHeap;   // render targets, two per view

    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE fenceEvent = nullptr;

    std::wstring adapterName;

    bool initialise(bool enableDebugLayer);
    void shutdown();

    void beginFrame();          // reset allocator and list, bind the srv heap
    void submitAndWait();       // close, execute, block until the GPU is done

    // Compiles a shader from shaders/<file> at startup. Keeping compilation at
    // runtime is what made the look spike fast to iterate, and it costs a few
    // hundred milliseconds once.
    Shader compile(const wchar_t* file, const wchar_t* entry, const wchar_t* target);

    static D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r,
                                             D3D12_RESOURCE_STATES from,
                                             D3D12_RESOURCE_STATES to);
};

// Locates the shaders directory by walking up from the executable, so a build
// output directory and an installed copy both work.
const std::wstring& ShaderDirectory();
