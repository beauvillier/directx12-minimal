#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>

using Microsoft::WRL::ComPtr;
constexpr UINT Width = 960, Height = 640, BufferCount = 2;

void Report(const char* message) {
    std::fprintf(stderr, "%s\n", message);
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

void Check(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        char message[256];
        std::snprintf(message, sizeof(message), "%s failed (HRESULT 0x%08lX).",
                      operation, static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

class Demo {
public:
    HWND window = nullptr;
    bool paused = false;
    bool quit = false;

    ~Demo() {
        try { WaitForGpu(); }
        catch (const std::exception& error) { Report(error.what()); }
        if (fenceEvent) CloseHandle(fenceEvent);
        if (window) DestroyWindow(window);
    }

    void Initialize(HINSTANCE instance, bool warp, bool smoke);
    void Render(float angle);
    void VerifyPixels();
    void WaitForGpu() {
        if (!gpuPending) return;
        const UINT64 value = ++fenceValue;
        Check(queue->Signal(fence.Get(), value), "Signal GPU fence");
        if (fence->GetCompletedValue() < value) {
            Check(fence->SetEventOnCompletion(value, fenceEvent), "Set fence event");
            const DWORD result = WaitForSingleObject(fenceEvent, INFINITE);
            if (result == WAIT_FAILED)
                Check(HRESULT_FROM_WIN32(GetLastError()), "Wait for GPU");
            if (result != WAIT_OBJECT_0)
                throw std::runtime_error("Unexpected GPU fence wait result.");
        }
        // UINT64_MAX from GetCompletedValue indicates device removal.
        if (fence->GetCompletedValue() == UINT64_MAX) {
            Check(device->GetDeviceRemovedReason(), "GPU device removed");
            throw std::runtime_error("GPU fence reported device removal.");
        }
        gpuPending = false;
    }

private:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12Resource> targets[BufferCount];
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 fenceValue = 0;
    UINT rtvStride = 0;
    bool gpuPending = false;
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* demo = reinterpret_cast<Demo*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        demo = static_cast<Demo*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(demo));
    }
    if (demo) {
        if (message == WM_KEYDOWN) {
            if (wparam == VK_ESCAPE) demo->quit = true;
            if (wparam == VK_SPACE && !(lparam & (1LL << 30)))
                demo->paused = !demo->paused;
            return 0;
        }
        if (message == WM_CLOSE) {
            demo->quit = true;
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void Demo::Initialize(HINSTANCE instance, bool warp, bool smoke) {
    if (!smoke) {
        WNDCLASSW windowClass = {};
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = L"MinimalDirectX12";
        if (!RegisterClassW(&windowClass))
            Check(HRESULT_FROM_WIN32(GetLastError()), "Register window class");
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rect = {0, 0, static_cast<LONG>(Width), static_cast<LONG>(Height)};
        if (!AdjustWindowRect(&rect, style, FALSE))
            Check(HRESULT_FROM_WIN32(GetLastError()), "Adjust window rectangle");
        window = CreateWindowExW(0, windowClass.lpszClassName, L"Minimal Direct3D 12",
                                style, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance, this);
        if (!window) Check(HRESULT_FROM_WIN32(GetLastError()), "Create window");
    }

#ifdef _DEBUG
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
        debug->EnableDebugLayer();
#endif
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    if (warp)
        Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "Select WARP adapter");
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                           IID_PPV_ARGS(&device)), "Create D3D12 device (try --warp)");
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)), "Create queue");

    DXGI_SWAP_CHAIN_DESC1 swapDesc = {};
    swapDesc.Width = Width;
    swapDesc.Height = Height;
    swapDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapDesc.SampleDesc.Count = 1;
    swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapDesc.BufferCount = BufferCount;
    swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (!smoke) {
        ComPtr<IDXGISwapChain1> initialSwapChain;
        Check(factory->CreateSwapChainForHwnd(queue.Get(), window, &swapDesc, nullptr,
              nullptr, &initialSwapChain), "Create swap chain");
        Check(initialSwapChain.As(&swapChain), "Get swap chain interface");
        Check(factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER),
              "Disable fullscreen shortcut");
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    heapDesc.NumDescriptors = smoke ? 1 : BufferCount;
    Check(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap)),
          "Create render target descriptor heap");
    rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < heapDesc.NumDescriptors; ++i) {
        if (smoke) {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            heap.CreationNodeMask = heap.VisibleNodeMask = 1;
            D3D12_RESOURCE_DESC texture = {};
            texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            texture.Width = Width;
            texture.Height = Height;
            texture.DepthOrArraySize = texture.MipLevels = 1;
            texture.Format = swapDesc.Format;
            texture.SampleDesc.Count = 1;
            texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture,
                  D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&targets[i])),
                  "Create offscreen render target");
        } else {
            Check(swapChain->GetBuffer(i, IID_PPV_ARGS(&targets[i])), "Get back buffer");
        }
        device->CreateRenderTargetView(targets[i].Get(), nullptr, handle);
        handle.ptr += rtvStride;
    }

    D3D12_ROOT_PARAMETER parameter = {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameter.Constants.Num32BitValues = 2;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 1;
    rootDesc.pParameters = &parameter;
    ComPtr<ID3DBlob> signature, errors;
    HRESULT result = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                &signature, &errors);
    if (errors) Report(static_cast<const char*>(errors->GetBufferPointer()));
    Check(result, "Serialize root signature");
    Check(device->CreateRootSignature(0, signature->GetBufferPointer(),
          signature->GetBufferSize(), IID_PPV_ARGS(&rootSignature)), "Create root signature");

    const char* shader = R"(
