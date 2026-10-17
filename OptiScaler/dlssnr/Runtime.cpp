#include "Runtime.h"
#include "Submission.h"
#include <shaders/dlssnr/DlssNr_Common.h>
#include <shaders/dlssnr/DlssNr_ColourEncoding.h>
#include <shaders/dlssnr/DlssNr_Guides.h>
#include <shaders/dlssnr/precompile/DlssNr_Shader.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace DlssNr
{
namespace
{
using Microsoft::WRL::ComPtr;
void Check(HRESULT result)
{
    if (FAILED(result))
        throw std::runtime_error("D3D12 operation failed (HRESULT " + std::to_string(static_cast<unsigned>(result)) +
                                 ")");
}

// Match the base shader chain's FLOAT/UNORM interpretation of typeless colour families.
// Keep integer/depth/sRGB resources out: the shared shader expects float colour values.
DXGI_FORMAT ColourViewFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return format;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

std::string FormatDiagnostic(DXGI_FORMAT format)
{
    return "DXGI_FORMAT=" + std::to_string(static_cast<unsigned>(format));
}

struct Backend
{
    HMODULE library = nullptr;
    std::shared_ptr<void> lifetime;
    using Create = void* (*) (const wchar_t*, const wchar_t*, ID3D12Device*, ID3D12GraphicsCommandList*, void*,
                              unsigned, unsigned, int, float, int, float, float, float, int, int);
    using Evaluate = int (*)(ID3D12GraphicsCommandList*, void*, void*, ID3D12Resource*, ID3D12Resource*,
                             ID3D12Resource*, ID3D12Resource*, unsigned, unsigned, unsigned, unsigned, unsigned,
                             unsigned, unsigned, unsigned, unsigned, unsigned, int, int, float, int, float, float,
                             float, int, float, float);
    Create create = nullptr;
    Evaluate evaluate = nullptr;
    void (*release)(void*) = nullptr;
    void (*floatSlot)(int) = nullptr;
    void (*probe)(void*, const char*, float, int) = nullptr;
    void (*extras)(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*, unsigned, unsigned, unsigned,
                   unsigned) = nullptr;
    const char* (*error)() = nullptr;

    explicit Backend(const Frame& f)
    {
        library = LoadLibraryExW((f.directory / L"nvngx.dll_dlssnr.dll").c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!library)
            throw std::runtime_error("nvngx.dll_dlssnr.dll is missing or cannot be loaded beside OptiScaler");
        lifetime = std::shared_ptr<void>(library, [](void* module) { FreeLibrary(static_cast<HMODULE>(module)); });
#define NR_EXPORT(field, name) field = reinterpret_cast<decltype(field)>(GetProcAddress(library, name))
        NR_EXPORT(create, "dlssnr_call_create");
        NR_EXPORT(evaluate, "dlssnr_call_evaluate_v2");
        NR_EXPORT(release, "dlssnr_call_release");
        NR_EXPORT(floatSlot, "dlssnr_call_set_float_slot");
        NR_EXPORT(probe, "dlssnr_call_probe_float");
        NR_EXPORT(extras, "dlssnr_call_set_extras");
        NR_EXPORT(error, "dlssnr_call_error");
#undef NR_EXPORT
        if (!create || !evaluate || !release || !floatSlot || !probe || !extras || !error)
            throw std::runtime_error("Incompatible NR forwarder: rebuild it from this source tree");
        if (!f.readFloat || !f.capabilityParameters)
            throw std::runtime_error("NGX capability parameters are unavailable");
        bool found = false;
        for (int slot : { 1, 2, 5, 6, 7, 4, 3, 0 })
        {
            float value = 0;
            probe(f.capabilityParameters, "DLSSNR.OptiScalerFloatProbe", 0.375f, slot);
            if (f.readFloat(f.capabilityParameters, "DLSSNR.OptiScalerFloatProbe", &value) && value == 0.375f)
            {
                floatSlot(slot);
                found = true;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("Cannot determine this NGX parameter block's float ABI");
    }
    // Forwarder/model modules remain loaded for process lifetime, like the host's NGX proxy.
};

struct Texture
{
    ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
};

void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    cmd->ResourceBarrier(1, &barrier);
}
void To(ID3D12GraphicsCommandList* cmd, Texture& texture, D3D12_RESOURCE_STATES state)
{
    Barrier(cmd, texture.resource.Get(), texture.state, state);
    texture.state = state;
}
void UavBarrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    cmd->ResourceBarrier(1, &barrier);
}

Texture MakeTexture(ID3D12Device* device, unsigned w, unsigned h, DXGI_FORMAT format)
{
    Texture texture;
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, texture.state, nullptr,
                                          IID_PPV_ARGS(&texture.resource)));
    return texture;
}

