$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
& py -3 -B (Join-Path $root 'build.py') engine
if ($LASTEXITCODE -ne 0) { throw 'Contest solver build failed' }
