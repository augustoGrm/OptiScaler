# Modular DLSS Neural Rendering

This branch adds an opt-in **native DX12 post-upscale** integration to Augusto's OptiScaler `master`,
based on `1be9b79682d0c2ab5b01f65db3be7dfa67e07fd1`. It uses the NR shader and model-call protocol of
janblade **v0.1.31**, commit `5d5cdccdbe671ab222d5e29fc7663e5115bc5d9b`.
At inspection, v0.1.31 was the newest release tag; `release/0.1.32` existed without a matching tag.

The model invocation, composition, menu and INI persistence are implemented. The complete solution
and forwarder have compiled with MSVC on Windows CI. **NVIDIA runtime/game compatibility and feature
parity with janblade's fork are not established.** This task's development machine is Linux with no
GPU; see the validation section for the actual checks and remaining hardware validation.

## Dependency analysis and scope

The fork's complete NR host has dependencies on native-input capture, optical flow, detail reuse,
exposure calibration, LUTs, residual SR/FG, alternate model loaders and API bridges. Importing that
host would pull changes into graphics hooks, frame generation, menu infrastructure and global
configuration. Those systems are unnecessary for an initial post-upscale integration.

This implementation retains the dependencies needed for a real model call:

```text
IFeature_Dx12::Evaluate (after RCAS/output scaling, before ImGui)
  -> dlssnr/Integration (OptiScaler/NGX metadata adapter)
     -> dlssnr/Runtime (one model session per upscaler handle)
        -> existing NVNGXProxy capability block (borrowed; never destroyed)
        -> dlssnr/forwarder -> separately supplied nvngx_dlssnr.dll, feature 18
        -> shaders/dlssnr: encode -> optional downsample -> model(s) -> resolve
        -> dlssnr/Submission: retain each recording until a real GPU fence completes

Config::Reload/SaveIni -> dlssnr/SettingsIni -> thread-safe Settings store
menu sidebar -> dlssnr/Menu -> Settings / runtime status
NGX ReleaseFeature/Shutdown -> NR session retirement
```

The shader, constants, guide-region calculations, colour-encoding selection and three upstream tests
come from the pinned tag. Imported C++ headers have formatting changes only; the feedback test adds
crash diagnostics and a published-bytecode variant, retaining all reference assertions. Compiled shader
bytes are identical. `OptiScaler/dlssnr/UPSTREAM.json` records original/local source checksums and
the shader-payload SHA-256 checksum.
The small forwarder is a DX12-only extraction of that tag, with its public call protocol retained.
The runtime host and settings adapter are local, rather than a copy of the fork's large host files.
The project licence applies; the shader's RenoDX MIT attribution is retained in
`Licenses/RenoDX_ATTRIBUTION.txt`. No NVIDIA runtime is redistributed.

Implemented controls include enable/disable, effect/colour strength, work resolution, 1–3 model
passes, feedback between passes, model preset/style/intensity, structure/tone/skin tuning, automatic
skin mask, colour encoding, game exposure with a fallback white point, and maximum brightening ratio.
All passes currently share one model profile. Tuning that requires new model features is debounced
for 500 ms; composition settings do not rebuild the model.

Deferred capabilities include DX11/Vulkan bridges, native/no-upscaler input, async-compute lists,
pre-SR placement, finished-picture/Present processing, LUTs, automatic exposure calibration, detail
reuse/edit cache, frame hold, custom per-pass profiles, and residual SR/FG experiments. These are
separate features, not prerequisites silently provided by copying the whole fork.

## Integration boundaries and original files changed

Only these six original files are changed; the initial patch adds 22 lines across them:

