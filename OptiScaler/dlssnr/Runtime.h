#pragma once
#include "Settings.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <filesystem>
#include <string>

namespace DlssNr
{
struct Frame
{
    uint32_t handle = 0;
    ID3D12GraphicsCommandList* commands = nullptr;
    ID3D12Resource* output = nullptr; // arrives and returns in UNORDERED_ACCESS
    ID3D12Resource* depth = nullptr;  // optional; guides remain NON_PIXEL_SHADER_RESOURCE
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* exposure = nullptr;
    uint32_t renderWidth = 0, renderHeight = 0;
    uint32_t depthX = 0, depthY = 0, motionX = 0, motionY = 0;
    bool lowResMotion = false, depthInverted = false, hdr = false, reset = false;
    float mvScaleX = 1, mvScaleY = 1, preExposure = 1;
    void* capabilityParameters = nullptr; // borrowed; never destroy the driver's shared block
    bool (*readFloat)(void*, const char*, float*) = nullptr;
    std::filesystem::path directory;
    std::wstring dataPath;
};

// Records on the host list; the output is committed only after every NR stage succeeds.
void Process(const Frame& frame, const Settings& settings);
void Release(uint32_t handle);
void ReleaseAll();
void Retry();
std::string GetStatus();
} // namespace DlssNr