cbuffer Settings : register(b0) { float angle; float aspect; };
struct Vertex { float4 position : SV_Position; float3 color : COLOR; };
Vertex VSMain(uint id : SV_VertexID) {
    float2 positions[3] = { float2(0, 0.7), float2(0.65, -0.5), float2(-0.65, -0.5) };
    float3 colors[3] = { float3(1, 0.2, 0.3), float3(0.2, 1, 0.4), float3(0.3, 0.5, 1) };
    float2 p = positions[id];
    float s = sin(angle), c = cos(angle);
    p = float2(c * p.x - s * p.y, s * p.x + c * p.y);
    Vertex v;
    v.position = float4(p.x / aspect, p.y, 0, 1);
    v.color = colors[id];
    return v;
}
float4 PSMain(Vertex v) : SV_Target { return float4(v.color, 1); }
)";
    auto compile = [&](const char* entry, const char* target, ID3DBlob** output) {
        ComPtr<ID3DBlob> diagnostics;
        const HRESULT hr = D3DCompile(shader, std::char_traits<char>::length(shader),
            "inline triangle shader", nullptr, nullptr, entry, target,
            D3DCOMPILE_ENABLE_STRICTNESS, 0, output, &diagnostics);
        if (diagnostics) Report(static_cast<const char*>(diagnostics->GetBufferPointer()));
        Check(hr, entry);
    };
    ComPtr<ID3DBlob> vertexShader, pixelShader;
    compile("VSMain", "vs_5_0", vertexShader.GetAddressOf());
    compile("PSMain", "ps_5_0", pixelShader.GetAddressOf());
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = rootSignature.Get();
    pso.VS = {vertexShader->GetBufferPointer(), vertexShader->GetBufferSize()};
    pso.PS = {pixelShader->GetBufferPointer(), pixelShader->GetBufferSize()};
    auto& blend = pso.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = swapDesc.Format;
    pso.SampleDesc.Count = 1;
    Check(device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline)),
          "Create graphics pipeline");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
          IID_PPV_ARGS(&allocator)), "Create command allocator");
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
          pipeline.Get(), IID_PPV_ARGS(&commands)), "Create command list");
    Check(commands->Close(), "Close initial command list");
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "Create fence");
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) Check(HRESULT_FROM_WIN32(GetLastError()), "Create fence event");
    if (window) ShowWindow(window, SW_SHOW);
}

