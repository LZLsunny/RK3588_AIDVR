<#
.SYNOPSIS
  Pull selected files from the remote build server back to the local Windows workspace.

.DESCRIPTION
  The default "push then build" flow overwrites the remote working directory, so any
  files created only on the remote (e.g. bringup evidence) will not appear locally
  unless you pull them back.
#>

[CmdletBinding()]
param(
  [Parameter()] [string] $RemoteHost = "192.168.64.88",
  [Parameter()] [string] $RemoteUser = "root",
  [Parameter()] [string] $RemoteDir = "/root/AI_Media_RK3588",
  [Parameter()] [string] $LocalDir = "",

  # What to pull
  [Parameter()] [string] $RemoteSubPath = "docs/bringup",
  [Parameter()] [string] $LocalSubPath = "docs/bringup",

  [Parameter()] [switch] $UseRsync,
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
Require-Cmd "scp"

if ($LocalDir -eq "") {
  $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
  $LocalDir = (Resolve-Path (Join-Path $scriptDir "..")).Path
}

$remote = "$RemoteUser@$RemoteHost"
$remotePath = "$RemoteDir/$RemoteSubPath"
$localPath = Join-Path $LocalDir $LocalSubPath

New-Item -ItemType Directory -Force -Path $localPath | Out-Null

if ($DryRun) {
  Write-Host "DryRun: would pull from ${remote}:${remotePath}/ to $localPath/"
  if ($UseRsync) {
    Write-Host "Command: rsync -az -e ssh ${remote}:${remotePath}/ $localPath/"
  } else {
    Write-Host "Command: scp -r ${remote}:${remotePath}/. $localPath/"
  }
  exit 0
}

if ($UseRsync) {
  Require-Cmd "rsync"
  & rsync -az -e ssh "${remote}:${remotePath}/" "$localPath/"
  exit $LASTEXITCODE
}

& scp -r "${remote}:${remotePath}/." "$localPath/"