struct Surfaces
{
    Texture proxy, original, reducedProxy, scratch, scratch2, resolved;
    std::array<Texture, 3> answers;
    ComPtr<ID3D12Resource> constants;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    std::weak_ptr<Submission::Flight> flight;
    unsigned stage = 0;
};

struct Session
{
    Settings tuning;
    unsigned width = 0, height = 0, workWidth = 0, workHeight = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT viewFormat = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12CommandQueue> initQueue;
    ComPtr<ID3D12CommandAllocator> initAllocator;
    ComPtr<ID3D12GraphicsCommandList> initList;
    ComPtr<ID3D12Fence> initFence;
    HANDLE initEvent = nullptr;
    std::shared_ptr<Backend> backend;
    std::array<void*, 3> features {};
    std::vector<std::shared_ptr<Surfaces>> pool;
    bool reset = true;
    ~Session()
    {
        if (backend)
            for (auto feature : features)
                if (feature)
                    backend->release(feature);
        if (initEvent)
            CloseHandle(initEvent);
    }
};

struct Payload
{
    std::shared_ptr<Session> session;
    std::shared_ptr<Surfaces> surfaces;
    ComPtr<ID3D12Resource> output, depth, motion, exposure;
};

struct Failure
{
    Settings settings;
    UINT64 width;
    UINT height;
    DXGI_FORMAT format;
};

struct Module
{
    std::recursive_mutex mutex;
    std::unordered_map<uint32_t, std::shared_ptr<Session>> sessions;
    std::unordered_map<uint32_t, Failure> failed;
    // Keep one ABI discovery per loaded forwarder. Reprobing a shared parameter block can read
    // a stale successful probe left by an earlier session and select the wrong setter slot.
    std::unordered_map<std::wstring, std::shared_ptr<Backend>> backends;
    std::vector<std::shared_ptr<Session>> quarantine;
    Settings lastSettings;
    std::chrono::steady_clock::time_point changed {};
    std::string status = "Off";
};
Module& State()
{
    static auto* state = new Module;
    return *state;
}

bool SameModel(const Settings& a, const Settings& b)
{
    return a.preset == b.preset && a.style == b.style && a.passes == b.passes && a.intensity == b.intensity &&
           a.localStructure == b.localStructure && a.localTone == b.localTone && a.skinStructure == b.skinStructure &&
           a.autoMask == b.autoMask && a.scale == b.scale;
}

