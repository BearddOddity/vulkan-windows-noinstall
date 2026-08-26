# cooperative-matrix — working notes

## What this branch establishes

The `main` branch ended by pointing at `VK_KHR_cooperative_matrix` in this
card's extension list and saying it had not been touched. This touches it, and
compares every number with what the ROCm repository measured on the same
silicon through HIP.

| instruction | Vulkan, here | HIP, `wmma` branch | Vulkan / HIP |
| --- | ---: | ---: | ---: |
| fp32 vector FMA | **13,765** GFLOP/s | 9,513 | **1.45×** |
| packed fp16 FMA | **21,404** GFLOP/s | 18,633 | 1.15× |
| 16×16×16 matrix multiply-add | **46,471** GFLOP/s | 39,077 | **1.19×** |

> **The cooperative matrix instruction reaches 46.5 TFLOP/s on this card, 3.4×
> its fp32 vector ceiling — and 19% more than the same units delivered through
> HIP's WMMA intrinsic.**
>
> The fp32 figure is the surprise. 13.8 TFLOP/s is above this card's
> single-issue fp32 peak of about 11.3, so the Vulkan compiler is dual-issuing
> where HIP's was not. Two of the three ceilings in this repository are higher
> through Vulkan than through HIP.

And then the question that follows a ceiling — how much survives contact with
memory:

| C = A·B, 2048³, fp16 in fp32 out | GFLOP/s | of the ceiling |
| --- | ---: | ---: |
| scalar, one invocation per element | 972 | 2% |
| cooperative matrix, one 16×16 tile per subgroup | 5,079 | 11% |
| cooperative matrix, **2×2 tiles per subgroup** | **19,206** | **41%** |

> **Doubling the arithmetic per load nearly quadrupled the rate**, from 11% of
> the ceiling to 41%, with the same instruction, the same shape and no shared
> memory in either. The gap between a matrix GEMM and the matrix ceiling is
> load width, not the instruction.
>
> And 19,206 GFLOP/s is 1.40× the *ceiling* of the fp32 vector units — real
> work, operands fetched from memory, checked against the processor, still
> beating the best the vector path could theoretically do.

Every rate above was produced by a kernel whose answer was checked. Both GEMM
kernels agree with a double-precision reference to **zero relative error** on a
512-element sample, which they should: the operands are small multiples of
0.25, so an fp32 accumulator over 2048 terms is exact.

## What the driver offers

`probe.exe` asks, because none of it can be assumed:

```
VK_KHR_cooperative_matrix  present  (of 220 device extensions)
cooperativeMatrix                   = true
supported stages                    = 0x00000020 (compute)
subgroupSize 64, min 32, max 64
11 supported shapes
```

All eleven are **16×16×16 at subgroup scope**. There is no other shape. The
type combinations are fp16→fp32, fp16→fp16 (with and without saturation), and
the four sign combinations of 8-bit integers into sint32 (each with and without
saturation).

Two things are missing that the hardware has. The ROCm branch measured
`v_wmma_*` in **bf16** and in **iu4** — 4-bit integers, where it found exactly
double the int8 rate. **Neither is reachable through this extension on this
driver.** The instruction exists in the silicon and the Vulkan path does not
expose it.

## Wave32 and wave64 are the same speed

RDNA3 runs either width and `VkPhysicalDeviceSubgroupSizeControlProperties`
says compute can be pinned to either. Both were measured, by creating the same
pipeline twice with `VkPipelineShaderStageRequiredSubgroupSizeCreateInfo`:

```
coopmat 16x16x16, wave32      46,471 GFLOP/s
coopmat 16x16x16, wave64      46,392 GFLOP/s
```

A wave64 subgroup takes half as many subgroups to cover the same threads and
each one takes twice as long, so the rate is identical to within measurement
noise. **The width is free to choose.** Pick it for whatever else the shader
does; the matrix instruction does not care.

## The measurement, and the thing that nearly ruined it

Method copied deliberately from the ROCm `wmma` branch so the numbers can be
divided by one another: operands built once from a runtime value and never
reloaded, several independent accumulators to keep the pipeline fed, each
seeded from a *different* value so the compiler cannot fold them into one, one
float per thread written at the end so nothing is optimised away.

Timing is GPU-side, from a `VK_QUERY_TYPE_TIMESTAMP` query pool with the
dispatch as the only thing between the two writes. `timestampPeriod` on this
card is 10.0 ns, so the raw ticks are not nanoseconds and treating them as such
would have been a silent factor of ten.

**The first version of `ceilings.cpp` reported this:**

```
  fp32 vector FMA                    4096        1.149           7474
                                    16384        2.439          14089
```

Four times the work in twice the time. The kernel does not change with the
iteration count, so one of those two numbers was a lie — and 14,089 GFLOP/s
sits between this card's single-issue and dual-issue fp32 peaks, so it was the
plausible-looking one that was closer to right.

The cause is clocks. A one-millisecond dispatch on an idle card spends most of
itself below boost, and a longer dispatch amortises the ramp. **The fix is the
same idea as the idle guard in the ROCm repository's `card_report.ps1`**: half
a second of back-to-back submissions before any timestamp is read. With it:

