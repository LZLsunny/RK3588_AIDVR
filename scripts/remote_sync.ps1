<#
.SYNOPSIS
  Sync this repo from Windows to a remote Linux build server over SSH.

.DESCRIPTION
  Designed for a "Windows edits, Linux builds" workflow.
  Supports three modes:
    - rsync (fast incremental, requires rsync on Windows + remote)
    - git   (sync tracked files via git archive; ignores untracked changes)
    - tar   (sync full directory via tar; excludes build artifacts and .git by default)
#>

[CmdletBinding()]
param(
  [Parameter()] [string] $RemoteHost = "192.168.64.88",
  [Parameter()] [string] $RemoteUser = "root",
  [Parameter()] [string] $RemoteDir = "/root/AI_Media_RK3588",
  [Parameter()] [string] $LocalDir = "",
  [Parameter()] [ValidateSet("rsync","git","tar")] [string] $Mode = "tar",
  [Parameter()] [switch] $IncludeGit,
  [Parameter()] [string] $KnownHostsFile = "",
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

if (-not (Test-Path -LiteralPath $LocalDir)) {
  throw "LocalDir does not exist: $LocalDir"
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

$remote = "$RemoteUser@$RemoteHost"

if (-not $DryRun) {
  & ssh @sshBaseArgs $remote "mkdir -p '$RemoteDir'"
}

function Warn-GitDirty() {
  Require-Cmd "git"
  if (-not (Test-IsGitRepo)) {
    return
  }
  $status = & git -C $LocalDir status --porcelain
  if ($status -and $Mode -eq "git") {
    Write-Warning "Repo has uncommitted/untracked changes. Mode=git syncs only tracked files from HEAD. Use -Mode tar (or commit) to include working changes."
  }
}

function Test-IsGitRepo() {
  return (Test-Path -LiteralPath (Join-Path $LocalDir ".git\\HEAD")) -and (Test-Path -LiteralPath (Join-Path $LocalDir ".git\\config"))
}

switch ($Mode) {
  "rsync" {
    Require-Cmd "rsync"
    $exclude = @(
      "--exclude=.git/"
      "--exclude=build*/"
      "--exclude=.cache/"
      "--exclude=.vscode/"
      "--exclude=.idea/"
      "--exclude=artifacts/logs/"
    )
    if ($IncludeGit) { $exclude = $exclude | Where-Object { $_ -ne "--exclude=.git/" } }

    $args = @("-az","--delete") + $exclude + @("-e","ssh","$LocalDir/","${remote}:${RemoteDir}/")
    if ($DryRun) { $args = @("--dry-run") + $args }
    & rsync @args
  }

  "git" {
    Require-Cmd "git"
    if (-not (Test-IsGitRepo)) {
      throw "Mode=git requires a real git repo (missing .git/HEAD or .git/config). Use -Mode tar or -Mode rsync instead."
    }
    Warn-GitDirty

    $tmp = Join-Path $env:TEMP ("aimedia_repo_" + [Guid]::NewGuid().ToString("n") + ".tar")
    try {
      if (-not $DryRun) {
        & git -C $LocalDir archive --format=tar -o $tmp HEAD
        Require-Cmd "scp"
        & scp @sshBaseArgs $tmp "${remote}:/tmp/aimedia_repo.tar"
        & ssh @sshBaseArgs $remote "rm -rf '$RemoteDir'/* && tar -xf /tmp/aimedia_repo.tar -C '$RemoteDir' && rm -f /tmp/aimedia_repo.tar"
      } else {
        Write-Host "DryRun: would git-archive HEAD, scp to $remote, and extract into $RemoteDir"
      }
    } finally {
      if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    }
  }

  "tar" {
    Require-Cmd "tar"
    Warn-GitDirty

    $tmp = Join-Path $env:TEMP ("aimedia_repo_" + [Guid]::NewGuid().ToString("n") + ".tar")
    $excludes = @(
      "--exclude=.git"
      "--exclude=build"
      "--exclude=build-host"
      "--exclude=build-rk3588"
      "--exclude=.cache"
      "--exclude=.vscode"
      "--exclude=.idea"
      "--exclude=artifacts\\logs"
      "--exclude=artifacts/logs"
    )
    if ($IncludeGit) { $excludes = $excludes | Where-Object { $_ -ne "--exclude=.git" } }

    try {
      if (-not $DryRun) {
        Push-Location $LocalDir
        try {
          & tar -cf $tmp @excludes .
        } finally {
          Pop-Location
        }
        Require-Cmd "scp"
        & scp @sshBaseArgs $tmp "${remote}:/tmp/aimedia_repo.tar"
        & ssh @sshBaseArgs $remote "rm -rf '$RemoteDir'/* && tar -xf /tmp/aimedia_repo.tar -C '$RemoteDir' && rm -f /tmp/aimedia_repo.tar"
      } else {
        Write-Host "DryRun: would tar $LocalDir, scp to $remote, and extract into $RemoteDir"
      }
    } finally {
      if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force }
    }
  }
}
