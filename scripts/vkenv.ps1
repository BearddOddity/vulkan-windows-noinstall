<#
    Dot-source this to put the SDK and a C++ toolchain on the current shell:

        . scripts\vkenv.ps1

    It is a separate file because verify.ps1 and smoke_test.ps1 both need the
    same environment, and because two of the variables it sets are the ones
    that decide which layers and which vulkaninfo you get - getting either
    wrong produces a working command that is answering about something else.
#>
param(
    # Empty means "discover". A machine can carry several SDK versions side by
    # side under C:\VulkanSDK and the newest is the sensible default, but an
    # explicit -Sdk always wins.
    [string] $Sdk = ""
)

$ErrorActionPreference = "Stop"

if (-not $Sdk) {
    if ($env:VULKAN_SDK -and (Test-Path -LiteralPath $env:VULKAN_SDK)) {
        $Sdk = $env:VULKAN_SDK
    }
    else {
        $found = Get-ChildItem -LiteralPath "C:\VulkanSDK" -Directory -ErrorAction SilentlyContinue |
                 Where-Object { Test-Path (Join-Path $_.FullName "Bin\glslc.exe") } |
                 Sort-Object { [version] $_.Name } | Select-Object -Last 1
        if (-not $found) { throw "no Vulkan SDK found - run get_vulkan_sdk.ps1 first, or pass -Sdk" }
        $Sdk = $found.FullName
    }
}
if (-not (Test-Path (Join-Path $Sdk "Bin\glslc.exe"))) {
    throw "no glslc under $Sdk - that is not an SDK root"
}

# The C++ side is ordinary MSVC; nothing here needs a pinned toolset. vswhere
# ships at a fixed path with every Visual Studio since 2017, including Build
# Tools, so it is the one location that does not have to be guessed.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere not found - install Visual Studio Build Tools" }

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) { throw "no Visual Studio install with the x64 C++ tools" }

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars)) { throw "no vcvars64.bat under $vsPath" }

# vcvars64 is a batch file and cannot alter this process, so it is run in a
# child cmd and the resulting environment is copied back. Parsing `set` output
# rather than setting INCLUDE and LIB by hand: those lists move between VS
# versions and a hand-written one is wrong the first time VS updates.
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
if (-not $env:VCToolsInstallDir) { throw "vcvars64 ran and set nothing - check $vcvars by hand" }

$env:VULKAN_SDK = $Sdk
$env:VK_SDK_PATH = $Sdk
$env:INCLUDE = @("$Sdk\Include", $env:INCLUDE) -join ";"
$env:LIB     = @("$Sdk\Lib",     $env:LIB)     -join ";"
$env:PATH    = @("$Sdk\Bin",     $env:PATH)    -join ";"

$summary = @(& "$Sdk\Bin\vulkaninfoSDK.exe" --summary 2>$null)

# Only if they are not already discoverable. The installer registers the SDK's
# layer manifests, and pointing a search path at that same directory does not
# add them twice - it makes the loader print eight "Removing layer ... because
# it is a duplicate" warnings on every run, which then have to be read past
# every time something real goes wrong.
if (-not ($summary | Select-String "VK_LAYER_KHRONOS_validation")) {
    # VK_ADD_LAYER_PATH, not VK_LAYER_PATH. The two look interchangeable and
    # are not: VK_LAYER_PATH *replaces* discovery, so setting it hides every
    # layer the drivers and the overlays registered, and an overlay problem
    # then cannot be reproduced from this shell. ADD prepends, keeps the rest.
    $env:VK_ADD_LAYER_PATH = "$Sdk\Bin"
    $summary = @(& "$Sdk\Bin\vulkaninfoSDK.exe" --summary 2>$null)
}

$driver = @($summary |
            Select-String "deviceName\s+=\s+(.*)" |
            ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() })

Write-Host "Vulkan SDK  $(Split-Path $Sdk -Leaf)  ($Sdk)"
Write-Host "MSVC        $(Split-Path $env:VCToolsInstallDir.TrimEnd('\') -Leaf)"
Write-Host "devices     $($driver -join ' | ')"
