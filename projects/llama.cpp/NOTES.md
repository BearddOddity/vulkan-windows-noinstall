# llama.cpp — working notes

## What this branch establishes

The same commit of llama.cpp, the same model, the same card, measured through
Vulkan here and through HIP in the ROCm repository. And one experiment that
repository could not run: turning the matrix units off and measuring again.

| test | Vulkan, here | HIP, ROCm repo | Vulkan / HIP |
| --- | ---: | ---: | ---: |
| **tg128** — token generation | **85.35 ± 0.24 t/s** | 74.70 ± 0.57 | **1.14×** |
| **pp512** — prompt processing | 1,609.08 ± 44.11 t/s | 2,030.51 ± 104.89 | **0.79×** |

> **Vulkan generates tokens 14% faster than HIP and processes prompts 21%
> slower.** Neither API wins outright, and which one is right for this card
> depends on whether the workload is answering or reading.

And the experiment:

| run | pp512 | tg128 |
| --- | ---: | ---: |
| card, cooperative matrix | **1,609.08** t/s | 85.35 t/s |
| card, cooperative matrix **off** | 736.26 t/s | **86.23** t/s |
| processor — Ryzen 7 7700X | 825.22 t/s | 14.25 t/s |

> **Cooperative matrix is worth 2.19× on prompt processing and nothing at all
> on token generation** — 86.23 t/s without it against 85.35 with, which is to
> say the run without the matrix units was marginally *faster*.
>
> That is the ROCm repository's central llama.cpp finding, reproduced in a
> different API by a different method. There it took `AMD_LOG_LEVEL=3`, a 5 MB
> kernel trace and 6,211 dispatch names to establish that token generation
> never reaches a matrix instruction. Here it takes one environment variable
> and a second benchmark run.

A third number falls out of the same table and is worth saying plainly:
**with cooperative matrix disabled, this card processes prompts more slowly
than the processor does** — 736 t/s against 825. The whole of the card's
prompt-processing advantage on this model is the matrix units.

## Which shaders ran

`GGML_VK_PIPELINE_STATS=1` makes ggml print every pipeline it created and
used. `trace.ps1` runs each workload twice and lines the two lists up:

**Token generation**

| pipeline | coopmat | no coopmat |
| --- | --- | --- |
| `flash_attn_f32_f16_aligned` | – | yes |
| `flash_attn_f32_f16_aligned_cm1` | yes | – |
| `mul_mat_vec_q4_k_q8_1_f32` | yes | yes |
| `quantize_q8_1_x4` | yes | yes |
| `rms_norm_mul_rope_f32_f16` | yes | yes |
| `set_rows_f32_f16_i64` | yes | yes |

**Prompt processing**

| pipeline | coopmat | no coopmat |
| --- | --- | --- |
| `flash_attn_f32_f16_aligned(_cm1)` | `_cm1` | plain |
| `matmul_q4_k_f32_f16acc_aligned_m` | yes | – |
| `matmul_q4_k_q8_1_m` | – | yes |
| `matmul_q6_k_f32_f16acc_aligned_m` | yes | – |
| `matmul_q6_k_q8_1_m` | – | yes |
| plus the four above | yes | yes |

Read together with the rates, that is the whole story:

- Token generation's work is `mul_mat_vec_q4_k_q8_1_f32` — a matrix times a
  **vector**, where every weight is read once and used once. It is the same
  pipeline either way. The only thing cooperative matrix changes is the flash
  attention variant, and attention is not where generation spends its time, so
  the swap is worth 1%.
- Prompt processing swaps both matmul pipelines for `_f16acc_aligned` ones,
  and that swap is the 2.19×.

**`_cm1` in a pipeline name is not evidence that the matrix units did any
work.** Token generation instantiates a `_cm1` pipeline and gains nothing from
it. The same trap the ROCm repository's `wmma` branch fell into — presence
mistaken for use — is available here in a new costume, and the way past it is
the same: turn it off and measure again.

## What this does not measure

