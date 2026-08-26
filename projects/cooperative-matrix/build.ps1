<#
    Builds one of this branch's programs and leaves the executable in $Work.

    Shaders are compiled here too, because a cooperative matrix shader needs
    flags that a default glslc invocation does not use: SPIR-V 1.6, and the
    Vulkan 1.3 target environment that goes with it. Compiled against 1.0 the
    extension's types do not exist and the error names the type rather than
    the target.
#>
param(
    [Parameter(Mandatory = $true)] [ValidateSet("probe", "ceilings", "gemm")] [string] $What,
    [string] $Sdk  = "",
    [string] $Work = "$env:TEMP\vk-coopmat"
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "..\..\scripts\vkenv.ps1") -Sdk $Sdk
$Sdk = $env:VULKAN_SDK

if (-not (Test-Path $Work)) { New-Item -ItemType Directory $Work | Out-Null }

$sources = @{
    probe    = @{ cpp = "probe.cpp";    shaders = @() }
    ceilings = @{ cpp = "ceilings.cpp"; shaders = @("vec_fp32.comp", "vec_fp16.comp", "coopmat.comp") }
    gemm     = @{ cpp = "gemm.cpp";     shaders = @("gemm_coopmat.comp", "gemm_blocked.comp", "gemm_scalar.comp") }
}
$job = $sources[$What]

foreach ($shader in $job.shaders) {
    $src = Join-Path $PSScriptRoot $shader
    $spv = Join-Path $Work ($shader -replace '\.comp$', '.spv')

    # --target-env=vulkan1.3 implies SPIR-V 1.6, which is what
    # GL_KHR_cooperative_matrix requires. -O because an unoptimised module goes
    # through a different path in the driver's compiler than anything real.
    & "$Sdk\Bin\glslc.exe" -O --target-env=vulkan1.3 -fshader-stage=compute $src -o $spv
    if ($LASTEXITCODE -ne 0) { throw "glslc failed on $shader" }

    & "$Sdk\Bin\spirv-val.exe" --target-env vulkan1.3 $spv
    if ($LASTEXITCODE -ne 0) { throw "spirv-val rejected $shader" }

    # The whole point of this branch is that the instruction is issued rather
    # than merely available, and the module either contains the op or it does
    # not. Checked here, once, rather than assumed by three later programs.
    # Keyed on what the source contains rather than on its name: the first
    # version matched "*coopmat*" and so silently skipped gemm_blocked.comp,
    # which is the file where the instruction count matters most.
    if (Select-String -Path $src -Pattern "coopMatMulAdd" -Quiet) {
        $ops = (& "$Sdk\Bin\spirv-dis.exe" $spv | Select-String "OpCooperativeMatrixMulAddKHR").Count
        if ($ops -eq 0) { throw "$shader compiled without a single OpCooperativeMatrixMulAddKHR" }
        Write-Host "$shader -> $((Get-Item $spv).Length) bytes, $ops OpCooperativeMatrixMulAddKHR"
    }
    else {
        Write-Host "$shader -> $((Get-Item $spv).Length) bytes"
    }
}

$exe = Join-Path $Work "$What.exe"
Push-Location $Work
try {
    & cl.exe /nologo /std:c++17 /EHsc /MD /O2 (Join-Path $PSScriptRoot $job.cpp) `
        /Fe:$exe /link "/LIBPATH:$Sdk\Lib" vulkan-1.lib
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed" }
}
finally { Pop-Location }

Write-Host ""
Write-Host $exe