void Demo::Render(float angle) {
    WaitForGpu();
    Check(allocator->Reset(), "Reset command allocator");
    Check(commands->Reset(allocator.Get(), pipeline.Get()), "Reset command list");
    const UINT index = swapChain ? swapChain->GetCurrentBackBufferIndex() : 0;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = targets[index].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    commands->ResourceBarrier(1, &barrier);
    auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += index * rtvStride;
    const float navy[] = {0.015f, 0.025f, 0.07f, 1.0f};
    commands->ClearRenderTargetView(rtv, navy, 0, nullptr);
    commands->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = {0, 0, float(Width), float(Height), 0, 1};
    const D3D12_RECT scissor = {0, 0, LONG(Width), LONG(Height)};
    commands->RSSetViewports(1, &viewport);
    commands->RSSetScissorRects(1, &scissor);
    commands->SetGraphicsRootSignature(rootSignature.Get());
    const float settings[] = {angle, float(Width) / float(Height)};
    commands->SetGraphicsRoot32BitConstants(0, 2, settings, 0);
    commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands->DrawInstanced(3, 1, 0, 0);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    commands->ResourceBarrier(1, &barrier);
    Check(commands->Close(), "Close command list");
    ID3D12CommandList* lists[] = {commands.Get()};
    gpuPending = true;
    queue->ExecuteCommandLists(1, lists);
    const HRESULT result = swapChain ? swapChain->Present(1, 0) : S_OK;
    Check(result, "Present frame");
    WaitForGpu(); // Deliberately serialize frames: one allocator, no in-flight reuse.
    if (result == DXGI_STATUS_OCCLUDED) Sleep(50);
}

void Demo::VerifyPixels() {
    WaitForGpu();
    const auto texture = targets[0]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT64 size = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &size);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = size;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)),
          "Create pixel readback buffer");
    Check(allocator->Reset(), "Reset readback allocator");
    Check(commands->Reset(allocator.Get(), nullptr), "Reset readback command list");
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = targets[0].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    // COMMON and PRESENT both have value zero; offscreen frames return to COMMON.
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    commands->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source = {}, destination = {};
    source.pResource = targets[0].Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    commands->ResourceBarrier(1, &barrier);
    Check(commands->Close(), "Close readback command list");
    ID3D12CommandList* lists[] = {commands.Get()};
    gpuPending = true;
    queue->ExecuteCommandLists(1, lists);
    WaitForGpu();
    void* mapped = nullptr;
    const D3D12_RANGE range = {0, static_cast<SIZE_T>(size)};
    Check(readback->Map(0, &range, &mapped), "Map pixel readback");
    const auto* corner = static_cast<const unsigned char*>(mapped) + footprint.Offset;
    const auto* centre = corner + (Height / 2) * footprint.Footprint.RowPitch + (Width / 2) * 4;
    const int navy[] = {4, 6, 18, 255}; // RGBA8 quantization of the clear colour.
    bool background = true, triangle = false;
    for (UINT channel = 0; channel < 4; ++channel) {
        const int delta = int(corner[channel]) - navy[channel];
        background = background && delta >= -1 && delta <= 1;
        if (channel < 3 && int(centre[channel]) > navy[channel] + 32) triangle = true;
    }
    triangle = triangle && centre[3] == 255;
    const D3D12_RANGE written = {0, 0};
    readback->Unmap(0, &written);
    if (!background || !triangle)
        throw std::runtime_error("Smoke test pixel verification failed: expected navy corner and coloured centre.");
    Report("Smoke test passed: 8 offscreen frames; navy corner and triangle centre verified by GPU readback.");
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    try {
        bool warp = false, smoke = false;
        std::wistringstream arguments(commandLine);
        std::wstring argument;
        while (arguments >> argument) {
            if (argument == L"--warp") warp = true;
            else if (argument == L"--smoke-test") smoke = true;
            else throw std::runtime_error("Unknown argument. Use --warp and/or --smoke-test.");
        }
        // Keep the client area in physical pixels, including on scaled desktops.
        if (!smoke && !SetProcessDPIAware())
            Check(HRESULT_FROM_WIN32(GetLastError()), "Set process DPI awareness");
        Demo demo;
        demo.Initialize(instance, warp, smoke);
        if (smoke) {
            for (unsigned frame = 0; frame < 8; ++frame)
                demo.Render(float(frame) * 0.125f);
            demo.VerifyPixels();
            return 0;
        }
        auto previous = std::chrono::steady_clock::now();
        float angle = 0;
        while (!demo.quit) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) demo.quit = true;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (demo.quit) break;
            if (IsIconic(demo.window)) {
                if (!WaitMessage())
                    Check(HRESULT_FROM_WIN32(GetLastError()), "Wait for window message");
                previous = std::chrono::steady_clock::now();
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!demo.paused)
                angle += std::chrono::duration<float>(now - previous).count();
            previous = now;
            demo.Render(angle);
        }
        demo.WaitForGpu();
        return 0;
    } catch (const std::exception& error) {
        Report(error.what());
        return 1;
    }
}
