<#
    Runs llama-bench three ways on one model and prints them together:
    the card with cooperative matrix, the card without it, and the processor.

    The middle one is the point. ggml's Vulkan backend reports
    "matrix cores: KHR_coopmat" on this card, which is the same extension the
    cooperative-matrix branch measured at 46.5 TFLOP/s, and
    GGML_VK_DISABLE_COOPMAT turns it off without changing anything else. The
    difference between those two runs is what the matrix units are worth to a
    language model - a question the ROCm repository could only answer by
    counting kernel dispatches.

    The idle check is not decoration. The ROCm repository's llama.cpp branch
    published a prompt-processing figure that was 2.8x too low because other
    GPU work was in flight, and nothing about the run said so: every repeat was
    contended the same way, so the standard deviation was small and the number
    looked trustworthy.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Bin,
    [Parameter(Mandatory = $true)] [string] $Model,

    # Vulkan device ordinal, as ggml prints it at startup. 0 is the discrete
    # card on this machine and 1 is the integrated one, and the wrong choice
    # produces numbers that look like a broken card rather than like the wrong
    # device.
    [int]    $Device      = 0,
    [int]    $Repetitions = 5,
    [int]    $Prompt      = 512,
    [int]    $Generate    = 128,
    [switch] $Force
)

$ErrorActionPreference = "Stop"

$bench = Join-Path $Bin "llama-bench.exe"
if (-not (Test-Path -LiteralPath $bench)) { throw "no llama-bench.exe in $Bin - run build.ps1 first" }
if (-not (Test-Path -LiteralPath $Model)) { throw "model not found: $Model" }

# Summed across every engine of every GPU, so an idle desktop is not zero -
# compositing alone reads about 14% here. The threshold is set above that and
# below anything that would actually contend.
$busy = 0.0
try {
    $samples = (Get-Counter '\GPU Engine(*)\Utilization Percentage' -ErrorAction Stop).CounterSamples
    $busy = ($samples | Measure-Object -Property CookedValue -Sum).Sum
}
catch { Write-Host "note: GPU utilisation counter unavailable, skipping the idle check" }

Write-Host ("GPU engines at {0:N1}% before starting" -f $busy)
if ($busy -gt 25.0 -and -not $Force) {
    throw ("the card is at {0:N1}%, which is not idle. Close whatever is using it, or pass -Force and do not trust the numbers." -f $busy)
}

$env:GGML_VK_VISIBLE_DEVICES = "$Device"

$runs = @(
    @{ name = "card, coopmat";     ngl = 99; disable = $false }
    @{ name = "card, no coopmat";  ngl = 99; disable = $true  }
    @{ name = "processor";         ngl = 0;  disable = $false }
)

$results = @()

foreach ($run in $runs) {
    Write-Host ""
    Write-Host "=== $($run.name) ==="

    if ($run.disable) { $env:GGML_VK_DISABLE_COOPMAT = "1" }
    else              { Remove-Item Env:\GGML_VK_DISABLE_COOPMAT -ErrorAction SilentlyContinue }

    $output = & $bench -m $Model -ngl $run.ngl -p $Prompt -n $Generate -r $Repetitions 2>&1
    $output | Write-Host

    foreach ($line in $output) {
        # llama-bench's own table: | model | size | params | backend | ngl | test | t/s |
        #
        # The separator between the rate and its deviation is matched as
        # "not a digit" rather than as the plus-minus sign it actually is.
        # llama-bench writes UTF-8; what arrives here has been through the
        # console code page, and a literal U+00B1 in the pattern matches
        # nothing - which shows up as an empty summary table under a set of
        # runs that all printed correctly.
        if ($line -match '\|\s*(pp\d+|tg\d+)\s*\|\s*([\d.]+)\s*[^\d|]+\s*([\d.]+)\s*\|') {
            $results += [pscustomobject]@{
                Run  = $run.name
                Test = $matches[1]
                Rate = [double] $matches[2]
                Dev  = [double] $matches[3]
            }
        }
    }
}

Remove-Item Env:\GGML_VK_DISABLE_COOPMAT -ErrorAction SilentlyContinue

Write-Host ""
Write-Host "=== together ==="
Write-Host ""
Write-Host ("  {0,-18} {1,-8} {2,14} {3,12}" -f "run", "test", "t/s", "vs CPU")

$cpu = @{}
foreach ($r in $results | Where-Object { $_.Run -eq "processor" }) { $cpu[$r.Test] = $r.Rate }

foreach ($r in $results) {
    $ratio = if ($cpu.ContainsKey($r.Test) -and $cpu[$r.Test] -gt 0) { "{0:N2}x" -f ($r.Rate / $cpu[$r.Test]) } else { "-" }
    Write-Host ("  {0,-18} {1,-8} {2,8:N2} +/- {3,-6:N2} {4,10}" -f $r.Run, $r.Test, $r.Rate, $r.Dev, $ratio)
}

# The comparison this branch exists for. Printed rather than left to the
# reader, because the interesting case is when it is close to 1.
Write-Host ""
foreach ($test in @("pp$Prompt", "tg$Generate")) {
    $with    = ($results | Where-Object { $_.Run -eq "card, coopmat"    -and $_.Test -eq $test }).Rate
    $without = ($results | Where-Object { $_.Run -eq "card, no coopmat" -and $_.Test -eq $test }).Rate
    if ($with -and $without) {
        Write-Host ("  {0}: cooperative matrix is worth {1:N2}x  ({2:N2} -> {3:N2} t/s)" -f $test, ($with / $without), $without, $with)
    }
}