```
  fp32 vector FMA                    4096        0.655          13109
                                    16384        2.541          13524
                                    65536       10.246          13414
```

Flat across a 16× range of iteration counts, which is what a ceiling should
look like. **Every number in this branch was taken after that warm-up**, and
none of them would have been worth printing without it.

The same effect, smaller, is why the GEMM runs at 2048³ rather than 1024³: at
1024 the matrix kernels finish in a tenth of a millisecond and consecutive runs
of the same binary reported 8,516 and 9,475 GFLOP/s. At 2048 the spread is
about 1%.

## Present is not executed — how that is checked here

The ROCm branch's first draft claimed WMMA figures on the strength of the
instruction being *present* in a binary, and the correction became the
interesting part. The same trap is available in Vulkan and worse, because a
`coopmat` variable that the driver cannot map to hardware is still legal GLSL.

Two things guard against it, and neither is sufficient alone:

1. **`build.ps1` disassembles every module** it compiles and refuses to
   continue if a shader that says `coopMatMulAdd` produced no
   `OpCooperativeMatrixMulAddKHR`:

   ```
   coopmat.comp      -> 2348 bytes, 4 OpCooperativeMatrixMulAddKHR
   gemm_coopmat.comp -> 2176 bytes, 1 OpCooperativeMatrixMulAddKHR
   gemm_blocked.comp -> 2736 bytes, 4 OpCooperativeMatrixMulAddKHR
   ```

   The counts are the loop bodies, and they are the numbers the flop
   accounting assumes. The first version of that check matched on the file
   *name* and so skipped `gemm_blocked.comp` — the one file where the count
   matters most.

2. **The rate itself.** 46.5 TFLOP/s is about 4× what these vector units can
   do and within a couple of percent of this card's rated fp16 matrix peak. An
   emulated cooperative matrix would land at or below the vector ceiling. The
   arithmetic is the proof that the hardware path was taken; the disassembly
   only proves the compiler tried.

What is *not* available here is the ROCm branch's third instrument. There is no
Vulkan equivalent of `AMD_LOG_LEVEL=3` — nothing in this SDK names the ISA the
driver generated, so "which instruction retired" cannot be answered directly on
Windows. `gfxrecon` records API calls, not machine code.

## What is here

| file | what |
| --- | --- |
| `probe.cpp` | prints the shape list, the feature bits and the subgroup widths |
| `ceilings.cpp` | the three instruction ceilings, no memory traffic in any of them |
| `vec_fp32.comp` | eight independent fp32 FMA chains |
| `vec_fp16.comp` | the same in `f16vec2`, which is what maps to `v_pk_fma_f16` |
| `coopmat.comp` | four independent 16×16×16 accumulators |
| `gemm.cpp` | C = A·B three ways, device-local operands, answers checked |
| `gemm_coopmat.comp` | one 16×16 tile per subgroup |
| `gemm_blocked.comp` | 2×2 tiles per subgroup — half the loads for the same arithmetic |
| `gemm_scalar.comp` | one invocation per element, the floor |
| `build.ps1` | compiles a shader and its host program, and checks the disassembly |

```powershell
projects\cooperative-matrix\build.ps1 -What probe
projects\cooperative-matrix\build.ps1 -What ceilings
projects\cooperative-matrix\build.ps1 -What gemm
```

Each prints the executable's path; run it with the same directory as its first
argument, which is where the `.spv` files were left. Outputs from this machine
are in `samples/`.

## What the shaders need that a plain compute shader does not

Four extensions in the GLSL, and four features on the device, and a missing one
shows up as a pipeline that fails to create complaining about a *capability*
rather than about a feature:

| GLSL | Vulkan feature |
| --- | --- |
| `GL_KHR_cooperative_matrix` | `VkPhysicalDeviceCooperativeMatrixFeaturesKHR::cooperativeMatrix`, plus the device extension |
| `GL_KHR_memory_scope_semantics` | `VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModel` |
| `GL_EXT_shader_explicit_arithmetic_types_float16` | `shaderFloat16` |
| `GL_EXT_shader_16bit_storage` | `storageBuffer16BitAccess` — needed by the GEMM shaders, which read fp16 out of a buffer, and *not* implied by `shaderFloat16` |

And the module must be SPIR-V 1.6, which means compiling with
`--target-env=vulkan1.3`. Against the default target the `coopmat` type does
not exist and glslc's error names the type rather than the target environment,
which is a slow five minutes.

The ceiling shaders dodge the 16-bit storage requirement by reading an fp32
seed and converting in the shader. That is not a trick to be proud of, but it
keeps the ceiling measurement independent of one more device feature.

## Open, and worth the next session

- **41% is not the end.** The blocked kernel still reads every operand from
  device memory. Staging tiles through shared memory is the standard next
  step, and the ROCm branch's evidence — hipBLASLt at 80% of the ceiling where
  a hand-written tiling got 20% — says there is roughly another factor of two
  in it.
- **Why is Vulkan's fp32 vector ceiling 45% above HIP's?** Dual issue is the
  obvious answer and it is not proven here. It would be, on Linux, with
  `RADV_DEBUG=asm`.
- **bf16 and iu4 are missing from the shape list.** The hardware has both.
  Whether that is the extension, the driver, or a deliberate omission is not
  answered here.
