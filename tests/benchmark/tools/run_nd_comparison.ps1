param(
  [Parameter(Mandatory = $true)]
  [string]$LocalAddr,

  [string[]]$Scenario,
  [string]$CasesDir,
  [string]$OutputDir = "nd-comparison-run",
  [int]$BasePort = 19000,
  [string]$BinDir = $PSScriptRoot,
  [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Tool([string]$Name) {
  $exe = Join-Path $BinDir "$Name.exe"
  if (Test-Path $exe) { return (Resolve-Path $exe).Path }
  $plain = Join-Path $BinDir $Name
  if (Test-Path $plain) { return (Resolve-Path $plain).Path }
  throw "tool not found: $Name in $BinDir"
}

function Invoke-Logged([string]$Exe, [string[]]$ToolArgs,
                       [string]$StdoutPath, [string]$StderrPath) {
  if ($DryRun) {
    Write-Host "$Exe $($ToolArgs -join ' ')"
    return 0
  }
  & $Exe @ToolArgs > $StdoutPath 2> $StderrPath
  return $LASTEXITCODE
}

$scenarios = @()
if ($Scenario) { $scenarios += $Scenario }
if ($CasesDir) {
  $scenarios += Get-ChildItem -Path $CasesDir -Filter *.json | ForEach-Object {
    $_.FullName
  }
}
if ($scenarios.Count -eq 0) {
  throw "no scenarios provided; use -Scenario or -CasesDir"
}

$asio = Resolve-Tool "asio_perftest"
$nd = Resolve-Tool "nd_perftest"
$compare = Resolve-Tool "rdma_benchmark_compare_results"

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$summary = Join-Path $OutputDir "summary.md"
"# ND comparison`n" | Set-Content -Path $summary -Encoding utf8

$port = $BasePort
foreach ($scenarioPath in $scenarios) {
  $scenarioFull = (Resolve-Path $scenarioPath).Path
  $scenarioJson = Get-Content -Raw -Path $scenarioFull | ConvertFrom-Json
  $name = if ($scenarioJson.name) { [string]$scenarioJson.name } else {
    [IO.Path]::GetFileNameWithoutExtension($scenarioFull)
  }
  $caseDir = Join-Path $OutputDir $name
  New-Item -ItemType Directory -Force -Path $caseDir | Out-Null

  $asioJson = Join-Path $caseDir "rdma_on_asio.json"
  $ndJson = Join-Path $caseDir "native_nd.json"
  $asioOut = Join-Path $caseDir "rdma_on_asio.stdout.log"
  $asioErr = Join-Path $caseDir "rdma_on_asio.stderr.log"
  $ndOut = Join-Path $caseDir "native_nd.stdout.log"
  $ndErr = Join-Path $caseDir "native_nd.stderr.log"

  $common = @(
    "--single-process",
    "--local-addr", $LocalAddr,
    "--port", [string]$port,
    "--scenario", $scenarioFull
  )

  Write-Host "[run_nd_comparison] $name port=$port"
  Invoke-Logged $asio ($common + @("--json-out", $asioJson)) $asioOut $asioErr | Out-Null
  Invoke-Logged $nd ($common + @("--json-out", $ndJson)) $ndOut $ndErr | Out-Null

  if (-not $DryRun) {
    $compareOut = Join-Path $caseDir "compare.md"
    & $compare $asioJson $ndJson | Tee-Object -FilePath $compareOut | Out-Null
    Add-Content -Path $summary -Value "## $name`n"
    Add-Content -Path $summary -Value (Get-Content -Raw -Path $compareOut)
    Add-Content -Path $summary -Value "`n"
  }

  $port += 10
}

Write-Host "[run_nd_comparison] summary: $summary"
