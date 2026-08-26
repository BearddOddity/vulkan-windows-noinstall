<#
    Proves the SDK before anything is built with it. Every check here
    corresponds to a failure that is silent or misleading when it happens
    later: the wrong vulkaninfo answers about the wrong loader, a layer that
    is present but not discoverable looks like a layer that does nothing, and
    an overlay injected by a game launcher looks like a driver bug.

    This asks whether the right files are in the right places. It proves
    nothing about whether they work together - that is smoke_test.ps1.
#>
param(
    [string] $Sdk = "",
    # The card this repository is about. Substring match against deviceName.
    [string] $Device = "Radeon RX 7600 XT"
)

$ErrorActionPreference = "Continue"
$failed = 0

function Check($name, [scriptblock] $body) {
    try {
        $result = & $body
        Write-Host "ok    $name - $result"
    }
    catch {
        Write-Host "FAIL  $name - $($_.Exception.Message)"
        $script:failed++
    }
}

function Note($name, $text) { Write-Host "note  $name - $text" }

# Dot-sourced rather than assumed: running verify.ps1 in a fresh shell has to
# check the same environment the build scripts will get, not whatever happens
# to be left over in this one.
. (Join-Path $PSScriptRoot "vkenv.ps1") -Sdk $Sdk
$Sdk = $env:VULKAN_SDK
Write-Host ""

Check "no space in the SDK path" {
    if ($Sdk -match '\s') { throw "$Sdk contains a space, which breaks the SDK's own CMake configs" }
    "clean"
}

Check "shader toolchain" {
    $missing = @("glslc.exe", "glslangValidator.exe", "spirv-val.exe", "spirv-dis.exe") |
               Where-Object { -not (Test-Path (Join-Path $Sdk "Bin\$_")) }
    if ($missing) { throw "missing from Bin: $($missing -join ', ')" }
    (& (Join-Path $Sdk "Bin\glslc.exe") --version 2>&1 | Select-Object -First 1)
}

Check "headers and import library" {
    foreach ($f in @("Include\vulkan\vulkan.h", "Include\vulkan\vulkan_core.h", "Lib\vulkan-1.lib")) {
        if (-not (Test-Path (Join-Path $Sdk $f))) { throw "missing: $f" }
    }
    $line = Select-String -Path (Join-Path $Sdk "Include\vulkan\vulkan_core.h") `
                          -Pattern "define VK_HEADER_VERSION\s+(\d+)" | Select-Object -First 1
    "VK_HEADER_VERSION $($line.Matches[0].Groups[1].Value)"
}

Check "loader" {
    # The SDK does not ship the loader; the driver or LunarG's separate
    # runtime installer puts it in System32. Without it every one of these
    # tools starts and reports zero devices, which reads like a dead card.
    $dll = "$env:SystemRoot\System32\vulkan-1.dll"
    if (-not (Test-Path $dll)) { throw "no vulkan-1.dll in System32 - install the Vulkan runtime" }
    "$((Get-Item $dll).VersionInfo.FileVersion)"
}

Check "validation layer discoverable" {
    # Present on disk is not the same as found by the loader. The manifest is
    # what discovery reads, and VK_ADD_LAYER_PATH from vkenv.ps1 is what puts
    # this directory in front of the layers the drivers registered.
    foreach ($f in @("VkLayer_khronos_validation.json", "VkLayer_khronos_validation.dll")) {
        if (-not (Test-Path (Join-Path $Sdk "Bin\$f"))) { throw "missing from Bin: $f" }
    }
    $layers = & (Join-Path $Sdk "Bin\vulkaninfoSDK.exe") --summary 2>$null
    if (-not ($layers | Select-String "VK_LAYER_KHRONOS_validation")) {
        throw "on disk but not enumerated - VK_ADD_LAYER_PATH is $($env:VK_ADD_LAYER_PATH)"
    }
    "enumerated"
}

Check "$Device present" {
    $names = @(& (Join-Path $Sdk "Bin\vulkaninfoSDK.exe") --summary 2>$null |
               Select-String "deviceName\s+=\s+(.*)" |
               ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() })
    if (-not $names) { throw "the loader enumerated no devices at all" }
    # @() around the filter: one match comes back as a bare string, and
    # indexing a string gives a character, not the match.
    $hit = @($names | Where-Object { $_ -like "*$Device*" })
    if (-not $hit) { throw "not found. This machine has: $($names -join ', ')" }
    # Which one it is matters: this machine also carries an integrated Radeon,
    # and "the first device" is the integrated one on plenty of machines.
    "index $([array]::IndexOf($names, $hit[0])) of $($names.Count)"
}

Check "MSVC on the environment" {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) { throw "cl.exe not on PATH after vkenv.ps1" }
    (& $cl.Source 2>&1 | Select-String "Version" | Select-Object -First 1).ToString().Trim()
}

# Not a failure, but the single most likely reason a Vulkan program behaves
# differently on this machine than on a clean one. Every entry here is a DLL
# that gets loaded into the process before the driver does.
$thirdParty = @(& (Join-Path $Sdk "Bin\vulkaninfoSDK.exe") --summary 2>$null |
                Select-String "^(VK_LAYER_|Galaxy)\S*" |
                ForEach-Object { $_.Matches[0].Value }) |
              Where-Object { $_ -notlike "VK_LAYER_KHRONOS_*" -and $_ -notlike "VK_LAYER_LUNARG_*" } |
              Sort-Object -Unique
if ($thirdParty) {
    Write-Host ""
    Note "third-party layers" "$($thirdParty.Count) injected by things other than the SDK:"
    $thirdParty | ForEach-Object { Write-Host "        $_" }
    Write-Host "        These load into every Vulkan process. Disable them before believing a"
    Write-Host "        capture, a hang, or a frame time. VK_LOADER_LAYERS_DISABLE=* turns the lot off."
}

Write-Host ""
if ($failed -eq 0) { Write-Host "all passed" } else { Write-Host "$failed failed"; exit 1 }