std::shared_ptr<Session> CreateSession(const Frame& frame, const Settings& settings, ID3D12Device* device)
{
    auto session = std::make_shared<Session>();
    auto& s = *session;
    s.device = device;
    s.tuning = settings;
    const auto desc = frame.output->GetDesc();
    s.width = static_cast<unsigned>(desc.Width);
    s.height = desc.Height;
    s.workWidth = std::max(1u, static_cast<unsigned>(s.width * settings.scale));
    s.workHeight = std::max(1u, static_cast<unsigned>(s.height * settings.scale));
    s.format = desc.Format;
    s.viewFormat = ColourViewFormat(s.format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { s.viewFormat };
    Check(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)));
    const auto required = D3D12_FORMAT_SUPPORT1_TEXTURE2D | D3D12_FORMAT_SUPPORT1_SHADER_LOAD |
                          D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW;
    if ((support.Support1 & required) != required || !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
        throw std::runtime_error("GPU cannot read/store the NR colour view (" + FormatDiagnostic(s.format) + ")");

    D3D12_DESCRIPTOR_RANGE ranges[3] = { { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0 },
                                         { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0 },
                                         { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 0 } };
    D3D12_ROOT_PARAMETER parameters[3] {};
    for (unsigned i = 0; i < 3; ++i)
    {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable = { 1, &ranges[i] };
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc { 3, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ComPtr<ID3DBlob> blob, errors;
    Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors));
    Check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s.root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline {};
    pipeline.pRootSignature = s.root.Get();
    pipeline.CS = { DlssNr_cso, sizeof(DlssNr_cso) };
    Check(device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&s.pipeline)));
    auto& backend = State().backends[frame.directory.wstring()];
    if (!backend)
        backend = std::make_shared<Backend>(frame);
    s.backend = backend;
    if (!std::filesystem::is_regular_file(frame.directory / L"nvngx_dlssnr.dll"))
        throw std::runtime_error("Place the separately obtained nvngx_dlssnr.dll beside OptiScaler");

    D3D12_COMMAND_QUEUE_DESC queue {};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(device->CreateCommandQueue(&queue, IID_PPV_ARGS(&s.initQueue)));
    if (!Submission::Install(s.initQueue.Get()))
        throw std::runtime_error("Cannot install the NR submission lifetime adapter");
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&s.initAllocator)));
    Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.initAllocator.Get(), nullptr,
                                    IID_PPV_ARGS(&s.initList)));
    Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.initFence)));

    std::string failure;
    for (unsigned i = 0; i < settings.passes; ++i)
    {
        // Clear optional UI resources on the shared block; never install Streamline tag hooks.
        s.backend->extras(frame.capabilityParameters, 0, nullptr, nullptr, nullptr, 0, 0, 0, 0);
        s.features[i] = s.backend->create(
            (frame.directory / L"nvngx_dlssnr.dll").c_str(), frame.dataPath.c_str(), device, s.initList.Get(),
            frame.capabilityParameters, s.workWidth, s.workHeight, settings.preset, settings.intensity, settings.style,
            settings.localStructure, settings.localTone, settings.skinStructure, settings.autoMask, 1);
        if (!s.features[i])
        {
            failure = s.backend->error();
            if (failure.empty())
                failure = "The model refused feature creation";
            break;
        }
    }
    // Even a failed create may have recorded GPU work. Submit and fence before releasing anything.
    Check(s.initList->Close());
    ID3D12CommandList* lists[] = { s.initList.Get() };
    s.initQueue->ExecuteCommandLists(1, lists);
    const HRESULT signalled = s.initQueue->Signal(s.initFence.Get(), 1);
    HANDLE ready = s.initEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT eventResult = ready ? s.initFence->SetEventOnCompletion(1, ready) : E_FAIL;
    const DWORD waited =
        SUCCEEDED(signalled) && SUCCEEDED(eventResult) ? WaitForSingleObject(ready, 30000) : WAIT_FAILED;
    if (waited != WAIT_OBJECT_0)
    {
        State().quarantine.push_back(session);
        throw std::runtime_error("Model initialization did not complete; resources retained safely for this process");
    }
    if (!failure.empty())
        throw std::runtime_error(failure);
    CloseHandle(s.initEvent);
    s.initEvent = nullptr;
    return session;
}

