#requires -Version 5.1
[CmdletBinding(DefaultParameterSetName = 'Direct')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Direct')]
    [string]$Config,
    [Parameter(Mandatory = $true, ParameterSetName = 'Direct')]
    [string]$ServerExe,
    [ValidateRange(0, 32)]
    [int]$SupplyCount = 1,
    [ValidateRange(1, 65535)]
    [int]$Port = 18080,
    [ValidateRange(1, 3600)]
    [int]$KindDeadlineSeconds = 20,
    [ValidateRange(1, 3600)]
    [int]$MatchStartDelaySeconds = 1,
    [string]$RunDirectory,
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

function Resolve-InputFile([string]$Path, [string]$Label) {
    if ([string]::IsNullOrWhiteSpace($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).ProviderPath
}

function Test-PortInUse([int]$Number) {
    $listeners = [System.Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
    return [bool](@($listeners | Where-Object { $_.Port -eq $Number }).Count)
}

if ($Interactive) {
    $Config = Read-PathInput 'Config JSON path'
    $ServerExe = Read-PathInput 'Official server exe path'
    $answer = Read-Host 'SupplyCount [1]'
    if (-not [string]::IsNullOrWhiteSpace($answer)) {
        $number = 0
        if (-not [int]::TryParse($answer, [ref]$number) -or $number -lt 0 -or $number -gt 32) {
            throw 'SupplyCount must be an integer from 0 to 32.'
        }
        $SupplyCount = $number
    }
}

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).ProviderPath
$serverPath = Resolve-InputFile $ServerExe 'Official server exe'
$configPath = Resolve-InputFile $Config 'Config JSON'
$enginePath = Resolve-InputFile (Join-Path $repoRoot 'competition\solver\build\contest_solver.exe') 'Contest solver (run build.py engine first)'
$clientPath = Resolve-InputFile (Join-Path $repoRoot 'competition\solver\contest\match_client.py') 'Match client'
$python = Get-Command 'py.exe' -CommandType Application -ErrorAction Stop
$null = Get-Command 'Get-NetTCPConnection' -ErrorAction Stop

try {
    $inputConfig = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8 | ConvertFrom-Json
} catch {
    throw "Cannot read Config JSON: $($_.Exception.Message)"
}
$generatedConfig = $false
if ($null -ne $inputConfig.problem -and $null -ne $inputConfig.teams) {
    $serverConfig = $inputConfig
} elseif ($null -ne $inputConfig.map -and $null -ne $inputConfig.players) {
    if ($Interactive -and -not $PrepareReferenceConfig) {
        $answer = Read-Host 'Reference-map JSON: create a separate server config under runs, without changing input or fuel? [y/N]'
        $PrepareReferenceConfig = ($answer -match '^(?i:y|yes)$')
    }
    if (-not $PrepareReferenceConfig) {
        throw 'This is a reference map, not a problem/teams server config. Use a prepared server config, or explicitly pass -PrepareReferenceConfig to create a separate format-only copy under runs.'
    }
    foreach ($field in @('spots', 'agents', 'fuelLimits', 'daySteps', 'daySeconds', 'busyThreshold', 'jammedThreshold')) {
        if ($null -eq $inputConfig.PSObject.Properties[$field]) {
            throw "Reference map is missing: $field"
        }
    }
    foreach ($field in @('width', 'height', 'cells')) {
        if ($null -eq $inputConfig.map.PSObject.Properties[$field]) {
            throw "Reference map is missing: map.$field"
        }
    }
    $players = 0
    if (-not [int]::TryParse([string]$inputConfig.players, [ref]$players) -or $players -lt 1 -or $players -gt 1000) {
        throw 'Reference map players must be an integer from 1 to 1000.'
    }
    $teams = @(for ($i = 0; $i -lt $players; $i++) { [ordered]@{ token = "token-p$i" } })
    # Same field mapping as the supplied server example; no rule values are changed.
    $serverConfig = [ordered]@{
        problem = [ordered]@{
            width = $inputConfig.map.width
            height = $inputConfig.map.height
            cells = $inputConfig.map.cells
            spots = $inputConfig.spots
            agentStarts = $inputConfig.agents
            fuelLimits = $inputConfig.fuelLimits
            daySteps = $inputConfig.daySteps
            daySeconds = $inputConfig.daySeconds
            busyThreshold = $inputConfig.busyThreshold
            jammedThreshold = $inputConfig.jammedThreshold
        }
        teams = $teams
    }
    $generatedConfig = $true
} else {
    throw 'Config must contain problem/teams, or be an explicitly permitted reference map.'
}

$teams = @($serverConfig.teams)
if ($teams.Count -eq 0 -or $teams[0].token -isnot [string] -or [string]::IsNullOrWhiteSpace($teams[0].token)) {
    throw 'Config must contain a nonempty teams[0].token string.'
}
if ($null -eq $serverConfig.problem.agentStarts -or
    $SupplyCount -ge @($serverConfig.problem.agentStarts).Count) {
    throw 'SupplyCount must leave at least one patrol vehicle; check problem.agentStarts.'
}
if (Test-PortInUse $Port) {
    throw "TCP port $Port is already in use. No existing process will be stopped."
}

$runsRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot 'runs'))
if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss_fff'
    $RunDirectory = Join-Path $runsRoot ("local_{0}_{1}" -f $stamp, [guid]::NewGuid().ToString('N').Substring(0, 6))
}
$runPath = [System.IO.Path]::GetFullPath($RunDirectory)
if (-not $runPath.StartsWith($runsRoot + [System.IO.Path]::DirectorySeparatorChar, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'RunDirectory must be a new directory inside this repository runs folder.'
}
if (Test-Path -LiteralPath $runPath) {
    throw "RunDirectory already exists; previous journals will not be reused: $runPath"
}
$null = New-Item -ItemType Directory -Path $runPath
$serverConfigPath = $configPath
if ($generatedConfig) {
    $serverConfigPath = Join-Path $runPath 'server_config.json'
    $json = $serverConfig | ConvertTo-Json -Depth 100
    [System.IO.File]::WriteAllText($serverConfigPath, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host 'Created a separate problem/teams server config. Input JSON and fuelLimits are unchanged.'
}

$serverProcess = $null
$oldToken = [Environment]::GetEnvironmentVariable('PROCON_TOKEN', 'Process')
$oldBytecode = [Environment]::GetEnvironmentVariable('PYTHONDONTWRITEBYTECODE', 'Process')
try {
    # Recheck just before starting; readiness also checks ownership of the listener.
    if (Test-PortInUse $Port) { throw "TCP port $Port became busy. No process will be stopped." }
    $serverArgs = @(
        '-config', ('"{0}"' -f $serverConfigPath),
        '-addr', "127.0.0.1:$Port",
        '-kind-deadline', "${KindDeadlineSeconds}s",
        '-match-start-delay', "${MatchStartDelaySeconds}s"
    )
    Write-Host "Run directory: $runPath"
    $serverProcess = Start-Process -FilePath $serverPath -ArgumentList $serverArgs `
        -WorkingDirectory $runPath -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput (Join-Path $runPath 'server.stdout.log') `
        -RedirectStandardError (Join-Path $runPath 'server.stderr.log')
    $null = $serverProcess.Handle
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $ready = $false
    $startupSeconds = [Math]::Min(15.0, [Math]::Max(0.5, $KindDeadlineSeconds - 1.0))
    while ($watch.Elapsed.TotalSeconds -lt $startupSeconds) {
        $serverProcess.Refresh()
        if ($serverProcess.HasExited) { throw 'Official server exited during startup; inspect server.stderr.log.' }
        $listeners = @(Get-NetTCPConnection -LocalPort $Port -State Listen -ErrorAction SilentlyContinue)
        if (@($listeners | Where-Object { $_.OwningProcess -eq $serverProcess.Id }).Count -gt 0) {
            $ready = $true
            break
        }
        if ($listeners.Count -gt 0) { throw 'Another process acquired the requested port; it will not be stopped.' }
        Start-Sleep -Milliseconds 100
    }
    if (-not $ready) { throw 'Server readiness timed out before type selection; inspect server logs.' }

    [Environment]::SetEnvironmentVariable('PROCON_TOKEN', $teams[0].token, 'Process')
    [Environment]::SetEnvironmentVariable('PYTHONDONTWRITEBYTECODE', '1', 'Process')
    Write-Host "Client: http://127.0.0.1:$Port ; supply=$SupplyCount ; engine=$enginePath"
    # The client owns deadlines, fallback, replay, submissions, and its journal/events logs.
    & $python.Source -3.12 -B $clientPath `
        --base-url "http://127.0.0.1:$Port" `
        --run-dir $runPath --supply-count $SupplyCount --engine $enginePath
    $clientExit = $LASTEXITCODE
    if ($clientExit -ne 0) { throw "match_client.py exited with code $clientExit. See journal.json and events.jsonl." }
    Write-Host 'Local match client completed.'
} finally {
    [Environment]::SetEnvironmentVariable('PROCON_TOKEN', $oldToken, 'Process')
    [Environment]::SetEnvironmentVariable('PYTHONDONTWRITEBYTECODE', $oldBytecode, 'Process')
    if ($null -ne $serverProcess) {
        try {
            $serverProcess.Refresh()
            if (-not $serverProcess.HasExited) {
                $serverProcess.Kill()
                if (-not $serverProcess.WaitForExit(5000)) {
                    Write-Warning 'The server started by this script has not exited yet. Check it manually.'
                }
            }
        } catch {
            Write-Warning "Could not stop this script's server: $($_.Exception.Message)"
        } finally {
            $serverProcess.Dispose()
        }
    }
    Write-Host "Run directory: $runPath"
}
