<#
    Installs LunarG's Vulkan SDK from the downloaded installer without a GUI.

    The installer is a Qt Installer Framework package, which means it has a
    real command line and does not need the wizard. Left to the wizard it
    writes to C:\VulkanSDK\<version> and offers optional components one at a
    time; driven from here the component list is written down instead of
    clicked, which is the only form of it that survives a reinstall.

    Three things about this were learned the hard way and are the reason the
    script is not four lines. Each is commented where it bites.

      1. --accept-messages and --default-answer are mutually exclusive.
      2. Naming packages on the command line ADDS to the default selection
         rather than replacing it, so "install core" installs 4.99 GB.
      3. It cannot be run unelevated. The core component asks for elevated
         rights part way through and Qt cannot obtain them from a command
         line, so the install runs for a minute and then rolls itself back.

    What this does NOT install is the loader. `vulkan-1.dll` in System32 comes
    from the graphics driver, or from LunarG's separate VulkanRT installer.
    The SDK is headers, libraries, layers and tools; the thing that finds a
    driver at runtime is already on the machine or it is not.
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
    [string[]] $Components = @(),

    # Where the installer's own log goes when this script has to relaunch
    # itself elevated. A new elevated process gets a new console that closes
    # when it exits, so without this the output of a failed install is gone.
    [string] $LogPath = "$env:TEMP\vulkansdk-install.log"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Installer)) {
    throw "installer not found: $Installer"
}
$Installer = (Resolve-Path -LiteralPath $Installer).Path

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

    # --default-answer and NOT --accept-messages. Both flags look like things
    # a headless install wants, and this installer refuses the pair:
    #
    #   The following options are mutually exclusive: accept-messages, default-answer
    #
    # It says so on the last line of a screen and a half of --help output,
    # having exited 1 and printed nothing that reads like an error, which is
    # why the first cold install here looked like an installer that had simply
    # failed. --default-answer is the one to keep: it answers each query with
    # the answer the vendor marked as the default, rather than accepting all.
    "--default-answer",

    # Passed, and it does not work here. Naming packages after `install` does
    # not restrict the install to them: LunarG marks glm, sdl2, volk, debug,
    # vma and arm64 as defaults, so `install com.lunarg.vulkan
    # com.lunarg.vulkan.core` selects all eight components and asks for
    # 4.99 GB. --no-default-installations is the documented way to deselect
    # exactly those, and the log of a run with it shows the same eight:
    #
    #   Selected components without dependencies:
    #   com.lunarg.vulkan com.lunarg.vulkan.core com.lunarg.vulkan.glm
    #   com.lunarg.vulkan.sdl2 com.lunarg.vulkan.volk com.lunarg.vulkan.debug
    #   com.lunarg.vulkan.vma com.lunarg.vulkan.arm64
    #
    # It is left on because it costs nothing and may be honoured by a later
    # SDK. What actually trims the tree is the removal pass below, after the
    # install - which is why $Components means anything at all.
    "--no-default-installations",

    "--confirm-command",
    "install"
) + $packages

# The elevation check is here, before anything is written, because the
# alternative is finding out 45 seconds in. What happens without it: the
# installer extracts every archive, reaches "Installing component The Vulkan
# SDK Core", prints
#
#   installationError : Error : Cannot elevate access rights while running
#   from command line. Please restart the application as administrator.
#
# and then rolls back all 26 operations and exits 1. The elevated work is real
# - registering the SDK's layer manifests under HKLM and running the bundled
# VC_redist - so there is no flag that skips it. --auto-answer does not help
# either: the query offers no buttons, so `--auto-answer installationError=Ignore`
# is rejected as an invalid answer and the default NoButton aborts anyway.
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$elevated = (New-Object Security.Principal.WindowsPrincipal $identity).IsInRole(
                [Security.Principal.WindowsBuiltInRole]::Administrator)

Write-Host "installing to $Dest"
Write-Host "packages: $($packages -join ', ')"