| Original file | Addition and reason |
| --- | --- |
| `OptiScaler/upscalers/IFeature_Dx12.cpp` | Include and one call before ImGui. Pass the final `paramOutput`, since the NGX block can still point at an intermediate RCAS/scaling texture. Runs inside the existing root-signature/descriptor-capture envelope. |
| `OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp` | Include plus release/shutdown calls. Drop CPU ownership of NR sessions without freeing resources still referenced by recorded/submitted work. |
| `OptiScaler/Config.cpp` | Include plus one read and one write call on its existing SimpleIni document. No new fields in `Config.h`; other sections remain under the original code's control. |
| `OptiScaler/menu/menu_common.cpp` | Include and a sidebar entry calling the module's own panel. Existing FG, latency and keybind panels stay intact. |
| `OptiScaler.sln` | Register the standalone forwarder and its three configurations so solution builds produce the complete package. |
| `OptiScaler/OptiScaler.vcxproj` | One import of `dlssnr/Module.props`, which owns all new build items and the forwarder project reference. The existing solution builds the reference transitively. |

There are no edits to `dllmain.cpp`, `FG_Hooks.cpp`, `Reflex_Hooks.cpp`, `D3D12_Hooks.cpp`, NVAPI,
DXGI hooks, Streamline proxies, ASI loading, FG replacement implementations, existing upscalers, or
shortcut definitions. **Do not set `FrameGen.External=true` to use NR.** FG remains controlled by the
original OptiScaler interface. This preserves the code paths; actual FG/plugin combinations still
require the Windows game checks below.

The integration accepts only native DX12 input routes passing through OptiScaler's `IFeature_Dx12`
pipeline (DLSS, XeSS, FFX and FSR input adapters). It does not process native NGX passthrough handles
that bypass this pipeline, FG evaluates, or DX11/Vulkan bridge evaluates.

## GPU ownership and rendering state

The host adapter uses the feature's original depth/HDR/MV flags, render subrectangle, separate
depth/MV offsets, motion-vector scales, reset and exposure metadata. It reads optional DLSSD guide
names as a fallback. No NR setting is written into FG/Reflex configuration.

Each frame first encodes the original into private textures, runs the model into private outputs,
then composes into a private final texture. The game output is copied back only after all CPU-side
recording stages succeed. Disabled NR and zero effect strength record no NR GPU commands and do not
load the model or initialize NGX. A rejected/missing model bypasses NR and exposes a status message;
use the retry button after supplying/fixing it.

The output arrives and returns in `UNORDERED_ACCESS`, as in the existing DX12 shader chain. Guides
and optional exposure are borrowed as `NON_PIXEL_SHADER_RESOURCE`, the existing upscaler contract.
The module's transitions and UAV barriers surround its own writes. It preserves game output state
on failure and never substitutes its success/failure for the upscaler's result.

The model's create work runs on a private initialization queue, then is fenced before use. A timed
out initialization is quarantined with its command objects/event rather than freed under the GPU.
Unlike frame-count retirement, the submission adapter detours only the queue's
`ExecuteCommandLists` method from inside the module: it recognizes lists containing NR commands,
signals fences **after the actual submission**, and retains features, buffers, constants, descriptors
and borrowed textures until completion. NR submissions on different queues of the same device are
ordered with GPU waits; unrelated submissions acquire no GPU waits or signals. Without pending NR
recordings the detour immediately delegates to the original method.

This queue adapter is necessary for safe retirement without changing core graphics hooks. It must be
rechecked with queue-wrapping middleware. If a wrapper hides command-list identity, recordings remain
retained and NR stops after a bounded number of frames instead of reusing live buffers. Each session
has at most three reusable frame slots, and the global tracker permits at most eight outstanding
recordings. Abandoned/unsubmitted lists conservatively retain their resources for process lifetime.
The first implementation hooks one queue-method implementation; an incompatible implementation is
rejected. Queue-submit failures stop accepting new NR work. These limitations are explicit costs of
keeping the adapter independent from upstream hook internals.

Model features and large textures consume VRAM, especially with multiple passes. Start with one pass
at 1.0 work scale; lower the scale if allocation fails. Supported output formats are
`R16G16B16A16_FLOAT`, `R32G32B32A32_FLOAT`, `R11G11B10_FLOAT`, `R16G16B16A16_UNORM`,
`R8G8B8A8_UNORM`, and `R10G10B10A2_UNORM`, plus the corresponding RGBA `TYPELESS` families.
Typeless RGBA16/32 uses FLOAT views; RGBA8/RGB10A2 uses UNORM views. The private final output is
typed and copied back within the original format family. The device must support shader reads and
typed UAV stores for that view. Only floating-point views select linear HDR when the game's HDR
flag is set. Textures must be single-sample, single-mip, single-layer 2D and readable as shader
resources. Other layouts/formats bypass NR; format rejections report the exact `DXGI_FORMAT` code.

