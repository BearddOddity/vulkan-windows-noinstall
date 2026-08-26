<#
    Builds llama.cpp against this SDK's Vulkan backend.

    Upstream ships a Windows Vulkan release, so unlike the ROCm repository's
    equivalent this is not the only way to get a working binary. It is here for
    a different reason: the point of the branch is to compare the same commit
    of llama.cpp against the same model on the same card through two APIs, and
    a release binary built by somebody else with an SDK of their choosing is
    not the same experiment.

    The Vulkan backend compiles every one of its shaders at build time with the
    SDK's glslc - several thousand of them - so most of the wall clock here is
    shader compilation rather than C++.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Source,

    [string] $Build = "",
    [string] $Sdk   = "",
    [int]    $Jobs  = 12
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath (Join-Path $Source "CMakeLists.txt"))) {
    throw "no CMakeLists.txt in $Source - is that the llama.cpp checkout?"
}
if (-not $Build) { $Build = Join-Path $Source "build\vulkan" }

# Inherited wholesale from the ROCm repository's build script, where it was
# learned the hard way: MAX_PATH kills this with "C1083: Cannot open compiler
# generated file: ''", which names neither the path nor its length.
if ($Build.Length -gt 100) {
    throw "build path is $($Build.Length) characters; keep it under about 100 or MAX_PATH will kill the compile with a message that names nothing"
}
foreach ($p in @($Source, $Build)) {
    if ($p -match '\s') { throw "path contains a space, which produces a syntax error inside a generated CMake file: $p" }
}

. (Join-Path $PSScriptRoot "..\..\scripts\vkenv.ps1") -Sdk $Sdk
$Sdk = $env:VULKAN_SDK

function Find-Tool($name) {
    $found = Get-Command $name -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    $vs = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Recurse -Filter $name -ErrorAction SilentlyContinue |
          Select-Object -First 1
    if ($vs) { return $vs.FullName }
    throw "$name not found"
}

$cmake = Find-Tool "cmake.exe"
$ninja = Find-Tool "ninja.exe"

# Forward slashes throughout: a Windows path in a quoted CMake string turns
# \P and \W into invalid escapes.
$fwd = { param($p) $p -replace "\\", "/" }

Write-Host ""
Write-Host "=== configure ==="

# CMake finds the SDK through the VULKAN_SDK environment variable that
# vkenv.ps1 has just set; there is no -DVulkan_ROOT to pass. GGML_VULKAN=ON is
# the whole of the backend selection.
#
# LLAMA_BUILD_SERVER is deliberately NOT turned off, though nothing here uses
# the server. With -DLLAMA_BUILD_SERVER=OFF the build gets to 643 of 649
# targets and then fails linking the unified `llama.exe`, which still asks for
# llama-server-impl.lib:
#
#   LINK : fatal error LNK1181: cannot open input file 'llama-server-impl.lib'
#
# Every tool this branch needs has already been built by then, so the failure
# looks like a broken toolchain and is an upstream option that does not
# compose.
#
# It is passed as ON rather than simply omitted, because dropping a -D does not
# clear it: CMake keeps the cached OFF from the previous configure and the
# build fails again in exactly the same place, on a command line that no longer
# mentions the option. Same trap the ROCm repository's build script documents
# for its target list.
& $cmake -S (& $fwd $Source) -B (& $fwd $Build) -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_MAKE_PROGRAM="$(& $fwd $ninja)" `
    -DGGML_VULKAN=ON `
    -DLLAMA_CURL=OFF `
    -DLLAMA_BUILD_TESTS=OFF `
    -DLLAMA_BUILD_EXAMPLES=OFF `
    -DLLAMA_BUILD_TOOLS=ON `
    -DLLAMA_BUILD_SERVER=ON
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Host ""
Write-Host "=== build ==="
& $cmake --build (& $fwd $Build) --config Release --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "build failed" }

$bench = Join-Path $Build "bin\llama-bench.exe"
$cli   = Join-Path $Build "bin\llama-cli.exe"
foreach ($p in @($bench, $cli)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "build reported success and did not produce $p" }
}

Write-Host ""
Write-Host "built:"
Write-Host "  $bench"
Write-Host "  $cli"
Write-Host ""
Write-Host "Next: projects\llama.cpp\bench.ps1 -Bin `"$(Join-Path $Build 'bin')`" -Model <a .gguf>"
