# Vulkan SDK on an RX 7600 XT

A recipe for installing LunarG's Vulkan SDK on Windows from nothing, without
the wizard, proving it works, and running a real compute shader on the card
with the validation layer switched on and fatal.

No vendor binary is committed here. The scripts fetch the SDK from LunarG and
unpack it into an ignored directory; what is version controlled is which
package, which version, which components, which environment variables, and why.

Sibling to the ROCm SDK repository on the same machine and the same card. That
one gets HIP working; this one gets Vulkan working. They share no files.

**Verified cold on 2026-08-25**: installed to a second, empty location on a
machine that already had an SDK, trimmed, proved, and removed again. The whole
transcript, failures included, is in
[`samples/cold-install.txt`](samples/cold-install.txt).

## Contents

- [What you need](#what-you-need)
- [Running it](#running-it)
- [The four scripts](#the-four-scripts)
- [Installing without the wizard](#installing-without-the-wizard)
- [Three things this repository exists to stop you doing](#three-things-this-repository-exists-to-stop-you-doing)
- [What is in the SDK](#what-is-in-the-sdk)
- [Layers](#layers)
- [What verify.ps1 checks, and what a failure means](#what-verifyps1-checks-and-what-a-failure-means)
- [What the smoke test proves](#what-the-smoke-test-proves)
- [The card, as the SDK reports it](#the-card-as-the-sdk-reports-it)
- [Branches](#branches)
- [What has been measured](#what-has-been-measured)
- [Error messages, and what they actually mean](#error-messages-and-what-they-actually-mean)

## What you need

- Windows x64
- The RX 7600 XT, or any Vulkan 1.1 device if you pass `-Device`
- A graphics driver, or LunarG's `VulkanRT` installer, for `vulkan-1.dll`
- Visual Studio or Build Tools with the x64 C++ workload
- **Administrator rights, once.** The SDK installer cannot complete without
  them; see below
- The SDK installer from <https://vulkan.lunarg.com/sdk/home>, downloaded by
  hand - LunarG offers no stable direct link worth hard-coding
- A network connection at install time. Even the offline installer contacts
  LunarG's repositories

## Running it

```powershell
scripts\get_vulkan_sdk.ps1 -Installer "C:\path\to\vulkansdk-windows-X64-1.4.357.0.exe"
scripts\verify.ps1
scripts\smoke_test.ps1
```

The first of those raises one UAC prompt and then runs headless. It installs
to `C:\VulkanSDK\1.4.357.0` unless you pass `-Dest`, and refuses rather than
merging into a directory that already exists.

`verify.ps1` and `smoke_test.ps1` find the SDK on their own: `VULKAN_SDK` if
it is set, otherwise the newest version under `C:\VulkanSDK`. Pass `-Sdk` to
override, which is how the cold-install run pointed them at a second copy.

To get a shell with the SDK and MSVC on it and nothing else:

```powershell
. scripts\vkenv.ps1
```

To remove an SDK this installed, including its registry entries:

```powershell
C:\VulkanSDK\1.4.357.0\maintenancetool.exe purge --default-answer --confirm-command
```

## The four scripts

| script | what it does |
| --- | --- |
| `get_vulkan_sdk.ps1` | installs the SDK headlessly through one UAC prompt, then trims it to the components you asked for |
| `vkenv.ps1` | dot-sourced; puts the SDK and MSVC on the current shell |
| `verify.ps1` | seven checks on files, loader, layers and device, plus a note on the layers other software has registered. Answers nothing about whether they work together |
| `smoke_test.ps1` | GLSL to SPIR-V to a running dispatch, answer checked, validation clean or the run fails |

`main` builds nothing else. Anything built on top belongs on its own branch.

## Installing without the wizard

The installer is a Qt Installer Framework package with a real command line, so
the component choice can be written down instead of clicked:

```
installer.exe --root <dir> --accept-licenses --default-answer --confirm-command install com.lunarg.vulkan com.lunarg.vulkan.core
```

The package names come from the installer itself - `installer.exe search`
prints them, and nothing else does. As of 1.4.357.0 the optional ones are
`debug` (shader toolchain symbols), `sdl2`, `glm`, `vma`, `volk` and `arm64`,
each prefixed `com.lunarg.vulkan.`.

Three things about that command line are not obvious and each cost a run:

**`--accept-messages` and `--default-answer` cannot both be passed.** The
installer prints `The following options are mutually exclusive` as the last
line after a screen and a half of `--help`, and exits 1 with nothing before it
that reads like an error.

**It cannot run unelevated.** The core component asks for elevated rights part
way through, Qt cannot obtain them from a command line, and the install
extracts everything, runs for 45 seconds, rolls back all 26 operations and
exits 1:

```
installationError : Error : Cannot elevate access rights while running from
command line. Please restart the application as administrator.
```

The elevated work is real - it registers the layer manifests under HKLM and
runs the bundled VC_redist - so no flag skips it and `--auto-answer` cannot
answer it. `get_vulkan_sdk.ps1` checks for elevation before writing anything
and relaunches itself through UAC.

**Naming packages does not restrict the install to them.** It adds them to
LunarG's default selection, and `--no-default-installations`, which is the
documented flag for deselecting exactly those, does not do it here either. A
command line asking for two components installs eight and 4.99 GB of them.

What works is the maintenance tool the install leaves behind:
`maintenancetool.exe rm <packages>` takes 4.97 GB down to 1.70 GB in under a
second, needs no elevation, and lands on byte-for-byte what the wizard
produces for core alone. `get_vulkan_sdk.ps1` does this automatically for
anything you did not ask for, which is the only reason its `-Components`
parameter means anything.

The SDK does not include the loader. `vulkan-1.dll` in System32 comes from the
driver or from LunarG's separate runtime installer.

## Three things this repository exists to stop you doing

**Asking the wrong `vulkaninfo`.** The one on PATH is the runtime's, in
System32. The SDK ships its own as `vulkaninfoSDK.exe` so the two do not
collide, and every script here calls it by full path.

**Setting `VK_LAYER_PATH`.** It *replaces* layer discovery rather than adding
to it, so it hides every layer the driver and the overlays registered.
`VK_ADD_LAYER_PATH` prepends and keeps the rest - and `vkenv.ps1` sets even
that only when the validation layer is not already enumerable, because
pointing a search path at manifests the installer already registered makes the
loader print eight duplicate-layer warnings on every run.

**Trusting a measurement taken through ten other people's layers.** This
machine has GOG Galaxy, Epic, Steam (twice), Rockstar, OBS (twice) and the AMD
switchable-graphics layer registered, all of which load into every Vulkan
process. `verify.ps1` lists them. Turn them off with
`$env:VK_LOADER_LAYERS_DISABLE = "*"` before believing a capture or a frame
time.

There is a fourth, which this repository can only warn about: **installing a
second SDK repoints every explicit layer entry at the new one.** Before the
cold-install run, `HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers` held nine
manifests under `C:\VulkanSDK`; afterwards it held nine under `D:\vk-cold` and
none under C:. Both trees were intact on disk and only one had working layers.

## What is in the SDK

Core alone is 1.70 GB, and 1.6 GB of that is `Bin` (817 MB, 82 entries) and
`Lib` (782 MB). The rest is headers, the Vulkan XML registry under
`share\vulkan\registry`, a Visual Studio 2022 template, SPIRV-Reflect as
source, and a worked `Config\vk_layer_settings.txt`.

Four shader compilers ship, and they are not interchangeable:

| tool | version here | takes |
| --- | --- | --- |
| `glslc` | shaderc v2026.3 | GLSL, HLSL, gcc-style flags. The default choice |
| `glslangValidator` | glslang 16.4.0 | GLSL, HLSL, the Khronos reference front end |
| `slangc` | slang 2026.13.1 | Slang, a different language targeting SPIR-V |
| `dxc` | 1.9.0.5399 | HLSL, Microsoft's, with the SPIR-V backend |

### Everything else in `Bin`, by what it is for

**Working on a SPIR-V module you already have:**

| tool | for |
| --- | --- |
| `spirv-val` | is this module legal? The arbiter, and the only opinion the driver shares |
| `spirv-dis`, `spirv-as` | disassemble to text and back. The text form is readable, and diffable |
| `spirv-opt` | run optimisation passes by name, or `-O` / `-Os` for the standard sets |
| `spirv-cross` | translate *back* to GLSL, HLSL or MSL. How you find out what a shipped shader does |
| `spirv-reflect`, `spirv-reflect-pp` | dump the descriptor interface without writing a reflection pass |
| `spirv-link` | link several modules into one |
| `spirv-diff` | structural diff of two modules, which a text diff of the disassembly is not |
| `spirv-lint` | complaints that are legal but suspect |
| `spirv-reduce` | shrink a module while it still reproduces a bug. The one you want at 2am |
| `spirv-cfg` | emit the control flow graph as a dot file |
| `spirv-objdump` | inspect a module the way you would an object file |

**Capturing and replaying an application** - `gfxrecon-capture.py` and
`gfxrecon-capture-vulkan.py` start a program with the capture layer attached;
`gfxrecon-replay` plays the file back; `gfxrecon-info` summarises it;
`gfxrecon-extract`, `gfxrecon-optimize`, `gfxrecon-compress` and
`gfxrecon-convert` work on the file itself. This is the closest thing the
Windows SDK has to a profiler, and it is a recorder rather than a profiler.

**Nine layers**, each a `.dll` and a `.json` manifest in `Bin`:

| layer | what it does |
| --- | --- |
| `VK_LAYER_KHRONOS_validation` | the one that matters. Legality, best practices, synchronisation, GPU-assisted checks |
| `VK_LAYER_KHRONOS_synchronization2` | emulates `VK_KHR_synchronization2` where the driver lacks it |
| `VK_LAYER_KHRONOS_shader_object` | emulates `VK_EXT_shader_object` |
| `VK_LAYER_KHRONOS_profiles` | forces the device to *report* a chosen profile, so you can test the limits of hardware you do not own |
| `VK_LAYER_LUNARG_api_dump` | every call, every argument, to a file. Enormous, and definitive |
| `VK_LAYER_LUNARG_crash_diagnostic` | on a device loss, says how far the GPU got |
| `VK_LAYER_LUNARG_gfxreconstruct` | the capture half of gfxrecon |
| `VK_LAYER_LUNARG_monitor` | frame rate in the title bar |
| `VK_LAYER_LUNARG_screenshot` | frame grabs by frame number |

**Sanity checks**: `vkcube` and `vkcubepp` are the spinning cube in C and C++,
and are the fastest way to answer "is anything working at all".
`vulkanCapsViewer` is the GUI vulkaninfo. `vkconfig` and `vkconfig-gui`
configure layers.

`Lib\cmake` is not the Vulkan package - `find_package(Vulkan)` needs only the
`VULKAN_SDK` variable. It holds config packages for glslang, SPIRV-Tools and
spirv_cross, for linking the compiler stack into your own program.

## Layers

Explicit layers - asked for by name - come from
`HKLM\SOFTWARE\Khronos\Vulkan\ExplicitLayers`, the `HKCU` equivalent, and any
directory in `VK_LAYER_PATH` or `VK_ADD_LAYER_PATH`. Implicit layers - which
load into every process without being asked - come from `...\ImplicitLayers`
under both hives. Each registry value name is the full path to a `.json`
manifest.

`VK_LOADER_LAYERS_ENABLE` and `VK_LOADER_LAYERS_DISABLE` take globs and are
the blunt instrument; `Config\vk_layer_settings.txt` and `vkconfig.exe` are
the precise one, and are how you turn on the validation layer's expensive
checks - GPU-assisted validation, best practices, synchronisation - none of
which are on by default.

## What verify.ps1 checks, and what a failure means

Seven checks, each corresponding to a failure that is silent or misleading
when it happens later rather than here.

| check | fails when | which otherwise looks like |
| --- | --- | --- |
| no space in the SDK path | `VULKAN_SDK` contains a space | a CMake error in a generated file that names neither the SDK nor the space |
| shader toolchain | `glslc`, `glslangValidator`, `spirv-val` or `spirv-dis` missing from `Bin` | a build that cannot compile shaders, blamed on the build system |
| headers and import library | `vulkan.h`, `vulkan_core.h` or `vulkan-1.lib` missing | a compile or link failure attributed to the compiler |
| loader | no `vulkan-1.dll` in System32 | every tool starting cleanly and reporting **zero devices**, which reads as a dead card |
| validation layer discoverable | the manifest is on disk but the loader does not enumerate it | a validation layer that appears to be enabled and silently is not |
| the target device present | no `deviceName` matches | nothing, until a benchmark runs on the integrated GPU |
| MSVC on the environment | `cl.exe` not on PATH after `vkenv.ps1` | vswhere found a Visual Studio without the C++ workload |

It then prints, as a note rather than a failure, every layer registered by
software that is not the SDK. That list is not a problem to fix; it is the
thing to remember before trusting a measurement.

The check that is deliberately *not* there is anything about the SDK's own
version matching the driver's. They are independent: this machine runs SDK
1.4.357.0 against a driver reporting `apiVersion 1.4.349`, and that is normal.

## What the smoke test proves

It compiles `scripts/smoke_test.comp` with `glslc -O`, has `spirv-val` confirm
the module, compiles `scripts/smoke_test.cpp` with MSVC against
`vulkan-1.lib`, and dispatches 1024 element-wise adds on the device whose name
matches `-Device`.

The exit codes are distinct on purpose:

| code | meaning |
| --- | --- |
| 2 | could not get as far as running - missing layer, no matching device, bad SPIR-V file |
| 3 | a Vulkan call returned a failure `VkResult` |
| 4 | the shader ran and the answer was wrong |
| 5 | the answer was right and the validation layer objected |

Code 5 is the one worth having. A Vulkan program with a real misuse in it
usually still runs and still prints the right numbers, so the layer's
complaint is made fatal rather than printed. The output buffer starts at
`-1.0f` so that "did nothing" and "wrote zeroes" are separate failures, and
every object is destroyed in order so the layer's silence is about a complete
run.

## The card, as the SDK reports it

Reported capability, not measurement. Nothing here was timed.

Five queue families: graphics+compute (8 queues), compute-only (8), transfer
(1), video encode (1), video decode (1). Three memory heaps: 15.73 GiB device
local, 15.57 GiB host, and a **256 MiB** heap that is device local *and* host
visible - the BAR window, small enough to say resizable BAR is not in play.

`subgroupSize = 64` with `minSubgroupSize 32`, `maxSubgroupSize 64` and
`subgroupSizeControl` - RDNA 3's wave32/wave64 choice, exposed and settable.

220 device extensions, including **`VK_KHR_cooperative_matrix` with
`cooperativeMatrix = true`**, compute stage only. That is the counterpart of
the ROCm repository's WMMA branch, which measured those units at 39.1 TFLOP/s,
4.1x the fp32 vector ceiling. The Vulkan route to the same silicon exists on
this card and has not been touched here.

## Branches

`main` carries the toolchain and nothing else. Everything built on top lives on
a branch, each branched from `main`:

| branch | what it does | found |
| --- | --- | --- |
| `cooperative-matrix` | measures `VK_KHR_cooperative_matrix` against the vector units, and a real GEMM against the instruction's ceiling | the matrix instruction is **46,471 GFLOP/s**, 3.4x this card's fp32 vector ceiling and 19% above what the same units gave through HIP's WMMA intrinsic |
| `llama.cpp` | a language model on the Vulkan backend, against the ROCm repository's HIP numbers for the same model and card | **14% faster generating** than HIP, 21% slower on prompts; cooperative matrix is worth **2.19x** on prompt processing and **nothing** on generation. The 21% is one kernel: 74% of prompt processing is `MUL_MAT` at 13,920 GFLOP/s where 19,375 would match HIP |

Its working notes are `projects/cooperative-matrix/NOTES.md` on that branch.

## What has been measured

On `main`, nothing. It proves the SDK works and does not measure the card.

Two full runs are recorded. [`samples/first-run.txt`](samples/first-run.txt)
is the SDK already on the machine; [`samples/cold-install.txt`](samples/cold-install.txt)
is one installed from nothing, before and after trimming. On 2026-08-25:

```
SDK 1.4.357.0, VK_HEADER_VERSION 357, loader 1.4.357.0
shaderc v2026.3, MSVC 19.51.36252
AMD Radeon RX 7600 XT, driver 26.8.1 (LLPC), index 0 of 2 devices
1176 bytes of SPIR-V, 1024 elements, all correct, validation clean
```

The second device is the integrated Radeon in the processor. It is index 1
here and would be index 0 on plenty of machines, which is why the smoke test
selects by name and prints the name it used.

The obvious next branch is the one the extension list points at: cooperative
matrix in compute, measured against this card's fp32 vector ceiling, and
compared with what the ROCm repository already measured through WMMA. Two
paths to the same silicon, and no reason yet to believe they land in the same
place.

## Error messages, and what they actually mean

Every one of these was hit while building this repository. They are indexed by
the text you would search for.

**`The following options are mutually exclusive: accept-messages, default-answer.`**
Printed last, after the whole of `--help`. Drop `--accept-messages`.

**`Cannot elevate access rights while running from command line.`**
The SDK installer cannot complete unelevated, and rolls back everything it has
already extracted. Run `get_vulkan_sdk.ps1`, which relaunches through UAC.

**`Invalid answer "Ignore" for "installationError".`**
An attempt to get past the previous message with `--auto-answer`. The query
offers no buttons; there is no answer that continues.

**`Parameter set cannot be resolved using the specified named parameters.`**
From `Start-Process` given both `-Verb RunAs` and `-RedirectStandardOutput`.
They are in different parameter sets. Elevate a shell, not the installer.

**An elevated command reports success and did nothing.**
Two causes seen here. A `-Command` string passed through `-ArgumentList` can
lose its inner quoting and still exit with the code the string ends in - use
`-File` and a generated script. And an elevated Store-packaged `pwsh` from
`C:\Program Files\WindowsApps` runs, exits correctly, and lands none of its
file writes - use `System32\WindowsPowerShell\v1.0\powershell.exe`.

**`Removing layer VK_LAYER_X ... because it is a duplicate of VK_LAYER_X`,
naming the same file twice.** A search path in `VK_ADD_LAYER_PATH` pointing at
manifests that are already registered. Harmless, and eight lines of it per
run.

**`Layer name GalaxyOverlayVkLayer does not conform to naming standard
(Policy #LLP_LAYER_3)`.** GOG Galaxy's overlay, not yours. Same for the OBS
and AMD duplicate-layer lines.

**Zero devices enumerated, from every tool at once.** No loader. That is
`vulkan-1.dll` in System32, which comes from the driver or LunarG's separate
runtime installer, not from the SDK.

**`index -1 of 2` for a device that was just found.** A PowerShell one, and
this repository's own bug: `$x = $list | Where-Object {...}` returns a bare
string when exactly one thing matches, and indexing a string gives a
character. Wrap the filter in `@()`.

## Licence

MIT, see [LICENSE](LICENSE). It covers the scripts. It does not cover the
Vulkan SDK, which is LunarG's and is licensed under the terms shown by its own
installer.
