# 7600 XT Vulkan SDK - working notes

Read this before changing anything here. It records why each script does what
it does, which is mostly a list of failures that were silent or misleading.

Sibling to `D:\My apps\ROCm SDK`, same machine, same card, same shape: a
recipe, not a toolchain. The scripts fetch LunarG's SDK from the vendor and
put it somewhere `.gitignore`d. **No vendor binary is ever committed.** What is
version controlled is: which package, which version, which components, which
environment variables, and why.

The target is one card: **RX 7600 XT**, `deviceID 0x7480`, AMD proprietary
driver 26.8.1 (LLPC). Nothing here has been tried on another.

## The order things run in

```powershell
scripts\get_vulkan_sdk.ps1 -Installer <the LunarG SDK installer>
scripts\verify.ps1                        # do not skip this
scripts\smoke_test.ps1
```

`vkenv.ps1` is dot-sourced by the other three and can be dot-sourced by hand to
get a shell with the SDK and MSVC on it.

`main` builds nothing. Anything built on top belongs on a branch, as in the
ROCm repository.

## What this SDK is and is not

The SDK is headers, import libraries, layers and tools. It is **not** the
loader. `vulkan-1.dll` in System32 comes from the graphics driver, or from
LunarG's separate `VulkanRT-...-Installer.exe`, and that one does want
elevation. The SDK installer does not.

Consequence worth knowing before it wastes an afternoon: **`vulkaninfo.exe` on
PATH is the runtime's, in System32, not the SDK's.** The SDK ships its copy as
`vulkaninfoSDK.exe` precisely so the two do not collide. Every script here
calls `vulkaninfoSDK.exe` by full path, because a check that silently asked a
different loader about a different layer set is worse than no check.

## Getting the SDK without the wizard

The installer is a Qt Installer Framework package, so it has a real command
line:

```
installer.exe --root <dir> --accept-licenses --accept-messages --default-answer --confirm-command install <packages>
```

The package names are not guessable and are not in the SDK documentation this
project found. They come out of the installer itself:

```
installer.exe search
```

which prints an `<availablepackages>` XML block. As of **1.4.357.0**:

| package | what |
| --- | --- |
| `com.lunarg.vulkan` | the root |
| `com.lunarg.vulkan.core` | everything below, marked "Always Installed" |
| `com.lunarg.vulkan.debug` | shader toolchain debug symbols, 64-bit |
| `com.lunarg.vulkan.sdl2` | SDL libraries and headers |
| `com.lunarg.vulkan.glm` | GLM headers |
| `com.lunarg.vulkan.vma` | Vulkan Memory Allocator header |
| `com.lunarg.vulkan.volk` | volk header, source and library |
| `com.lunarg.vulkan.arm64` | ARM64 binaries for cross compiling |

Run `search` again when the version moves rather than trusting that table.

`core` is not small: it already carries glslc, glslangValidator, the whole
`spirv-*` set, dxc, slang, gfxreconstruct, vkconfig, vkcube and every LunarG
and Khronos layer. **The default component list in `get_vulkan_sdk.ps1` is
therefore empty.** An optional package that is installed and never used is
indistinguishable from one that is missing.

The installer contacts LunarG's repositories even for the offline package, so
`search` and `install` both want a network.

## The layer path trap

`vkenv.ps1` sets `VK_ADD_LAYER_PATH`, not `VK_LAYER_PATH`. The two look
interchangeable and are not: `VK_LAYER_PATH` **replaces** discovery, so setting
it hides every layer the driver and the overlays registered, and an overlay
problem then cannot be reproduced from that shell. `ADD` prepends and keeps the
rest.

And it sets it **only when the validation layer is not already enumerable**.
The first draft set it unconditionally, which looked harmless and was not: the
installer already registers the SDK's manifests, so pointing a search path at
the same directory made the loader emit eight

```
Removing layer VK_LAYER_... because it is a duplicate of VK_LAYER_...
```

warnings on every single run - all of them naming the same file twice. Eight
lines of noise on top of a program whose whole job is to make the messages
that matter visible. Removing that one assignment removed all eight.

## What is on this machine that is not the SDK

`verify.ps1` ends with a note rather than a check: **ten Vulkan layers are
registered on this machine by things other than the SDK.**

