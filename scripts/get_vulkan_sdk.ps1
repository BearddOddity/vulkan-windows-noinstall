<#
    Installs LunarG's Vulkan SDK from the downloaded installer without a GUI
    and without elevation.

    The installer is a Qt Installer Framework package, which means it has a
    real command line and does not need the wizard. Left to the wizard it
    writes to C:\VulkanSDK\<version> and offers optional components one at a
    time; driven from here the component list is written down instead of
    clicked, which is the only form of it that survives a reinstall.

    What this does NOT install is the loader. `vulkan-1.dll` in System32 comes
    from the driver, or from LunarG's separate VulkanRT installer, and that one
    does want elevation. The SDK is headers, libraries, layers and tools; the
    thing that finds a driver at runtime is already on the machine or it is not.
#>
param(
    # LunarG does not offer a stable direct link, so the installer is
    # downloaded by hand from https://vulkan.lunarg.com/sdk/home and named
    # here. Guessing a URL would age worse than asking for the file.
    [Parameter(Mandatory = $true)] [string] $Installer,

    # LunarG's own layout: a version directory under a root. Kept because
    # every tool, template and CMake config in the tree expects to find its
    # siblings at exactly this depth.
    [string] $Dest = "C:\VulkanSDK\1.4.357.0",

    # Extra packages beyond the core. The names are not guessable and are not
    # in any documentation this project found - they came out of
    # `installer.exe search`, which prints the real list and is worth running
    # again when the version moves. As of 1.4.357.0:
    #
    #   com.lunarg.vulkan.debug   shader toolchain debug symbols, 64-bit
    #   com.lunarg.vulkan.sdl2    SDL libraries and headers
    #   com.lunarg.vulkan.glm     GLM headers
    #   com.lunarg.vulkan.vma     Vulkan Memory Allocator header
    #   com.lunarg.vulkan.volk    volk header, source and library
    #   com.lunarg.vulkan.arm64   ARM64 binaries for cross compiling
    #
    # The default is empty on purpose. Everything the verify and smoke test
    # scripts need is in the core, and an optional package that is installed
    # but never used is indistinguishable from one that is missing.
    [string[]] $Components = @()
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Installer)) {
    throw "installer not found: $Installer"
}
if ($Dest -match '\s') {
    # The SDK's own CMake configs and the shader toolchain both take paths
    # from VULKAN_SDK unquoted in places. A space there fails somewhere else,
    # later, with a message that names neither.
    throw "destination must not contain a space: $Dest"
}
if (Test-Path -LiteralPath $Dest) {
    # Refusing beats merging into a half-populated tree; the caller can delete,
    # or run $Dest\maintenancetool.exe to uninstall properly.
    throw "destination already exists, delete it or run its maintenancetool.exe first: $Dest"
}

# com.lunarg.vulkan is the root package; core is marked "Always Installed" and
# comes with it. Naming both is what the wizard records in components.xml, so
# name both here and the two installs are comparable.
$packages = @("com.lunarg.vulkan", "com.lunarg.vulkan.core") + $Components

$arguments = @(
    "--root", $Dest,
    "--accept-licenses",
    "--accept-messages",
    "--default-answer",
    "--confirm-command",
    "install"
) + $packages

Write-Host "installing to $Dest"
Write-Host "packages: $($packages -join ', ')"

& $Installer @arguments
if ($LASTEXITCODE -ne 0) { throw "installer exited with code $LASTEXITCODE" }

# The installer reports success by exiting zero, which it also does when it
# has been asked for a package that does not exist and has silently skipped
# it. Check for the thing the rest of this repository actually needs.
foreach ($required in @("Bin\glslc.exe", "Include\vulkan\vulkan.h", "Lib\vulkan-1.lib")) {
    $p = Join-Path $Dest $required
    if (-not (Test-Path -LiteralPath $p)) {
        throw "installer reported success and did not produce $required"
    }
}

Write-Host ""
Write-Host "installed. Next:"
Write-Host "  scripts\verify.ps1 -Sdk `"$Dest`""
