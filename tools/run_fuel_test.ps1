#requires -Version 5.1
[CmdletBinding(DefaultParameterSetName = 'Direct')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Direct')]
    [string]$NormalConfig,
    [Parameter(Mandatory = $true, ParameterSetName = 'Direct')]
    [string]$InfiniteFuelConfig,
    [Parameter(Mandatory = $true, ParameterSetName = 'Direct')]
    [string]$ServerExe,
    [ValidateRange(1, 65535)]
    [int]$Port = 18080,
    [ValidateRange(1, 3600)]
    [int]$KindDeadlineSeconds = 20,
    [ValidateRange(1, 3600)]
    [int]$MatchStartDelaySeconds = 1,
    [switch]$PrepareReferenceConfig,
    [Parameter(Mandatory = $true, ParameterSetName = 'Interactive')]
    [switch]$Interactive
)

$ErrorActionPreference = 'Stop'
function Read-PathInput([string]$Prompt) {
    $value = (Read-Host $Prompt).Trim()
    if ($value.StartsWith('"') -and $value.EndsWith('"')) {
        $value = $value.Substring(1, $value.Length - 2)
    }
    return $value
}
if ($Interactive) {
    $NormalConfig = Read-PathInput 'Normal Config JSON path'
    $InfiniteFuelConfig = Read-PathInput 'Infinite-fuel Config JSON path'
    $ServerExe = Read-PathInput 'Official server exe path'
}
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
$localScript = Join-Path $PSScriptRoot 'run_local_match.ps1'
foreach ($path in @($NormalConfig, $InfiniteFuelConfig, $ServerExe, $localScript,
        (Join-Path $repoRoot 'competition\solver\build\contest_solver.exe'),
        (Join-Path $repoRoot 'competition\solver\contest\match_client.py'))) {
    if ([string]::IsNullOrWhiteSpace($path) -or -not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}
$NormalConfig = (Resolve-Path -LiteralPath $NormalConfig).ProviderPath
$InfiniteFuelConfig = (Resolve-Path -LiteralPath $InfiniteFuelConfig).ProviderPath
$ServerExe = (Resolve-Path -LiteralPath $ServerExe).ProviderPath
$hasReferenceMap = $false
foreach ($path in @($NormalConfig, $InfiniteFuelConfig)) {
    $config = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -ne $config.problem -and $null -ne $config.teams) {
        if (@($config.teams).Count -eq 0 -or [string]::IsNullOrWhiteSpace($config.teams[0].token)) {
            throw "Config is missing teams[0].token: $path"
        }
    } elseif ($null -ne $config.map -and $null -ne $config.players) {
        $hasReferenceMap = $true
    } else {
        throw "Unrecognized Config JSON format: $path"
    }
}
if ($hasReferenceMap -and -not $PrepareReferenceConfig) {
    if ($Interactive) {
        $answer = Read-Host 'Reference maps need separate problem/teams configs under runs. Preserve all input/fuel values and allow format-only copies? [y/N]'
        $PrepareReferenceConfig = ($answer -match '^(?i:y|yes)$')
    }
    if (-not $PrepareReferenceConfig) {
        throw 'Prepare server configs first, or explicitly pass -PrepareReferenceConfig. No match was started.'
    }
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
$runRoot = Join-Path $repoRoot ("runs\fuel_test_{0}_{1}" -f $stamp, [guid]::NewGuid().ToString('N').Substring(0, 6))
$null = New-Item -ItemType Directory -Path $runRoot
$conditions = @(
    [pscustomobject]@{ Name = 'normal_supply1'; Config = $NormalConfig; Supply = 1 },
    [pscustomobject]@{ Name = 'infinite_supply1'; Config = $InfiniteFuelConfig; Supply = 1 },
    [pscustomobject]@{ Name = 'infinite_supply0'; Config = $InfiniteFuelConfig; Supply = 0 }
)
Write-Warning 'Infinite fuel may be outside official rules. Server/client may reject it; no fuel values will be adjusted.'
Write-Host "Comparison directory: $runRoot"
try {
    foreach ($condition in $conditions) {
        Write-Host "Starting $($condition.Name)"
        $arguments = @{
            Config = $condition.Config
            ServerExe = $ServerExe
            SupplyCount = $condition.Supply
            Port = $Port
            KindDeadlineSeconds = $KindDeadlineSeconds
            MatchStartDelaySeconds = $MatchStartDelaySeconds
            RunDirectory = (Join-Path $runRoot $condition.Name)
            PrepareReferenceConfig = [bool]$PrepareReferenceConfig
        }
        & $localScript @arguments
        # Allow the previous server's listener to disappear. Never stop a port owner.
        Start-Sleep -Milliseconds 300
    }
    Write-Host 'All three local clients completed. Scores must be reviewed in each journal.'
} finally {
    Write-Host "Comparison directory: $runRoot"
}
