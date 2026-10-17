#include "Submission.h"
#include <detours/detours.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace DlssNr::Submission
{
namespace
{
using Execute = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
struct Tracker
{
    std::recursive_mutex mutex;
    Execute original = nullptr;
    Execute target = nullptr;
    std::unordered_map<ID3D12CommandList*, std::vector<std::shared_ptr<Flight>>> pending;
    std::vector<std::shared_ptr<Flight>> submitted;
    std::unordered_map<ID3D12Device*, Microsoft::WRL::ComPtr<ID3D12Fence>> previous;
    bool broken = false;
    std::atomic<size_t> pendingCount { 0 };
};
// Process lifetime: pending command lists must never lose resources during DLL/static teardown.
Tracker& State()
{
    static auto* tracker = new Tracker;
    return *tracker;
}

void STDMETHODCALLTYPE ExecuteLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
{
    auto& t = State();
    if (t.pendingCount.load(std::memory_order_acquire) == 0)
    {
        t.original(queue, count, lists);
        return;
    }
    std::lock_guard lock(t.mutex);
    std::vector<std::shared_ptr<Flight>> batch;
    for (UINT i = 0; i < count; ++i)
    {
        auto found = t.pending.find(lists[i]);
        if (found != t.pending.end())
        {
            batch.insert(batch.end(), found->second.begin(), found->second.end());
            t.pendingCount.fetch_sub(found->second.size(), std::memory_order_release);
            t.pending.erase(found);
        }
    }
    // Only lists carrying NR work acquire dependencies; FG and other submissions pass through.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (!batch.empty())
        queue->GetDevice(IID_PPV_ARGS(&device));
    auto& previous = t.previous[device.Get()];
    if (!batch.empty() && previous && previous->GetCompletedValue() < 1)
        if (FAILED(queue->Wait(previous.Get(), 1)))
            t.broken = true;
    t.original(queue, count, lists);
    for (auto& flight : batch)
    {
        // If Signal fails, keep the flight forever rather than free live GPU resources.
        if (SUCCEEDED(queue->Signal(flight->fence.Get(), 1)))
            previous = flight->fence;
        else
            t.broken = true;
        t.submitted.push_back(std::move(flight));
    }
}
} // namespace

bool Install(ID3D12CommandQueue* queue)
{
    auto& t = State();
    std::lock_guard lock(t.mutex);
    auto address = reinterpret_cast<Execute>((*reinterpret_cast<void***>(queue))[10]);
    if (t.original)
        return t.target == address && !t.broken;
    t.target = address;
    t.original = address;
    if (DetourTransactionBegin() != NO_ERROR)
    {
        t.original = nullptr;
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    if (DetourAttach(reinterpret_cast<PVOID*>(&t.original), reinterpret_cast<PVOID>(ExecuteLists)) != NO_ERROR)
    {
        DetourTransactionAbort();
        t.original = nullptr;
        return false;
    }
    if (DetourTransactionCommit() == NO_ERROR)
        return true;
    t.original = nullptr;
    return false;
}

std::shared_ptr<Flight> Retain(ID3D12GraphicsCommandList* list, ID3D12Device* device, std::shared_ptr<void> payload)
{
    auto& t = State();
    std::lock_guard lock(t.mutex);
    size_t pendingCount = t.submitted.size();
    for (const auto& entry : t.pending)
        pendingCount += entry.second.size();
    // Bound memory if a game abandons recorded lists, or a queue wrapper hides their identity.
    if (!t.original || t.broken || pendingCount >= 8)
        return {};
    auto flight = std::make_shared<Flight>();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flight->fence))))
        return {};
    flight->payload = std::move(payload);
    t.pending[list].push_back(flight);
    t.pendingCount.fetch_add(1, std::memory_order_release);
    return flight;
}

void Collect()
{
    auto& t = State();
    std::lock_guard lock(t.mutex);
    std::erase_if(t.submitted, [](const auto& flight) { return flight->Complete(); });
    std::erase_if(t.previous,
                  [](const auto& entry) { return !entry.second || entry.second->GetCompletedValue() >= 1; });
}
} // namespace DlssNr::Submission