```
GalaxyOverlayVkLayer / _DEBUG / _VERBOSE     GOG Galaxy
VK_LAYER_AMD_switchable_graphics             driver, registered twice
VK_LAYER_EOS_Overlay                         Epic Online Services
VK_LAYER_OBS_HOOK, VK_LAYER_WEOBS_HOOK       OBS, and a second OBS hook under
                                             C:\ProgramData\lovense-obs-studio-hook
VK_LAYER_ROCKSTAR_GAMES_social_club
VK_LAYER_VALVE_steam_fossilize               Steam pipeline cache
VK_LAYER_VALVE_steam_overlay
```

Every one of those is a DLL loaded into a Vulkan process before the driver
answers. Three of them are registered twice over and the loader says so on
every run. Before believing a capture, a hang, or a frame time from this
machine, turn them off:

```powershell
$env:VK_LOADER_LAYERS_DISABLE = "*"
```

That is the Vulkan equivalent of the idle guard in the ROCm repository's
`card_report.ps1`: the instrument has to be quiet before the reading means
anything.

## Choosing the device

This machine enumerates **two** Radeons - the discrete 7600 XT and the
integrated part in the processor. Today the discrete one is index 0, and
nothing guarantees that: "device 0" is the integrated GPU on plenty of
machines. `smoke_test.cpp` therefore takes a **device name substring** and
matches on `deviceName`, and prints the name it actually used. A benchmark
that quietly ran on the integrated GPU would look like a slow card, and would
look like a slow card consistently.

## Why the smoke test is longer than the HIP one

Three things can break and only one of them is the compiler, so the script
keeps them apart:

1. `glslc` turning GLSL into SPIR-V,
2. `spirv-val` agreeing the SPIR-V is well formed - glslc emitting a module
   glslc is happy with is not the same as one a driver will accept,
3. the driver running it.

And the host program switches the **validation layer on and fails the run if it
objects**, exit code 5, separate from "wrong answer", exit code 4. This is the
part that has no HIP equivalent and is the reason the file is 350 lines: a
Vulkan program containing a genuine misuse usually still runs and still prints
the right numbers. The only way that failure becomes visible is to decide, up
front, that a complaint from the layer ends the run. The teardown destroys
every object in order for the same reason - the layer reports the ones that
are not destroyed, so a leak would mean the clean-validation claim was made
about an incomplete run.

The output buffer is filled with `-1.0f` before the dispatch, so "the shader
did nothing" and "the shader wrote zeroes" are different failures.

## Measured so far

Nothing. `main` proves the SDK works; it does not measure the card. There is no
Vulkan equivalent of `card_report.ps1` here yet - bandwidth, the vector
ceilings, subgroup and cooperative-matrix rates against the same processor are
all unwritten. Do not let this file imply otherwise.

What is proven, on 2026-08-25, is in `samples/first-run.txt`: SDK 1.4.357.0,
`VK_HEADER_VERSION 357`, loader 1.4.357.0, MSVC 19.51.36252, 1176 bytes of
SPIR-V, 1024 elements correct on the 7600 XT with the validation layer clean.

## Small things that are deliberate

- **No space in the SDK path**, checked twice. The SDK's own CMake configs take
  paths from `VULKAN_SDK` unquoted in places; a space fails somewhere else,
  later, in a message that names neither.
- **MSVC comes from `vswhere`**, not a pinned toolset. Vulkan needs no
  particular one, and `vswhere.exe` sits at a fixed path under
  `Program Files (x86)\Microsoft Visual Studio\Installer` for every VS since
  2017 including Build Tools. `vcvars64.bat` is run in a child `cmd` and its
  environment copied back, rather than `INCLUDE` and `LIB` being written by
  hand - those lists move between VS versions.
- **`glslc -O`.** An unoptimised module goes through a different path in the
  driver's compiler than anything real will.
- **A 10-second fence timeout** rather than `UINT64_MAX`. A hung dispatch
  should end the test, not the shell it was run from.
- **`get_vulkan_sdk.ps1` re-checks three files after the installer exits.** The
  installer exits zero when it has been handed a package name that does not
  exist and has silently skipped it.