if ($elevated) {
    & $Installer @arguments
    $code = $LASTEXITCODE
}
else {
    # One UAC prompt, then the install runs headless in the elevated process.
    #
    # Three details here are each a failed attempt:
    #
    #   A shell is elevated, not the installer, because -Verb and
    #   -RedirectStandardOutput are in different parameter sets. Asking
    #   Start-Process for both is not a runtime error, it is "Parameter set
    #   cannot be resolved" before anything runs. The redirect is worth
    #   keeping: an elevated process gets its own console that closes when it
    #   exits, and on failure the installer rolls back and deletes the
    #   InstallationLog.txt it would otherwise have left in $Dest.
    #
    #   The work goes in a generated script file rather than -Command. A
    #   -Command string handed through -ArgumentList loses its inner quoting
    #   somewhere in the round trip, and the shape of the failure is the
    #   nastiest kind: the child still exits with the code the string ends in,
    #   so it reports success while having silently done nothing.
    #
    #   Windows PowerShell at its fixed System32 path, not $PSHOME and not
    #   whatever is running this. On this machine pwsh is the Store build out
    #   of C:\Program Files\WindowsApps, and an elevated Store-packaged shell
    #   runs, exits with the right code, and lands none of its file writes.
    Write-Host "not elevated - relaunching the installer through UAC; approve the prompt"
    Write-Host "installer output: $LogPath"

    $quoted   = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $runner   = Join-Path $env:TEMP "vulkansdk-install-$PID.ps1"
    $shell    = "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"

    @(
        "& `"$Installer`" $quoted *> `"$LogPath`""
        'exit $LASTEXITCODE'
    ) | Set-Content -LiteralPath $runner -Encoding ascii

    try {
        $process = Start-Process -FilePath $shell `
                                 -ArgumentList @("-NoProfile", "-File", $runner) `
                                 -Verb RunAs -Wait -PassThru
        $code = $process.ExitCode
    }
    finally { Remove-Item -LiteralPath $runner -Force -ErrorAction SilentlyContinue }
}

if ($code -ne 0) {
    $hint = if (Test-Path $LogPath) { " - see $LogPath" } else { "" }
    throw "installer exited with code $code$hint"
}

# Trim back to what was asked for. The install flags cannot restrict the
# component set - see the note on --no-default-installations above - but the
# maintenance tool the install just left behind can, it is fast because it
# only deletes, and unlike the install it needs no elevation. 4.97 GB becomes
# 1.70 GB, which is byte for byte what a wizard install of core alone
# produces.
$optional = @(
    "com.lunarg.vulkan.debug", "com.lunarg.vulkan.sdl2", "com.lunarg.vulkan.glm",
    "com.lunarg.vulkan.vma",   "com.lunarg.vulkan.volk", "com.lunarg.vulkan.arm64"
)
$installed = Select-String -Path (Join-Path $Dest "components.xml") -Pattern "<Name>([^<]+)</Name>" -AllMatches |
             ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value }
$unwanted = @($optional | Where-Object { $_ -notin $Components -and $_ -in $installed })

if ($unwanted) {
    Write-Host ""
    Write-Host "removing components that were installed without being asked for:"
    $unwanted | ForEach-Object { Write-Host "  $_" }
    & (Join-Path $Dest "maintenancetool.exe") rm @unwanted --default-answer --confirm-command | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "maintenancetool rm exited with code $LASTEXITCODE" }
}

# The installer reports success by exiting zero, which it also does when it
# has been asked for a package that does not exist and has silently skipped
# it. Check for the things the rest of this repository actually needs - after
# the removal pass, because that is a second chance to have deleted them.
foreach ($required in @("Bin\glslc.exe", "Bin\vulkaninfoSDK.exe",
                        "Include\vulkan\vulkan.h", "Lib\vulkan-1.lib")) {
    $p = Join-Path $Dest $required
    if (-not (Test-Path -LiteralPath $p)) {
        throw "installer reported success and did not produce $required"
    }
}

$size = [math]::Round((Get-ChildItem -LiteralPath $Dest -Recurse -File -ErrorAction SilentlyContinue |
                       Measure-Object -Property Length -Sum).Sum / 1GB, 2)

Write-Host ""
Write-Host "installed, $size GB. Next:"
Write-Host "  scripts\verify.ps1 -Sdk `"$Dest`""
Write-Host ""
Write-Host "To remove it again, including its registry entries:"
Write-Host "  $Dest\maintenancetool.exe purge --default-answer --confirm-command"