std::shared_ptr<Surfaces> Acquire(Session& s)
{
    for (auto& surfaces : s.pool)
        if (surfaces->flight.expired())
        {
            surfaces->stage = 0;
            return surfaces;
        }
    if (s.pool.size() >= 3)
        return {};
    auto surfaces = std::make_shared<Surfaces>();
    auto& r = *surfaces;
    r.proxy = MakeTexture(s.device.Get(), s.width, s.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    r.original = MakeTexture(s.device.Get(), s.width, s.height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    r.reducedProxy = MakeTexture(s.device.Get(), s.workWidth, s.workHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    r.scratch = MakeTexture(s.device.Get(), s.workWidth, s.workHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    r.scratch2 = MakeTexture(s.device.Get(), s.workWidth, s.workHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    // A typed sibling can be copied into the original typeless resource without changing its layout.
    r.resolved = MakeTexture(s.device.Get(), s.width, s.height, s.viewFormat);
    for (unsigned i = 0; i < s.tuning.passes; ++i)
        r.answers[i] = MakeTexture(s.device.Get(), s.workWidth, s.workHeight, DXGI_FORMAT_R16G16B16A16_FLOAT);
    D3D12_DESCRIPTOR_HEAP_DESC heap {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 8 * 8;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(s.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&r.descriptors)));
    D3D12_HEAP_PROPERTIES upload {};
    upload.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 8 * sizeof(DlssNrConstants);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Check(s.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                            nullptr, IID_PPV_ARGS(&r.constants)));
    s.pool.push_back(surfaces);
    return surfaces;
}

void Dispatch(Session& s, Surfaces& r, ID3D12GraphicsCommandList* cmd, const DlssNrConstants& constants,
              ID3D12Resource* source, ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* exposure,
              Texture& target, Texture* keep = nullptr)
{
    if (r.stage >= 8)
        throw std::runtime_error("NR descriptor stage budget exceeded");
    const UINT stride = s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto first = r.stage * 8;
    auto cpu = [&](UINT offset)
    {
        auto handle = r.descriptors->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(first + offset) * stride;
        return handle;
    };
    auto gpu = [&](UINT offset)
    {
        auto handle = r.descriptors->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(first + offset) * stride;
        return handle;
    };
    ID3D12Resource* srvs[] = { source, model, original, nullptr, exposure };
    for (unsigned i = 0; i < 5; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view {};
        view.Format = srvs[i] ? srvs[i]->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (srvs[i] == source && ColourViewFormat(view.Format) != DXGI_FORMAT_UNKNOWN)
            view.Format = ColourViewFormat(view.Format);
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        s.device->CreateShaderResourceView(srvs[i], &view, cpu(i));
    }
    Texture* uavs[] = { &target, keep ? keep : &target };
    for (unsigned i = 0; i < 2; ++i)
    {
        To(cmd, *uavs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
        view.Format = uavs[i]->resource->GetDesc().Format;
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        s.device->CreateUnorderedAccessView(uavs[i]->resource.Get(), nullptr, &view, cpu(5 + i));
    }
    void* mapped = nullptr;
    D3D12_RANGE read { 0, 0 };
    Check(r.constants->Map(0, &read, &mapped));
    std::memcpy(static_cast<char*>(mapped) + r.stage * sizeof(constants), &constants, sizeof(constants));
    r.constants->Unmap(0, nullptr);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cb { r.constants->GetGPUVirtualAddress() + r.stage * sizeof(constants),
                                         sizeof(constants) };
    s.device->CreateConstantBufferView(&cb, cpu(7));
    ++r.stage;
    ID3D12DescriptorHeap* heaps[] = { r.descriptors.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(s.root.Get());
    cmd->SetPipelineState(s.pipeline.Get());
    cmd->SetComputeRootDescriptorTable(0, gpu(0));
    cmd->SetComputeRootDescriptorTable(1, gpu(5));
    cmd->SetComputeRootDescriptorTable(2, gpu(7));
    cmd->Dispatch((constants.Width + 7) / 8, (constants.Height + 7) / 8, 1);
    UavBarrier(cmd, target.resource.Get());
    if (keep)
        UavBarrier(cmd, keep->resource.Get());
}
} // namespace

void Process(const Frame& f, const Settings& value)
{
    auto& state = State();
    std::lock_guard lock(state.mutex);
    Submission::Collect();
    const auto settings = Sanitize(value);
    if (!ShouldRun(settings))
    {
        state.sessions.clear();
        state.status = "Off: original upscaler output";
        return;
    }
    if (!f.commands || !f.output || !f.motion || f.commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        state.status = "Needs a native DX12 direct list, upscaler output and motion vectors";
        return;
    }
    const auto desc = f.output->GetDesc();
    if (const auto failed = state.failed.find(f.handle);
        failed != state.failed.end() && failed->second.settings == settings && failed->second.width == desc.Width &&
        failed->second.height == desc.Height && failed->second.format == desc.Format)
        return;
    const auto now = std::chrono::steady_clock::now();
    if (!SameModel(state.lastSettings, settings))
    {
        state.lastSettings = settings;
        state.changed = now;
    }
    if (now - state.changed < std::chrono::milliseconds(500))
    {
        state.status = "Waiting for model settings to settle";
        return;
    }
    bool colourReadable = false;
    try
    {
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.DepthOrArraySize != 1 || desc.MipLevels != 1 ||
            desc.SampleDesc.Count != 1 || desc.Width > 16384 || !desc.Width || !desc.Height ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            throw std::runtime_error("Unsupported output texture layout; original rendering retained");
        if (ColourViewFormat(desc.Format) == DXGI_FORMAT_UNKNOWN)
            throw std::runtime_error("Unsupported output colour format (" + FormatDiagnostic(desc.Format) +
                                     "); original rendering retained");
        ComPtr<ID3D12Device> device;
        Check(f.commands->GetDevice(IID_PPV_ARGS(&device)));
        auto& active = state.sessions[f.handle];
        if (!active || active->device.Get() != device.Get() || active->width != desc.Width ||
            active->height != desc.Height || active->format != desc.Format || !SameModel(active->tuning, settings))
            active = CreateSession(f, settings, device.Get());
        auto session = active;
        auto& s = *session;
        auto surfaces = Acquire(s);
        if (!surfaces)
        {
            s.reset = true;
            state.status = "NR skipped: waiting for GPU completion";
            return;
        }
        const auto motionDesc = f.motion->GetDesc();
        const auto depthDesc = f.depth ? f.depth->GetDesc() : D3D12_RESOURCE_DESC {};
        const auto regions = ResolveGuideRegions({ static_cast<unsigned>(depthDesc.Width), depthDesc.Height },
                                                 { static_cast<unsigned>(motionDesc.Width), motionDesc.Height },
                                                 { f.renderWidth, f.renderHeight }, { s.width, s.height },
                                                 f.lowResMotion, f.depthX, f.depthY, f.motionX, f.motionY);
        if (!regions.motion.valid() || (f.depth && !regions.depth.valid()))
            throw std::runtime_error("Invalid depth/motion subrectangle");
        auto payload = std::make_shared<Payload>();
        payload->session = session;
        payload->surfaces = surfaces;
        payload->output = f.output;
        payload->depth = f.depth;
        payload->motion = f.motion;
        payload->exposure = f.exposure;
        auto flight = Submission::Retain(f.commands, device.Get(), payload);
        if (!flight)
        {
            s.reset = true;
            state.status = "NR skipped: submission tracking is full (no buffers overwritten)";
            return;
        }
        auto& r = *surfaces;
        r.flight = flight;
        DlssNrConstants constants {};
        constants.WhitePoint = settings.whitePoint;
        constants.Width = s.width;
        constants.Height = s.height;
        constants.TransferStrength = settings.strength;
        constants.ColourStrength = settings.colourStrength;
        constants.MaxRatio = settings.maxRatio;
        constants.ApplyModel = 1;
        constants.Transfer = 1;
        constants.CompareZoom = 1;
        constants.DebugScale = 1;
        constants.ModelWorkScale = settings.scale;
        constants.PassFeedback = settings.feedback;
        constants.PreExposure = f.preExposure;
        constants.ExposureTrim = 1;
        constants.ExposureBaseScale = 1;
        const bool floatColour = s.viewFormat == DXGI_FORMAT_R16G16B16A16_FLOAT ||
                                 s.viewFormat == DXGI_FORMAT_R32G32B32A32_FLOAT ||
                                 s.viewFormat == DXGI_FORMAT_R11G11B10_FLOAT;
        const auto encoding = DlssNrColourEncoding::Resolve(settings.colourEncoding, f.hdr, floatColour);
        constants.Passthrough = encoding.LinearHdr() ? 0 : 1;
        constants.InputEncoding = static_cast<uint32_t>(encoding.encoding);
        constants.UseGameExposure =
            encoding.encoding == DlssNrColourEncoding::Encoding::LinearHdr && settings.gameExposure && f.exposure;

        Barrier(f.commands, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        colourReadable = true;
        constants.Mode = DlssNrMode_Encode;
        Dispatch(s, r, f.commands, constants, f.output, nullptr, nullptr, f.exposure, r.proxy, &r.original);
        Barrier(f.commands, f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        colourReadable = false;
        To(f.commands, r.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        To(f.commands, r.original, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Texture* initial = &r.proxy;
        if (s.workWidth != s.width || s.workHeight != s.height)
        {
            auto down = constants;
            down.Mode = DlssNrMode_Downsample;
            down.Width = s.workWidth;
            down.Height = s.workHeight;
            Dispatch(s, r, f.commands, down, r.proxy.resource.Get(), nullptr, nullptr, nullptr, r.reducedProxy);
            To(f.commands, r.reducedProxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            initial = &r.reducedProxy;
        }
        Texture* input = initial;
        for (unsigned pass = 0; pass < settings.passes; ++pass)
        {
            To(f.commands, r.answers[pass], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            s.backend->extras(f.capabilityParameters, 0, nullptr, nullptr, nullptr, 0, 0, 0, 0);
            const int result = s.backend->evaluate(
                f.commands, s.features[pass], f.capabilityParameters, input->resource.Get(), f.depth, f.motion,
                r.answers[pass].resource.Get(), s.workWidth, s.workHeight, regions.depth.width, regions.depth.height,
                regions.motion.width, regions.motion.height, regions.depth.x, regions.depth.y, regions.motion.x,
                regions.motion.y, f.depthInverted, f.reset || s.reset, settings.intensity, settings.style,
                settings.localStructure, settings.localTone, settings.skinStructure, settings.autoMask, f.mvScaleX,
                f.mvScaleY);
            if (result != 1)
                throw std::runtime_error(s.backend->error());
            UavBarrier(f.commands, r.answers[pass].resource.Get());
            To(f.commands, r.answers[pass], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (pass + 1 < settings.passes)
            {
                // Alternate the boundary's destination so it never aliases its input.
                auto& next = input == &r.scratch ? r.scratch2 : r.scratch;
                auto clamp = constants;
                clamp.Mode = DlssNrMode_ClampProxy;
                clamp.Width = s.workWidth;
                clamp.Height = s.workHeight;
                // Clamp mode reads the raw answer at t0 and this pass's own input proxy at t1.
                Dispatch(s, r, f.commands, clamp, r.answers[pass].resource.Get(), input->resource.Get(), nullptr,
                         nullptr, next);
                To(f.commands, next, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                input = &next;
            }
        }
        constants.Mode = DlssNrMode_Resolve;
        // The first proxy is the baseline for the entire stack, including all multipass edits.
        Dispatch(s, r, f.commands, constants, initial->resource.Get(), r.answers[settings.passes - 1].resource.Get(),
                 r.original.resource.Get(), f.exposure, r.resolved);
        To(f.commands, r.resolved, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(f.commands, f.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        f.commands->CopyResource(f.output, r.resolved.resource.Get());
        Barrier(f.commands, f.output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.reset = false;
        state.status = "DLSS-NR active: " + std::to_string(settings.passes) + " pass(es)";
    }
    catch (const std::exception& error)
    {
        if (colourReadable)
            Barrier(f.commands, f.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        state.status = std::string("NR bypassed: ") + error.what();
        state.failed[f.handle] = { settings, desc.Width, desc.Height, desc.Format };
        state.sessions.erase(f.handle);
    }
}

void Release(uint32_t handle)
{
    std::lock_guard lock(State().mutex);
    State().sessions.erase(handle);
    State().failed.erase(handle);
    Submission::Collect();
}
void ReleaseAll()
{
    std::lock_guard lock(State().mutex);
    State().sessions.clear();
    State().failed.clear();
    Submission::Collect();
}
void Retry() { ReleaseAll(); }
std::string GetStatus()
{
    std::lock_guard lock(State().mutex);
    return State().status;
}
} // namespace DlssNr
