#include "view.h"

bool View::create(Gpu& gpu, HWND window, UINT w, UINT h)
{
    hwnd = window;
    width = w ? w : 1;
    height = h ? h : 1;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBackBufferCount;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.SampleDesc.Count = 1;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(gpu.factory->CreateSwapChainForHwnd(gpu.queue.Get(), hwnd, &desc,
                                                   nullptr, nullptr, &sc1)))
        return false;
    if (FAILED(sc1.As(&swapChain))) return false;

    // Storm handles its own exit; Alt+Enter has no meaning here.
    gpu.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    for (UINT i = 0; i < kBackBufferCount; ++i)
    {
        if (FAILED(swapChain->GetBuffer(i, IID_PPV_ARGS(&backBuffers[i])))) return false;
        rtvIndex[i] = gpu.rtvHeap.allocate();
        gpu.device->CreateRenderTargetView(backBuffers[i].Get(), nullptr,
                                           gpu.rtvHeap.cpu(rtvIndex[i]));
    }
    return true;
}

void View::release()
{
    for (UINT i = 0; i < kBackBufferCount; ++i) backBuffers[i].Reset();
    swapChain.Reset();
    if (ownsWindow && hwnd) { DestroyWindow(hwnd); }
    hwnd = nullptr;
}

void View::cropToFill(float outScale[2], float outOffset[2]) const
{
    outScale[0] = outScale[1] = 1.0f;
    outOffset[0] = outOffset[1] = 0.0f;
    if (!source || source->width == 0 || source->height == 0) return;

    const float targetAspect = source->aspect();
    const float viewAspect   = aspect();

    if (viewAspect < targetAspect)
    {
        // View is proportionally taller: keep full height, trim the sides.
        const float f = viewAspect / targetAspect;
        outScale[0] = f;
        outOffset[0] = (1.0f - f) * 0.5f;
    }
    else if (viewAspect > targetAspect)
    {
        // View is proportionally wider: keep full width, trim top and bottom.
        const float f = targetAspect / viewAspect;
        outScale[1] = f;
        outOffset[1] = (1.0f - f) * 0.5f;
    }
}
