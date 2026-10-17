#include "pch.h"
#include "Integration.h"
#include "Runtime.h"
#include <Util.h>
#include <proxies/NVNGX_Proxy.h>
#include <upscalers/IFeature_Dx12.h>

namespace DlssNr
{
void AfterUpscale(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* parameters, IFeature_Dx12* feature,
                  ID3D12Resource* output)
{
    const auto settings = GetSettings();
    if (!ShouldRun(settings))
    {
        // Poll completion and retire disabled sessions, without probing/loading NGX or the model.
        Process({}, settings);
        return;
    }
    if (!commands || !parameters || !feature)
        return;
    if (State::Instance().swapchainInteropApi != SwapchainInteropApi::None)
        return;
    switch (State::Instance().currentInputApiName)
    {
    case ApiUpscalerInput::DLSS_DX12:
    case ApiUpscalerInput::XeSS_DX12:
    case ApiUpscalerInput::FFX_DX12:
    case ApiUpscalerInput::FSR20_DX12:
    case ApiUpscalerInput::FSR2X_DX12:
    case ApiUpscalerInput::FSR3_DX12:
        break;
    default:
        return; // DX11/Vulkan bridge integration is a separate stage.
    }
    Frame frame;
    frame.handle = feature->Handle()->Id;
    frame.commands = commands;
    // IFeature temporarily points parameters.Output at the first RCAS/scaling work texture.
    // The caller supplies the final pipeline destination instead of that intermediate.
    frame.output = output;
    parameters->Get(NVSDK_NGX_Parameter_Depth, &frame.depth);
    parameters->Get(NVSDK_NGX_Parameter_MotionVectors, &frame.motion);
    if (!frame.depth)
        parameters->Get("DLSSD.Depth", &frame.depth);
    if (!frame.motion)
        parameters->Get("DLSSD.MotionVectors", &frame.motion);
    parameters->Get(NVSDK_NGX_Parameter_ExposureTexture, &frame.exposure);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &frame.renderWidth);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &frame.renderHeight);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, &frame.depthX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, &frame.depthY);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.motionX);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, &frame.motionY);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.mvScaleX);
    parameters->Get(NVSDK_NGX_Parameter_MV_Scale_Y, &frame.mvScaleY);
    parameters->Get(NVSDK_NGX_Parameter_DLSS_Pre_Exposure, &frame.preExposure);
    unsigned reset = 0;
    parameters->Get(NVSDK_NGX_Parameter_Reset, &reset);
    frame.reset = reset != 0;
    frame.depthInverted = feature->DepthInverted();
    frame.lowResMotion = feature->LowResMV();
    frame.hdr = feature->IsHdr();
    // AutoExposure means the game did not provide a usable exposure image, even if it bound a placeholder.
    if (feature->GetFeatureFlags() & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)
        frame.exposure = nullptr;
    if (frame.exposure)
    {
        auto desc = frame.exposure->GetDesc();
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
            (desc.Format != DXGI_FORMAT_R32_FLOAT && desc.Format != DXGI_FORMAT_R16_FLOAT))
            frame.exposure = nullptr;
    }
    frame.directory = Util::DllPath().parent_path();
    frame.dataPath = State::Instance().NVNGX_ApplicationDataPath;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (SUCCEEDED(commands->GetDevice(IID_PPV_ARGS(&device))) &&
        (NVNGXProxy::IsDx12Inited() || NVNGXProxy::InitDx12(device.Get())) &&
        NVNGXProxy::D3D12_GetCapabilityParameters())
    {
        NVSDK_NGX_Parameter* capabilities = nullptr;
        if (NVNGXProxy::D3D12_GetCapabilityParameters()(&capabilities) == NVSDK_NGX_Result_Success)
            frame.capabilityParameters = capabilities;
    }
    frame.readFloat = [](void* p, const char* key, float* value)
    { return static_cast<NVSDK_NGX_Parameter*>(p)->Get(key, value) == NVSDK_NGX_Result_Success; };
    Process(frame, settings);
}
} // namespace DlssNr
