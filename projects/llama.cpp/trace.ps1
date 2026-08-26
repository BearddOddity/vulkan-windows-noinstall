<#
    Lists which compute pipelines each workload actually used, with the matrix
    path on and off.

    This is the Vulkan answer to a question the ROCm repository answered with
    AMD_LOG_LEVEL=3 and a 5 MB kernel trace. GGML_VK_PIPELINE_STATS makes
    ggml's Vulkan backend print every pipeline it created and used, with the
    register and LDS usage the driver reported for it.

    One important difference, and it is a limitation: this is a list of
    pipelines, not a count of dispatches. It answers "which shaders ran" and
    not "how many times". Nothing in the Windows SDK answers the second - there
    is no Vulkan equivalent of the HIP runtime naming every dispatch.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Bin,
    [Parameter(Mandatory = $true)] [string] $Model,
    [int] $Device = 0
)

$ErrorActionPreference = "Stop"

$bench = Join-Path $Bin "llama-bench.exe"
if (-not (Test-Path -LiteralPath $bench)) { throw "no llama-bench.exe in $Bin - run build.ps1 first" }

$env:GGML_VK_VISIBLE_DEVICES = "$Device"
$env:GGML_VK_PIPELINE_STATS  = "1"

function Get-Pipelines($benchArgs, [bool] $disableCoopmat) {
    # Set or unset, never "0". ggml tests these variables with getenv() and
    # checks only whether they exist, so GGML_VK_DISABLE_COOPMAT=0 disables
    # cooperative matrix exactly as thoroughly as =1 does. An hour was lost to
    # two runs that were meant to differ and did not.
    if ($disableCoopmat) { $env:GGML_VK_DISABLE_COOPMAT = "1" }
    else                 { Remove-Item Env:\GGML_VK_DISABLE_COOPMAT -ErrorAction SilentlyContinue }

    $output = & $bench -m $Model -ngl 99 @benchArgs -r 1 2>&1
    $names = $output |
             Select-String -Pattern "pipeline stats for ([a-z0-9_]+)" |
             ForEach-Object { $_.Matches[0].Groups[1].Value } |
             Sort-Object -Unique
    return $names
}

$cases = @(
    @{ label = "token generation (tg8)";  args = @("-p", "0", "-n", "8") }
    @{ label = "prompt processing (pp512)"; args = @("-p", "512", "-n", "0") }
)

foreach ($case in $cases) {
    Write-Host ""
    Write-Host "=== $($case.label) ==="

    $with    = Get-Pipelines $case.args $false
    $without = Get-Pipelines $case.args $true

    $all = @($with + $without | Sort-Object -Unique)
    Write-Host ""
    Write-Host ("  {0,-42} {1,-10} {2}" -f "pipeline", "coopmat", "no coopmat")
    foreach ($name in $all) {
        Write-Host ("  {0,-42} {1,-10} {2}" -f $name,
                    $(if ($with    -contains $name) { "yes" } else { "-" }),
                    $(if ($without -contains $name) { "yes" } else { "-" }))
    }
}

Remove-Item Env:\GGML_VK_DISABLE_COOPMAT -ErrorAction SilentlyContinue
Remove-Item Env:\GGML_VK_PIPELINE_STATS  -ErrorAction SilentlyContinue
