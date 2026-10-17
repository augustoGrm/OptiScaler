// Real D3D12 WARP, production Runtime + Submission adapter, test-only identity model.
// Exercises recording, readback, descriptor reuse, multipass, resize, disable and failure bypass.
#define NOMINMAX
#include "../OptiScaler/dlssnr/Runtime.h"
#include "../OptiScaler/dlssnr/Submission.h"
#include "dlssnr_crash_report.h"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <thread>
#include <chrono>
#include <array>
#include <limits>

using Microsoft::WRL::ComPtr;
static void check(HRESULT hr)
{
    if (FAILED(hr))
        throw std::runtime_error("D3D12 call failed");
}
static void expect(bool value, const char* text)
{
    if (!value)
        throw std::runtime_error(text);
}
static void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                    D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    cmd->ResourceBarrier(1, &b);
}

static float unsignedFloat(unsigned bits, unsigned mantissaBits)
{
    const auto exponent = bits >> mantissaBits;
    const auto mantissa = bits & ((1u << mantissaBits) - 1);
    if (exponent == 31)
        return mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    return exponent ? std::ldexp(1.0f + std::ldexp(static_cast<float>(mantissa), -static_cast<int>(mantissaBits)),
                                 static_cast<int>(exponent) - 15)
                    : std::ldexp(static_cast<float>(mantissa), 1 - 15 - static_cast<int>(mantissaBits));
}

struct ColourCase
{
    DXGI_FORMAT format;
    std::vector<uint8_t> pixel;
    unsigned componentBits; // 32 float, 16 half, 11 packed float, 8/10/64 UNORM
    bool hdr = false;

    std::array<float, 4> Read(const void* bytes) const
    {
        std::array<float, 4> colour {};
        if (componentBits == 32)
            std::memcpy(colour.data(), bytes, 16);
        else if (componentBits == 16 || componentBits == 64)
        {
            uint16_t channels[4];
            std::memcpy(channels, bytes, 8);
            for (unsigned i = 0; i < 4; ++i)
                colour[i] = componentBits == 64
                                ? channels[i] / 65535.0f
                                : unsignedFloat(channels[i] & 0x7fff, 10) * (channels[i] & 0x8000 ? -1.0f : 1.0f);
        }
        else if (componentBits == 8)
            for (unsigned i = 0; i < 4; ++i)
                colour[i] = static_cast<const uint8_t*>(bytes)[i] / 255.0f;
        else
        {
            uint32_t packed;
            std::memcpy(&packed, bytes, 4);
            if (componentBits == 11)
                colour = { unsignedFloat(packed & 2047, 6), unsignedFloat((packed >> 11) & 2047, 6),
                           unsignedFloat(packed >> 22, 5), 1.0f };
            else
                colour = { (packed & 1023) / 1023.0f, ((packed >> 10) & 1023) / 1023.0f,
                           ((packed >> 20) & 1023) / 1023.0f, (packed >> 30) / 3.0f };
        }
        return colour;
    }
};

template <class T> static std::vector<uint8_t> Bytes(const T& value)
{
    const auto begin = reinterpret_cast<const uint8_t*>(&value);
    return { begin, begin + sizeof(value) };
}
const ColourCase float32 { DXGI_FORMAT_R32G32B32A32_FLOAT, Bytes(std::array { 0.25f, 0.5f, 0.75f, 1.0f }), 32 };