`GGML_VK_PIPELINE_STATS` prints a **list of pipelines, not a count of
dispatches**. It answers "which shaders ran" and not "how many times", so the
ROCm repository's "6,211 dispatches, zero matrix kernels" has no equal here -
only its conclusion, arrived at differently. Nothing in the Windows Vulkan SDK
names each dispatch the way the HIP runtime does; `gfxreconstruct` records API
calls, which is a different question again.

Also not covered: one model, one quantisation, llama-bench's synthetic prompt
and generation lengths, no long context, no batching, no draft model, no
server under load, and no assessment of output quality. These are throughput
numbers.

## The instruments, and what they cost to get right

### The machine has to be idle, and now the script insists

`bench.ps1` reads `\GPU Engine(*)\Utilization Percentage` before it starts and
refuses above 25% unless passed `-Force`. That is inherited, not invented: the
ROCm repository's llama.cpp branch published a prompt-processing figure that
was **2.8× too low** because other GPU work was in flight, and nothing in the
run said so. Every repeat was contended in the same way, so the standard
deviation was small and the number looked trustworthy.

An idle desktop is not 0% - compositing alone reads about 14% here, summed
across engines - which is why the threshold is 25 and why the measured value
is printed whether or not it passes.

### `GGML_VK_DISABLE_COOPMAT=0` disables cooperative matrix

ggml tests these variables with `getenv()` and checks only whether they
**exist**. Setting one to `0` disables the feature exactly as thoroughly as
setting it to `1`.

Two trace runs meant to differ came back byte-identical because of it, and the
identical output looked like evidence that cooperative matrix changed nothing —
which was very nearly published as the finding. `bench.ps1` and `trace.ps1`
both remove the variable rather than setting it to zero.

### Two build options that do not compose

`-DLLAMA_BUILD_SERVER=OFF` gets to 643 of 649 targets and then fails:

```
LINK : fatal error LNK1181: cannot open input file 'llama-server-impl.lib'
```

The unified `llama.exe` links the server implementation whether or not the
server was built. Every tool this branch needs has already been produced by
then, so it reads as a broken toolchain rather than as an option that does not
compose.

And **dropping the `-D` does not undo it.** CMake keeps the cached `OFF` and
the build fails again in the same place, on a command line that no longer
mentions the option. `build.ps1` passes `-DLLAMA_BUILD_SERVER=ON` explicitly.
The ROCm repository's build script documents the same trap for its target list.

### A table parser that matched nothing

`bench.ps1` reads llama-bench's own output table. The first version matched the
plus-minus sign literally; llama-bench writes UTF-8, what arrives has been
through the console code page, and the result was an empty summary under three
runs that had each printed perfectly. The pattern now matches "not a digit"
between the rate and its deviation.

## Running it

```powershell
git checkout main
scripts\get_vulkan_sdk.ps1 -Installer <the LunarG SDK installer>
scripts\verify.ps1

git checkout llama.cpp
projects\llama.cpp\build.ps1 -Source <llama.cpp checkout>
projects\llama.cpp\bench.ps1 -Bin <build>\bin -Model <a .gguf>
projects\llama.cpp\trace.ps1 -Bin <build>\bin -Model <a .gguf>
```

`build.ps1` compiles every shader in the Vulkan backend with the SDK's `glslc`,
which is most of the wall clock. Outputs from this machine are in `samples/`.

| | |
| --- | --- |
| **Card** | RX 7600 XT, AMD proprietary driver 26.8.1 (LLPC) |
| **Model** | Qwen3-4B-Instruct-2507, Q4_K_M, 2.32 GiB |
| **llama.cpp** | `2100e59` |
| **Toolchain** | [`main`](../../tree/main) — Vulkan SDK 1.4.357.0 + MSVC 14.51 |

## Where the 0.79× on prompt processing goes

Answered, in three measurements.

### 1. Three quarters of the time is one op

`GGML_VK_PERF_LOGGER=1` prints every op in the graph with its own timing and,
where it can, its rate. On the warm graph of a pp512 run
(`samples/perf-pp512.txt`):

| op | time | share |
| --- | ---: | ---: |
| `MUL_MAT` (seven shapes) | 261.7 ms | **74%** |
| `FLASH_ATTN_EXT` | 43.7 ms | 12% |
| `RMS_NORM_MUL_ROPE` | 22.7 ms | 6% |
| `GLU` | 8.5 ms | 2% |
| `ADD` | 5.8 ms | 2% |
| everything else | ~13 ms | 4% |