## Configuration and installation

### Download a compiled Windows package

Open [Modular DLSS-NR validation](https://github.com/augustoGrm/OptiScaler/actions/workflows/dlssnr-validation.yml),
choose a successful run for `feature/modular-dlssnr`, and download
**OptiScaler-DLSSNR-v0.1.31-windows-x64** from **Artifacts** at the bottom of the run page.
GitHub requires signing in to download artifacts. Packages expire after 14 days; a new push or
manual workflow run rebuilds them. These are test builds from this branch, not a stable release.

Extract the entire ZIP. It contains the compiled `OptiScaler.dll`, `nvngx.dll_dlssnr.dll`, base
dependencies/configuration/setup scripts, and licences. Install the extracted files using the
normal OptiScaler procedure. Preserve an existing game's INI when updating an installation.
The package does not include NVIDIA's `nvngx_dlssnr.dll`; obtain the runtime matching your GPU
separately, as described below. You do not need Visual Studio when using this compiled package.

### Runtime and activation

Build and deploy the complete base OptiScaler package plus the forwarder produced by this branch.
Place `nvngx.dll_dlssnr.dll` and the separately obtained `nvngx_dlssnr.dll` beside the active
`OptiScaler.dll` (or its renamed proxy DLL). The forwarder's file name is part of the model-call
contract; do not rename it. No Streamline replacement DLLs are needed by this integration.

Use the runtime matching your GPU, following the pinned source's
[v0.1.31 runtime guide](https://github.com/janblade/OptiScaler-F5-DLSSNR-Multipass/blob/v0.1.31/INSTALL-DLSSNR.md).
The official Blackwell runtime and cross-generation compatibility runtimes have different hardware
requirements. Their successful loading and visual output are not established by the synthetic tests.

Open the existing OptiScaler overlay, select **Neural Rendering**, and enable the effect. Use the
existing Save configuration button. Minimal INI example (other settings use module defaults):

```ini
[DlssNr]
Enabled=false
PassCount=1
TransferStrength=1.0
ColourStrength=1.0
WorkScale=1.0
PassFeedback=1.0
GameExposure=true
WhitePoint=1.0
```

Deleting the section restores disabled defaults. Saving writes only this module's keys into its
section; SimpleIni retains unrelated/unknown sections and keys. Existing FG and shortcut values are
not rewritten by the module. Disabling preserves any in-flight GPU ownership until its fence completes.

## Build and validation

Windows x64 requires Visual Studio 2022 C++ tools (v143) and Windows SDK 10.0, as on the base branch:

```powershell
git submodule update --init --recursive
msbuild OptiScaler.sln /m:4 /p:Configuration=Release /p:Platform=x64 /verbosity:minimal
./tests/run_dlssnr_windows.ps1
```

The forwarder is built through `Module.props` into `x64/Release/a/`; Debug and ReleaseDebug outputs
go beside their corresponding main DLL. The main module uses C++20-compatible code under the base
project's existing language setting, while the standalone forwarder retains C++17. The NR shader's
RenoDX attribution is copied into the package's `Licenses/` directory after the base post-build step.

The Windows test script must run from an x64 MSVC developer environment. It runs three portable test
programs, the upstream D3D11 WARP feedback test against both published and source-compiled shaders,
and a D3D12 WARP test of the production runtime plus
submission adapter using a **synthetic backend** in a temporary directory. That last test checks
edge coverage on non-workgroup-aligned dimensions, repeated frame/resource reuse, model invocation
counts, a visible synthetic edit, resize, multipass at full/reduced scale, exact disabled/zero-work bypass, and missing-model
fallback. It also checks typed/typeless RGBA and packed R11G11B10 output readback, visible edits,
an HDR value above 1, byte-exact disabled output and unsupported-format diagnostics. D3D12 debug-layer
errors are checked when that layer is available. It does not execute NVIDIA's model. Never package the fake test DLL. The GitHub Actions
workflow `Modular DLSS-NR validation` supplies Linux and full Windows build/test jobs, pinned to
`windows-2022`. Its manual dispatch can select `windows-latest` to reproduce the WARP issue below.

On Linux, initialize `external/simpleini` and run:

```sh
bash tests/run_dlssnr_host.sh
```

Checks executed in this task:

- Three portable C++ programs passed: configuration defaults/validation/round-trip with preservation
  of FG/plugins/shortcuts; depth/MV guide regions; and colour-encoding selection.
- The [full Windows MSVC solution build](https://github.com/augustoGrm/OptiScaler/actions/runs/39000775517)
  passed, including the standalone forwarder and package/license output checks.
- On [Windows Server 2022](https://github.com/augustoGrm/OptiScaler/actions/runs/39001105172/job/117304009815),
  all portable and WARP checks passed: production DX12 runtime/submission with a synthetic backend,
  and all feedback reference cases against both published and source-compiled v0.1.31 shaders.
- The same GPU tests fail on `windows-latest` with an access violation inside Windows'
  `d3d10warp.dll`. The upstream feedback test fails at dispatch as well. This runner-specific failure
  remains reproducible via manual workflow dispatch; it is not reported as a passing check and does
  not establish compatibility with that WARP version. Production NVIDIA-driver behavior needs RTX tests.
- Imported source/bytecode provenance matches the exact v0.1.31 tag; the main XML/project import parses.
- Original FG, Reflex, NVAPI, DXGI, Streamline and ASI files are unchanged from the base commit.
- NVIDIA NR evaluation and game/RTX checks have not run. The synthetic backend checks the host's
  composition and lifetime handling, not model loading or compatibility with a particular GPU.

The upstream fork's unrelated `nr_depth_finder_core_smoke` failure (comparison of logs containing
variable lock-wait timings) is not imported into this branch and is not reported as a passing test.

Before merging, require the Windows build/tests to pass, then test on RTX hardware:

1. Compare disabled NR against `master` in DX12 with DLSS/FSR/XeSS, RCAS and output scaling on/off.
2. Enable NR with one pass; verify model initialization, visible output, toggle off/on, resolution
   changes, DRS, camera cuts, SDR/HDR and a missing/incompatible model.
3. Verify two/three passes, work scale and tuning changes; inspect logs/device removal and VRAM use.
4. Exercise the existing FG panel and each available replacement (Nukem/Arturs/FFX/Combo), Reflex
   replacement and `dlssg_sm86.asi`. Check timing/HUD interaction rather than assuming code isolation
   proves visual compatibility. No FG option should become external or disappear.
5. Check normal DX11/Vulkan behavior with NR disabled and confirm bridge paths do not acquire NR work.

## Keeping master synchronized

Work on `feature/modular-dlssnr`; do not replace `master` with janblade's tree. Fetch and merge the
desired updates from your base `master` into the feature branch in the usual way, preserving the
six small extension sites above. Never resolve a conflict by replacing a core file with its
janblade counterpart. Recheck the output-resource and state-restoration contract if upstream changes
`IFeature_Dx12::Evaluate`; update `Integration.cpp` when the feature/NGX metadata interface changes.
Build membership stays in `Module.props`, settings in `SettingsIni.h`, and UI in `Menu.cpp`.

For a later NR version, fetch the exact release tag separately. Compare just the NR shader/protocol
and tests against `UPSTREAM.json`, identify new dependencies, and port only the required changes.
Update the manifest and rerun portable, Windows/WARP and RTX checks. Native-input, FG experiments,
new API support or caching features each need a separately reviewed adapter, not a fork-wide merge.

To remove this integration, remove the six extension sites/build entries, the module/shader files and its
test workflow. Model/ABI/queue-wrapper compatibility remain the principal runtime risks; the small
core diff reduces merge conflicts but cannot eliminate future interface changes.
