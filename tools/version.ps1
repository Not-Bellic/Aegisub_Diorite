#!/usr/bin/env powershell

param (
  [Parameter(Position = 0, Mandatory = $false)]
  [string]$BuildRoot = $null,
  [Parameter(Position = 1, Mandatory = $false)]
  [string]$SourceRoot = $null
)

$lastSvnRevision = 6962
$lastSvnHash = '16cd907fe7482cb54a7374cd28b8501f138116be'

$repositoryRootPath = Join-Path $PSScriptRoot .. | Resolve-Path
if (!(git -C $repositoryRootPath rev-parse --is-inside-work-tree 2>$null)) {
  throw "$repositoryRootPath is not a git repository"
}

if ($BuildRoot -eq $null -or $BuildRoot.Trim() -eq "")  {
  $BuildRoot = $repositoryRootPath
}

# support legacy in-tree builds
if ([System.IO.Path]::GetFullPath([System.IO.Path]::Combine((pwd).Path, $BuildRoot)) -eq
  [System.IO.Path]::GetFullPath([System.IO.Path]::Combine((pwd).Path, $repositoryRootPath))) {
    $BuildRoot = Join-Path $repositoryRootPath 'build'
}
$gitVersionHeaderPath = Join-Path $BuildRoot 'git_version.h'

# The Meson project version is the single source of truth for the version users
# see: the window title, the About box and the installer all read it. The git
# revision and hash stay separate because the updater orders releases by commit
# and crash reports identify the exact build with the hash.
$projectSourceRoot = $SourceRoot
if ($projectSourceRoot -eq $null -or $projectSourceRoot.Trim() -eq '') {
  $projectSourceRoot = $repositoryRootPath.Path
}
$projectVersion = '0.0.0'
if ($projectSourceRoot -ne $null -and $projectSourceRoot.Trim() -ne '') {
  $mesonFilePath = Join-Path $projectSourceRoot 'meson.build'
  if (Test-Path -LiteralPath $mesonFilePath) {
    $mesonContent = Get-Content -LiteralPath $mesonFilePath -Raw
    # Anchor at the start of a line: project() also contains "meson_version:",
    # which a bare "version\s*:" search would match first.
    $mesonVersionMatch = [regex]::Match($mesonContent, "(?m)^\s*version\s*:\s*'([^']+)'")
    if ($mesonVersionMatch.Success) {
      $projectVersion = $mesonVersionMatch.Groups[1].Value
    }
  }
}
if ($projectVersion -notmatch '^\d+\.\d+\.\d+') {
  $projectVersion = '0.0.0'
}
# VersionInfo in src/res/res.rc wants a comma separated numeric tuple.
$resourceBaseVersion = (($projectVersion -split '[.-]')[0..2] -join ', ')

$gitRevision = $lastSvnRevision + ((git -C $repositoryRootPath log --pretty=oneline "$($lastSvnHash)..HEAD" 2>$null | Measure-Object).Count)
$gitCommit = git -C $repositoryRootPath rev-parse HEAD 2>$null
$exactGitTag = git -C $repositoryRootPath describe --exact-match --tags 2>$null
$taggedRelease = 0
if ($exactGitTag -match 'v?(\d+)\.(\d+).(\d+)(?:-(\w+))?') {
  $taggedRelease = 1
}

$newVersionH = @(
  "#define BUILD_GIT_VERSION_NUMBER $gitRevision",
  "#define BUILD_GIT_HASH `"$gitCommit`"",
  "#define BUILD_GIT_VERSION_STRING `"$projectVersion`"",
  "#define TAGGED_RELEASE $taggedRelease",
  "#define INSTALLER_VERSION `"$projectVersion`"",
  "#define RESOURCE_BASE_VERSION $resourceBaseVersion"
) -join "`n"
$newVersionH += "`n"

$currentVersionH = $null
if (Test-Path -LiteralPath $gitVersionHeaderPath) {
  $currentVersionH = (Get-Content -LiteralPath $gitVersionHeaderPath -Raw) -replace "`r`n", "`n"
}
# Only rewrite when a value changed, so an unchanged build does not make every
# dependent object stale.
if ($currentVersionH -ne $newVersionH) {
  Set-Content -LiteralPath $gitVersionHeaderPath -Value $newVersionH -NoNewline -Encoding utf8
}
