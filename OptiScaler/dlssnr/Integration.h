#pragma once
#include <cstdint>

struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct NVSDK_NGX_Parameter;
class IFeature_Dx12;

namespace DlssNr
{
void AfterUpscale(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* parameters, IFeature_Dx12* feature,
                  ID3D12Resource* output);
void Release(uint32_t handle);
void ReleaseAll();
} // namespace DlssNr
