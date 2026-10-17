// Test-only identity backend. Never ship this DLL or call it NVIDIA/model validation.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <atomic>
#include <wrl/client.h>

namespace
{
std::atomic<int> evaluations { 0 }, liveFeatures { 0 };
std::atomic<bool> applyEdit { false };
struct Feature
{
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpuHeap;
};
void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    cmd->ResourceBarrier(1, &b);
}
} // namespace
extern "C"
{
    __declspec(dllexport) int nr_test_evaluations() { return evaluations; }
    __declspec(dllexport) int nr_test_live_features() { return liveFeatures; }
    __declspec(dllexport) void nr_test_edit(int enabled) { applyEdit = enabled != 0; }
    __declspec(dllexport) void dlssnr_call_set_float_slot(int) {}
    __declspec(dllexport) void dlssnr_call_probe_float(void* p, const char*, float value, int slot)
    {
        if (slot == 6)
            *static_cast<float*>(p) = value;
    }
    __declspec(dllexport) const char* dlssnr_call_error() { return "test identity backend failure"; }
    __declspec(dllexport) void dlssnr_call_set_extras(void*, float, ID3D12Resource*, ID3D12Resource*, ID3D12Resource*,
                                                      unsigned, unsigned, unsigned, unsigned)
    {
    }
    __declspec(dllexport) void* dlssnr_call_create(const wchar_t*, const wchar_t*, ID3D12Device* device,
                                                   ID3D12GraphicsCommandList*, void*, unsigned, unsigned, int, float,
                                                   int, float, float, float, int, int)
    {
        auto feature = new Feature;
        feature->device = device;
        D3D12_DESCRIPTOR_HEAP_DESC heap {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 1;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&feature->heap))))
        {
            delete feature;
            return nullptr;
        }
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&feature->cpuHeap))))
        {
            delete feature;
            return nullptr;
        }
        ++liveFeatures;
        return feature;
    }
    __declspec(dllexport) void dlssnr_call_release(void* feature)
    {
        delete static_cast<Feature*>(feature);
        --liveFeatures;
    }
    __declspec(dllexport) int dlssnr_call_evaluate_v2(ID3D12GraphicsCommandList* cmd, void* handle, void*,
                                                      ID3D12Resource* colour, ID3D12Resource*, ID3D12Resource*,
                                                      ID3D12Resource* output, unsigned, unsigned, unsigned, unsigned,
                                                      unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, int,
                                                      int, float, int, float, float, float, int, float, float)
    {
        ++evaluations;
        if (applyEdit)
        {
            auto* feature = static_cast<Feature*>(handle);
            D3D12_UNORDERED_ACCESS_VIEW_DESC view {};
            view.Format = output->GetDesc().Format;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            feature->device->CreateUnorderedAccessView(output, nullptr, &view,
                                                       feature->heap->GetCPUDescriptorHandleForHeapStart());
            feature->device->CreateUnorderedAccessView(output, nullptr, &view,
                                                       feature->cpuHeap->GetCPUDescriptorHandleForHeapStart());
            ID3D12DescriptorHeap* heaps[] = { feature->heap.Get() };
            cmd->SetDescriptorHeaps(1, heaps);
            const float edited[] = { 0.9f, 0.25f, 0.1f, 1.0f };
            cmd->ClearUnorderedAccessViewFloat(feature->heap->GetGPUDescriptorHandleForHeapStart(),
                                               feature->cpuHeap->GetCPUDescriptorHandleForHeapStart(), output, edited,
                                               0, nullptr);
            return 1;
        }
        transition(cmd, colour, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(output, colour);
        transition(cmd, colour, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        transition(cmd, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return 1;
    }
}