int main(int argc, char** argv)
try
{
    InstallNrCrashReport();
    std::fprintf(stderr, "NR_STAGE: production D3D12 runtime test\n");
    expect(argc == 2, "Pass a temporary directory containing the test forwarder");
    const auto directory = std::filesystem::absolute(argv[1]);
    // Only the fake forwarder uses this empty placeholder; no proprietary runtime is downloaded.
    std::ofstream(directory / "nvngx_dlssnr.dll").put('\0');
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
    {
        debug->EnableDebugLayer();
        std::fprintf(stderr, "NR_STAGE: D3D12 debug validation enabled\n");
    }
    ComPtr<ID3D12Device> device;
    std::fprintf(stderr, "NR_STAGE: creating D3D12 WARP device\n");
    check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> diagnostics;
    device.As(&diagnostics);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc {};
    check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> commands;
    check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&commands)));
    check(commands->Close());
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE completed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    expect(completed != nullptr, "CreateEvent");
    UINT64 epoch = 0;
    auto submit = [&]
    {
        check(commands->Close());
        ID3D12CommandList* lists[] = { commands.Get() };
        queue->ExecuteCommandLists(1, lists);
        check(queue->Signal(fence.Get(), ++epoch));
        check(fence->SetEventOnCompletion(epoch, completed));
        expect(WaitForSingleObject(completed, 60000) == WAIT_OBJECT_0, "GPU completion timeout");
        DlssNr::Submission::Collect();
    };
    float capabilityProbe = 0;
    DlssNr::Settings settings;
    DlssNr::Frame frame;
    frame.handle = 42;
    frame.directory = directory;
    frame.commands = commands.Get();
    frame.capabilityParameters = &capabilityProbe;
    frame.readFloat = [](void* p, const char*, float* out)
    {
        *out = *static_cast<float*>(p);
        return true;
    };

    auto run = [&](unsigned width, unsigned height, bool exact, bool edited = false, const ColourCase& colour = float32)
    {
        check(allocator->Reset());
        check(commands->Reset(allocator.Get(), nullptr));
        D3D12_HEAP_PROPERTIES gpu {};
        gpu.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Format = colour.format;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> output, motion;
        check(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&output)));
        auto motionDesc = desc;
        motionDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        check(device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &motionDesc,
                                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                                              IID_PPV_ARGS(&motion)));
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_RESOURCE_DESC buffer {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES cpu {};
        cpu.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> upload, readback;
        check(device->CreateCommittedResource(&cpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&upload)));
        cpu.Type = D3D12_HEAP_TYPE_READBACK;
        check(device->CreateCommittedResource(&cpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                              nullptr, IID_PPV_ARGS(&readback)));
        void* memory = nullptr;
        D3D12_RANGE noRead { 0, 0 };
        check(upload->Map(0, &noRead, &memory));
        const auto pixel = colour.Read(colour.pixel.data());
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
                std::memcpy(static_cast<char*>(memory) + y * footprint.Footprint.RowPitch + x * colour.pixel.size(),
                            colour.pixel.data(), colour.pixel.size());
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION from {}, to {};
        from.pResource = upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprint;
        to.pResource = output.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        barrier(commands.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        frame.output = output.Get();
        frame.motion = motion.Get();
        frame.renderWidth = width;
        frame.renderHeight = height;
        frame.hdr = colour.hdr;
        std::fprintf(stderr, "NR_STAGE: runtime frame %ux%u enabled=%u DXGI_FORMAT=%u\n", width, height,
                     settings.enabled, colour.format);
        DlssNr::Process(frame, settings);
        std::fprintf(stderr, "NR_STAGE: %s\n", DlssNr::GetStatus().c_str());
        barrier(commands.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        from.pResource = output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        submit();
        D3D12_RANGE range { 0, static_cast<SIZE_T>(bytes) };
        check(readback->Map(0, &range, &memory));
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; ++x)
            {
                const auto bytes =
                    static_cast<const uint8_t*>(memory) + y * footprint.Footprint.RowPitch + x * colour.pixel.size();
                const auto actual = colour.Read(bytes);
                if (exact)
                    expect(std::memcmp(bytes, colour.pixel.data(), colour.pixel.size()) == 0,
                           "Bypass changed the original frame");
                else if (edited)
                    expect(std::abs(actual[0] - pixel[0]) > 0.01f || std::abs(actual[1] - pixel[1]) > 0.01f ||
                               std::abs(actual[2] - pixel[2]) > 0.01f,
                           "Synthetic edit did not reach the output");
                else
                    for (unsigned c = 0; c < 4; ++c)
                        expect(std::isfinite(actual[c]) &&
                                   std::abs(actual[c] - pixel[c]) < (colour.componentBits == 11 ? 0.02f : 0.005f),
                               "Identity model changed a pixel or left a dispatch edge unwritten");
            }
        readback->Unmap(0, &noRead);
        if (diagnostics)
        {
            for (UINT64 i = 0; i < diagnostics->GetNumStoredMessages(); ++i)
            {
                SIZE_T length = 0;
                check(diagnostics->GetMessage(i, nullptr, &length));
                std::vector<uint8_t> storage(length);
                auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                check(diagnostics->GetMessage(i, message, &length));
                if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                    message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
                    throw std::runtime_error(message->pDescription);
            }
            diagnostics->ClearStoredMessages();
        }
    };

    run(17, 19, true);
    expect(!GetModuleHandleW(L"nvngx.dll_dlssnr.dll"), "Disabled NR loaded a backend");
    settings.enabled = true;
    for (int i = 0; i < 5; ++i)
        run(17, 19, false);
    auto dll = GetModuleHandleW(L"nvngx.dll_dlssnr.dll");
    expect(dll != nullptr, "The test backend never loaded");
    auto evaluations = reinterpret_cast<int (*)()>(GetProcAddress(dll, "nr_test_evaluations"));
    auto features = reinterpret_cast<int (*)()>(GetProcAddress(dll, "nr_test_live_features"));
    expect(evaluations && features && evaluations() == 5, "Not all enabled frames executed the model");
    settings.enabled = false;
    run(17, 19, true);
    expect(evaluations() == 5 && features() == 0, "Disable executed or retained an idle model");
    settings.enabled = true;
    run(23, 21, false); // resolution changes rebuild the session
    expect(evaluations() == 6, "Resize did not execute NR");
    auto edit = reinterpret_cast<void (*)(int)>(GetProcAddress(dll, "nr_test_edit"));
    expect(edit != nullptr, "Missing synthetic edit control");
    edit(1);
    run(23, 21, false, true);
    expect(evaluations() == 7, "Edited model frame did not execute");
    edit(0);
    settings.strength = 0;
    run(23, 21, true);
    expect(evaluations() == 7 && features() == 0, "Zero strength did not bypass NR exactly");
    settings.strength = 1;
    settings.passes = 3;
    // Only the first model edits; the later models copy their inputs. The edit can reach the
    // final output only if the host binds raw/proxy correctly at each multipass boundary.
    edit(2);
    run(23, 21, true); // the debounce bypasses while the model settings settle
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    run(23, 21, false, true);
    expect(evaluations() == 10, "The three-pass stack did not execute every model");
    settings.feedback = 0;
    run(23, 21, false);
    expect(evaluations() == 13, "Zero feedback did not execute all passes");
    settings.feedback = 1;
    edit(0);
    settings.scale = 0.5f;
    run(23, 21, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    run(23, 21, false);
    expect(evaluations() == 16, "Reduced-resolution multipass did not execute");
    settings.enabled = false;
    run(23, 21, true);
    std::filesystem::remove(directory / "nvngx_dlssnr.dll");
    settings.enabled = true;
    DlssNr::Retry();
    run(23, 21, true);
    expect(evaluations() == 16, "Missing model did not bypass");

    std::ofstream(directory / "nvngx_dlssnr.dll").put('\0');
    DlssNr::Retry();
    settings = {};
    settings.enabled = true;
    run(17, 19, true); // restore the one-pass/full-resolution model after debounce
    std::this_thread::sleep_for(std::chrono::milliseconds(550));
    const auto half = Bytes(std::array<uint16_t, 4> { 0x3400, 0x3800, 0x3a00, 0x3c00 });
    const auto unorm16 = Bytes(std::array<uint16_t, 4> { 16384, 32768, 49151, 65535 });
    const auto unorm8 = Bytes(std::array<uint8_t, 4> { 64, 128, 192, 255 });
    const auto unorm10 = Bytes(uint32_t { 256u | (512u << 10) | (767u << 20) | (3u << 30) });
    const auto packedFloat = Bytes(uint32_t { (13u << 6) | ((14u << 6) << 11) | (((14u << 5) | 16u) << 22) });
    const auto packedHdr = Bytes(uint32_t { (16u << 6) | ((14u << 6) << 11) | (((14u << 5) | 16u) << 22) });
    for (const auto& colour : std::vector<ColourCase> { { DXGI_FORMAT_R32G32B32A32_TYPELESS, float32.pixel, 32 },
                                                        { DXGI_FORMAT_R16G16B16A16_FLOAT, half, 16 },
                                                        { DXGI_FORMAT_R16G16B16A16_TYPELESS, half, 16 },
                                                        { DXGI_FORMAT_R16G16B16A16_UNORM, unorm16, 64 },
                                                        { DXGI_FORMAT_R8G8B8A8_UNORM, unorm8, 8 },
                                                        { DXGI_FORMAT_R8G8B8A8_TYPELESS, unorm8, 8 },
                                                        { DXGI_FORMAT_R10G10B10A2_UNORM, unorm10, 10 },
                                                        { DXGI_FORMAT_R10G10B10A2_TYPELESS, unorm10, 10 },
                                                        { DXGI_FORMAT_R11G11B10_FLOAT, packedFloat, 11 },
                                                        { DXGI_FORMAT_R11G11B10_FLOAT, packedHdr, 11, true } })
    {
        settings.enabled = false;
        run(17, 19, true, false, colour);
        settings.enabled = true;
        const auto before = evaluations();
        run(17, 19, false, false, colour);
        expect(evaluations() == before + 1, "Supported colour format bypassed the model");
        edit(1);
        run(17, 19, false, true, colour);
        expect(evaluations() == before + 2, "Edited colour format bypassed the model");
        edit(0);
    }
    const auto beforeUnsupported = evaluations();
    run(17, 19, true, false, { DXGI_FORMAT_R8G8B8A8_UINT, unorm8, 8 });
    expect(evaluations() == beforeUnsupported && DlssNr::GetStatus().find("DXGI_FORMAT=30") != std::string::npos,
           "Unsupported format did not preserve the frame and report its exact DXGI code");
    DlssNr::ReleaseAll();
    CloseHandle(completed);
    std::puts("PASS: WARP recording/readback, composition, edge coverage, reuse, resize, disable, missing-model "
              "bypass and typed/typeless/packed colour formats (synthetic backend)");
    return 0;
}
catch (const std::exception& e)
{
    std::fprintf(stderr, "FAIL: %s\n", e.what());
    return 1;
}
