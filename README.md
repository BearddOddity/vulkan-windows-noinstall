# Vulkan SDK on an RX 7600 XT

A recipe for getting LunarG's Vulkan SDK onto a Windows machine without the
wizard, proving it works, and running a real compute shader on the card with
the validation layer switched on.

No vendor binary is committed here. The scripts fetch the SDK from LunarG and
unpack it into an ignored directory; what is version controlled is which
package, which version, which components, which environment variables, and why.

Sibling to the ROCm SDK repository on the same machine and the same card. That
one gets HIP working; this one gets Vulkan working. They share no files.

## What you need

- Windows x64
- The RX 7600 XT, or any Vulkan 1.1 device if you pass `-Device`
- A graphics driver, or LunarG's `VulkanRT` installer, for `vulkan-1.dll`
- Visual Studio or Build Tools with the x64 C++ workload
- The SDK installer from <https://vulkan.lunarg.com/sdk/home>, downloaded by
  hand - LunarG offers no stable direct link worth hard-coding

## Running it

```powershell
scripts\get_vulkan_sdk.ps1 -Installer "C:\path\to\vulkansdk-windows-X64-1.4.357.0.exe"
scripts\verify.ps1
scripts\smoke_test.ps1
```

`verify.ps1` and `smoke_test.ps1` find the SDK on their own: `VULKAN_SDK` if it
is set, otherwise the newest version under `C:\VulkanSDK`. Pass `-Sdk` to
override.

To get a shell with the SDK and MSVC on it and nothing else:

```powershell
. scripts\vkenv.ps1
```

## The four scripts

| script | what it does |
| --- | --- |
| `get_vulkan_sdk.ps1` | installs the SDK headlessly, no elevation, named components |
| `vkenv.ps1` | dot-sourced; puts the SDK and MSVC on the current shell |
| `verify.ps1` | seven checks on files, loader, layers and device. Answers nothing about whether they work together |
| `smoke_test.ps1` | GLSL to SPIR-V to a running dispatch, answer checked, validation clean or the run fails |

`main` builds nothing else. Anything built on top belongs on its own branch.

## Installing without the wizard

The installer is a Qt Installer Framework package with a real command line, so
the component choice can be written down instead of clicked:

```
installer.exe --root <dir> --accept-licenses --accept-messages --default-answer --confirm-command install com.lunarg.vulkan com.lunarg.vulkan.core
```

The package names come from the installer itself - `installer.exe search`
prints them, and nothing else does. As of 1.4.357.0 the optional ones are
`debug` (shader toolchain symbols), `sdl2`, `glm`, `vma`, `volk` and `arm64`,
each prefixed `com.lunarg.vulkan.`. The default here installs none of them:
the core already carries glslc, glslangValidator, the `spirv-*` tools, dxc,
slang, gfxreconstruct, vkconfig, vkcube and every Khronos and LunarG layer.

The SDK does not include the loader. `vulkan-1.dll` in System32 comes from the
driver or from LunarG's separate runtime installer, and that one does need
elevation.

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

## What has been measured

Nothing yet. `main` proves the SDK works and does not measure the card.

The full first run is in [`samples/first-run.txt`](samples/first-run.txt). On
2026-08-25, on this machine:

```
SDK 1.4.357.0, VK_HEADER_VERSION 357, loader 1.4.357.0
shaderc v2026.3, MSVC 19.51.36252
AMD Radeon RX 7600 XT, driver 26.8.1 (LLPC), index 0 of 2 devices
1176 bytes of SPIR-V, 1024 elements, all correct, validation clean
```

The second device is the integrated Radeon in the processor. It is index 1
here and would be index 0 on plenty of machines, which is why the smoke test
selects by name and prints the name it used.

## Licence

MIT, see [LICENSE](LICENSE). It covers the scripts. It does not cover the
Vulkan SDK, which is LunarG's and is licensed under the terms shown by its own
installer.
