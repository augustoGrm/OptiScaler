#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <wrl/client.h>
#include <memory>

namespace DlssNr::Submission
{
struct Flight
{
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::shared_ptr<void> payload;
    // Fence value 1 is signalled only AFTER the real game list is submitted.
    bool Complete() const { return fence->GetCompletedValue() >= 1; }
};
bool Install(ID3D12CommandQueue* queue);
std::shared_ptr<Flight> Retain(ID3D12GraphicsCommandList* list, ID3D12Device* device, std::shared_ptr<void> payload);
void Collect();
} // namespace DlssNr::Submission
