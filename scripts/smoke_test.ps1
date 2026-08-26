<#
    Compiles a compute shader with this SDK, compiles and links a host program
    against it, runs the shader on the card, and checks the answer.

    Three separate things can be broken and only one of them is the compiler,
    so the script keeps them apart: glslc turning GLSL into SPIR-V, spirv-val
    agreeing that the SPIR-V is well formed, and the driver actually running
    it. A failure names which of the three it was.

    It is deliberately tiny. A failure here is a toolchain or driver failure
    and nothing else.
#>
param(
    [string] $Sdk    = "",
    [string] $Device = "Radeon RX 7600 XT",
    [string] $Work   = "$env:TEMP\vk-smoke"
)

$ErrorActionPreference = "Stop"

. (Join-Path $PSScriptRoot "vkenv.ps1") -Sdk $Sdk
$Sdk = $env:VULKAN_SDK

if (-not (Test-Path $Work)) { New-Item -ItemType Directory $Work | Out-Null }
$spv = Join-Path $Work "smoke_test.spv"
$exe = Join-Path $Work "smoke_test.exe"

Write-Host ""
Write-Host "=== compiling the shader ==="

# -O for the same reason a release build is what gets shipped: an unoptimised
# module exercises a different path through the driver's compiler than
# anything real will.
& "$Sdk\Bin\glslc.exe" -O -fshader-stage=compute `
    (Join-Path $PSScriptRoot "smoke_test.comp") -o $spv
if ($LASTEXITCODE -ne 0) { throw "glslc failed" }

# glslc emits SPIR-V that glslc is happy with, which is not the same as SPIR-V
# the driver will accept. spirv-val is the arbiter, and it is a second's work.
& "$Sdk\Bin\spirv-val.exe" $spv
if ($LASTEXITCODE -ne 0) { throw "glslc produced a module that spirv-val rejects" }

$spvSize = (Get-Item $spv).Length
Write-Host "$spvSize bytes of SPIR-V, valid"

Write-Host ""
Write-Host "=== compiling the host program ==="

# /EHsc because the C++ runtime headers assume it, /MD to match the SDK's own
# import library, and the link is against vulkan-1.lib - the loader, not a
# driver. Which driver answers is decided at run time.
Push-Location $Work
try {
    & cl.exe /nologo /std:c++17 /EHsc /MD /O2 `
        (Join-Path $PSScriptRoot "smoke_test.cpp") `
        /Fe:$exe `
        /link "/LIBPATH:$Sdk\Lib" vulkan-1.lib
    if ($LASTEXITCODE -ne 0) { throw "cl.exe failed" }
}
finally { Pop-Location }

if (-not (Test-Path $exe)) { throw "the compiler reported success and produced no executable" }

Write-Host ""
Write-Host "=== running on $Device ==="

& $exe $spv $Device
$code = $LASTEXITCODE

Remove-Item $Work -Recurse -Force -ErrorAction SilentlyContinue

switch ($code) {
    0 { }
    2 { throw "the program could not get as far as running: see the message above" }
    3 { throw "a Vulkan call failed: see the VkResult above" }
    4 { throw "the shader ran and gave the wrong answer" }
    5 { throw "the answer was right and the validation layer objected - fix that before trusting anything built here" }
    default { throw "the program exited $code" }
}

Write-Host ""
Write-Host "smoke test passed - this SDK compiles a shader, validates it, and runs it on the card"