Weighted across those seven shapes, ggml's Vulkan matmul runs at
**13,920 GFLOP/s**. The individual shapes range from 11,102 to 14,684.

So the deficit is in the matmul, not in the surrounding ops, and not in the
instruction: the [`cooperative-matrix`](../../tree/cooperative-matrix) branch
measured `coopMatMulAdd` itself at 46,471 GFLOP/s.

### 2. What the matmul would have to reach

Holding everything else fixed, closing the 1.26× end-to-end gap means doing the
same matmul work in 188 ms instead of 262 — that is **19,375 GFLOP/s**.

The same branch measured a 2×2 register-blocked cooperative-matrix GEMM,
fp16 in and fp32 out, at **19,206 GFLOP/s**.

> HIP's effective matmul rate on this model is, to within 1%, what a
> well-blocked cooperative-matrix GEMM gets on this card. ggml's Vulkan matmul
> is at **72% of that**, while also dequantizing Q4_K on the way in. There is
> no missing hardware in this story — the whole gap is one kernel's efficiency.

### 3. The obvious lever is already pulled, in the other direction

ggml has three matmul tile sizes and picks between them at run time. On this
card it only ever used the medium one, which looked like the answer:

```cpp
case VK_VENDOR_ID_AMD:
    device->mul_mat_l[i] = device->coopmat_support && device->driver_id != vk::DriverId::eAmdProprietary;
```

**The large tile is disabled by driver name**, and this is AMD's proprietary
Windows driver. A second AMD tuning path — the flash-attention occupancy
limiter — is gated on `maxComputeSharedMemorySize == 65536`, and this driver
reports **32768** where RADV reports 65536, so that one is skipped too. Two
AMD-specific tunings, both off, on the driver this repository runs.

So the patch is one word. It was applied, built into a separate tree, and
measured:

| | pp512 | tg128 |
| --- | ---: | ---: |
| stock — medium tile | **1,609.08 t/s** | **85.35 t/s** |
| patched — large tile allowed | 419.30 t/s | 73.80 t/s |

> **Enabling the large tile makes prompt processing 3.8× slower.** The gate is
> not an oversight. It is load-bearing, and the hypothesis this branch started
> with was wrong.

`GGML_VK_PIPELINE_STATS` says why, in the numbers the driver reports for each
compiled pipeline:

| pipeline | VGPRs | LDS used | scratch |
| --- | ---: | ---: | ---: |
| `matmul_q4_k_f32_f16acc_aligned_m` | 113 | 11,264 B | **0** |
| `matmul_q4_k_f32_f16acc_aligned_l` | 168 | 21,504 B | **1,056 B** |

168 VGPRs on RDNA3 leaves room for one wave per SIMD where 113 leaves two, and
the large tile **spills 1,056 bytes to scratch** on top of that. Half the
occupancy and a trip to memory in the inner loop.

### What that leaves

The gap is ggml's Vulkan matmul being 28% off what this card can do on fetched
fp16 work, and it cannot be closed by making the tile bigger, because the
register file is already the binding constraint at 113 VGPRs. Closing it means
a kernel that gets more arithmetic per register — which is what the
cooperative-matrix branch's blocked GEMM does, and it is not quantized.

Two smaller things were noticed and not chased:

- `FLASH_ATTN_EXT` took **432 µs per call in one run and 1,214 µs in another**,
  both warm, otherwise identical. Its share of the graph is somewhere between
  5% and 12% and this branch does not know why it moves.
- The first graph of every run is about 10% slower than the last, with flash
  attention worst affected. The clock ramp, again, and the reason the numbers
  quoted here come from the last graph.

## Open

- **The 0.79× on prompt processing is explained above** — what remains is
  whether ggml's Vulkan matmul can be made to reach 19 TFLOP/s at 113 VGPRs.
- **Token generation is memory-bound and neither API is close to the bus.**
  Nothing here measures bandwidth, so how much of the 14% Vulkan lead is the
  memory path rather than the kernels is not known.
- Larger models, longer contexts, and `-fa` variants are all untouched.
