param([string]$BuildDirectory = (Join-Path $env:TEMP ('optiscaler-nr-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null
Push-Location $root
try {
    function Invoke-Checked([string]$Program, [string[]]$Arguments) {
        Get-Command $Program -ErrorAction Stop | Out-Null
        $previousErrorAction = $ErrorActionPreference
        try {
            # Native stderr can contain successful compiler/shader warnings. Preserve it as text
            # and use the process exit code to determine failure, not PowerShell error records.
            $ErrorActionPreference = 'Continue'
            & $Program @Arguments 2>&1 | ForEach-Object { $_.ToString() }
            $commandExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousErrorAction
        }
        if ($commandExit -ne 0) { throw "$Program failed ($commandExit)" }
    }
    $common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/IOptiScaler', '/IOptiScaler/include', '/Iexternal/simpleini')
    Invoke-Checked 'python' @('tests/check_dlssnr_provenance.py')
    foreach ($name in @('dlssnr_settings', 'nr_guides_smoke', 'nr_colour_encoding_smoke')) {
        $sources = @("tests/$name.cpp")
        if ($name -eq 'dlssnr_settings') { $sources += 'OptiScaler/dlssnr/Settings.cpp' }
        Invoke-Checked 'cl' ($common + $sources + @("/Fo$BuildDirectory/", "/Fe$BuildDirectory/$name.exe"))
        Invoke-Checked "$BuildDirectory/$name.exe" @()
    }
    Invoke-Checked 'cl' ($common + @('tests/nr_multipass_feedback_smoke.cpp', "/Fo$BuildDirectory/", "/Fe$BuildDirectory/feedback.exe", '/link', 'd3d11.lib', 'd3dcompiler.lib'))
    Invoke-Checked "$BuildDirectory/feedback.exe" @('OptiScaler/shaders/dlssnr/precompile/dlssnr.hlsl')

    # This DLL belongs only in this temporary test directory. It is an identity model, not NVIDIA code.
    Invoke-Checked 'cl' ($common + @('/LD', 'tests/dlssnr_fake_forwarder.cpp', "/Fo$BuildDirectory/", "/Fe$BuildDirectory/nvngx.dll_dlssnr.dll"))
    Invoke-Checked 'cl' ($common + @('tests/dlssnr_runtime_warp.cpp', 'OptiScaler/dlssnr/Runtime.cpp',
        'OptiScaler/dlssnr/Submission.cpp', 'OptiScaler/dlssnr/Settings.cpp', "/Fo$BuildDirectory/", "/Fe$BuildDirectory/runtime.exe",
        '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', '/LIBPATH:OptiScaler/library/detours', 'detours.lib'))
    Invoke-Checked "$BuildDirectory/runtime.exe" @($BuildDirectory)
    Write-Host "PASS: portable tests, HLSL feedback and D3D12 runtime using a synthetic backend. Logs/artifacts: $BuildDirectory"
} finally {
    Pop-Location
}
