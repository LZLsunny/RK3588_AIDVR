<#
.SYNOPSIS
  Sync from Windows to Ubuntu build server, then build via SSH.
#>

[CmdletBinding()]
param(
  [Parameter()] [string] $RemoteHost = "192.168.64.88",
  [Parameter()] [string] $RemoteUser = "root",
  [Parameter()] [string] $RemoteDir = "/root/AI_Media_RK3588",
  [Parameter()] [string] $LocalDir = "",

  [Parameter()] [ValidateSet("rsync","git","tar")] [string] $SyncMode = "tar",

  # Remote build inputs
  [Parameter()] [string] $Sysroot = "/opt/rk3588-sysroot",
  [Parameter()] [string] $Toolchain = "cmake/toolchains/rk3588-aarch64-linux-gnu.cmake",
  [Parameter()] [string] $BuildDir = "build/rk3588",
  [Parameter()] [string] $Generator = "",
  [Parameter()] [string] $RemoteToolchainBinDir = "/opt/atk-dlrk3588-toolchain/bin",
  [Parameter()] [string] $Target = "",
  [Parameter()] [string] $KnownHostsFile = "",

  [Parameter()] [switch] $NoSync,
  [Parameter()] [switch] $DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Require-Cmd([string] $Name) {
  if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
    throw "Missing required command '$Name'. Install it and retry."
  }
}

Require-Cmd "ssh"

if ($LocalDir -eq "") {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $LocalDir = (Resolve-Path (Join-Path $scriptDir "..")).Path
}

$knownHosts = $KnownHostsFile
if ($knownHosts -eq "") {
  $knownHosts = (Join-Path $LocalDir ".ssh_known_hosts")
}

$sshBaseArgs = @(
  "-o", "StrictHostKeyChecking=accept-new"
  "-o", "UserKnownHostsFile=$knownHosts"
  "-o", "ConnectTimeout=10"
)

if (-not $NoSync) {
  & (Join-Path $PSScriptRoot "remote_sync.ps1") `
    -RemoteHost $RemoteHost `
    -RemoteUser $RemoteUser `
    -RemoteDir $RemoteDir `
    -LocalDir $LocalDir `
    -Mode $SyncMode `
    -KnownHostsFile $knownHosts `
    -DryRun:$DryRun
}

$remote = "$RemoteUser@$RemoteHost"

$genArg = ""
if ($Generator -ne "") {
  $genArg = "--generator '$Generator'"
}

$targetArg = ""
if ($Target -ne "") {
  $targetArg = "--target '$Target'"
}

if ($DryRun) {
  Write-Host "DryRun: would ssh $remote and run build in $RemoteDir"
  if ($RemoteToolchainBinDir -ne "") {
    Write-Host "Remote PATH prepend: $RemoteToolchainBinDir"
  }
  if ($Sysroot -eq "/opt/rk3588-sysroot") {
    Write-Host "Remote sysroot: (auto-detect on real run; default $Sysroot)"
  } else {
    Write-Host "Remote sysroot: $Sysroot"
  }
  Write-Host "Command: bash ./scripts/build_sdk.sh --toolchain '$Toolchain' --sysroot '$Sysroot' --build-dir '$BuildDir' $genArg $targetArg"
  exit 0
}

$originalSysroot = $Sysroot
if ($Sysroot -eq "/opt/rk3588-sysroot") {
  try {
    $detected = & ssh @sshBaseArgs $remote @"
set -euo pipefail
for d in \
  /opt/atk-dlrk3588-toolchain/aarch64-buildroot-linux-gnu/sysroot \
  /opt/atk-dlrk3588-toolchain/aarch64-linux/sysroot \
  /opt/atk-dlrk3588-toolchain/sysroot \
  /opt/rk3588-sysroot \
; do
  if [ -d "`$d" ]; then
    echo "`$d"
    exit 0
  fi
done
exit 1
"@
    $detected = ($detected | Select-Object -First 1).Trim()
    if ($detected -ne "") {
      $Sysroot = $detected
    }
  } catch {
    # Keep default; build will fail with a clearer sysroot error if needed.
  }
}

$pathExport = ""
if ($RemoteToolchainBinDir -ne "") {
  $pathExport = "export PATH='$RemoteToolchainBinDir':`$PATH; "
}

# Use `bash script` so Windows->Linux sync doesn't need executable bits preserved.
& ssh @sshBaseArgs $remote "set -euo pipefail; $pathExport cd '$RemoteDir'; bash ./scripts/build_sdk.sh --toolchain '$Toolchain' --sysroot '$Sysroot' --build-dir '$BuildDir' $genArg $targetArg"
