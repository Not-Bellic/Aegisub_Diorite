<#
.SYNOPSIS
    Downloads the ONNX OCR assets (PP-OCRv6 small detector/recognizer, the
    character dictionary and the ONNX Runtime DLLs) into the packaging tree.

.DESCRIPTION
    The files are published as assets of the `ocr-models-v1` GitHub release.
    Every download is verified against a pinned SHA-256 before it is used, so a
    corrupted or replaced asset fails the build instead of shipping.

    The result is laid out as:

        <DestinationDir>/onnx/onnxruntime.dll
        <DestinationDir>/onnx/onnxruntime_providers_shared.dll
        <DestinationDir>/onnx/ppocrv6_small_det.onnx
        <DestinationDir>/onnx/ppocrv6_small_rec.onnx
        <DestinationDir>/onnx/ppocrv6_dict.txt
        <DestinationDir>/onnx/LICENSE.onnxruntime.txt
        <DestinationDir>/onnx/ThirdPartyNotices.onnxruntime.txt
        <DestinationDir>/onnx/VERSION.txt
        <DestinationDir>/licenses/THIRD_PARTY_ONNX_OCR.txt

.PARAMETER DestinationDir
    Directory that will contain the OCR runtime, usually
    `build/installer-deps/ocr`. It is created when missing.

.PARAMETER SkipCheck
    Re-download even when the existing files already match their hashes.

.EXAMPLE
    tools\ocr\download_onnx_ocr.ps1 -DestinationDir build\installer-deps\ocr
#>
[CmdletBinding()]
param(
    [string]$DestinationDir = (Join-Path $PSScriptRoot "..\..\build\installer-deps\ocr"),
    [switch]$SkipCheck
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$ReleaseTag = 'ocr-models-v1'
$ReleaseBase = "https://github.com/Not-Bellic/Aegisub_Diorite/releases/download/$ReleaseTag"

# Asset name, expected size and SHA-256. Keep this in sync with
# docs/ocr-onnx-models.md when a new release is published.
$Assets = @(
    @{ Name = 'ppocrv6_small_det.onnx'; Size = 9929786; Sha256 = 'dc69df47664b71b7ed7f50cfec8b602d3826a6e90627c8b709d8c95e0e3a41db'; Kind = 'model' },
    @{ Name = 'ppocrv6_small_rec.onnx'; Size = 21104070; Sha256 = 'fd36dd7f279644b7b623c66f59c748ab6936fa2f228d8848a945b29f0954bef7'; Kind = 'model' },
    @{ Name = 'ppocrv6_dict.txt'; Size = 74012; Sha256 = 'd1979e9f794c464c0d2e0b70a7fe14dd978e9dc644c0e71f14158cdf8342af1b'; Kind = 'dict' },
    @{ Name = 'onnxruntime-win-x64-1.32.0.zip'; Size = 7941670; Sha256 = 'b874e79159101226bb593562cfdf1a1e4a48a11efd112a3f999f619b833446ed'; Kind = 'runtime' }
)

$RuntimeVersion = '1.32.0'

function Write-Step([string]$Message) {
    Write-Host "[onnx-ocr] $Message"
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-Asset([string]$Path, $Asset) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf))
        { return $false }
    if ((Get-Item -LiteralPath $Path).Length -ne $Asset.Size)
        { return $false }
    return (Get-Sha256 $Path) -eq $Asset.Sha256
}

function Save-Asset($Asset, [string]$TargetPath) {
    $url = "$ReleaseBase/$($Asset.Name)"
    Write-Step "downloading $($Asset.Name)"
    $partial = "$TargetPath.part"
    try {
        # curl streams to disk and can resume a stalled transfer; Invoke-WebRequest
        # buffers the whole body in memory and often hangs on large assets.
        # Windows 10+ ships curl.exe.
        $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
        if ($curl) {
            $attempt = 0
            $code = 1
            while ($attempt -lt 8) {
                ++$attempt
                & $curl.Source --location --fail --silent --show-error --continue-at - `
                    --connect-timeout 30 --max-time 1800 --output $partial $url
                $code = $LASTEXITCODE
                if ($code -eq 0) { break }
                # 33: the server ignored the range request, so start over.
                if ($code -eq 33) {
                    Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
                }
                Start-Sleep -Seconds ([Math]::Min(15, 2 * $attempt))
            }
            if ($code -ne 0) {
                throw "curl exited with code $code after $attempt attempts"
            }
        }
        else {
            Invoke-WebRequest -Uri $url -OutFile $partial -UseBasicParsing -MaximumRedirection 5
        }
        if ((Get-Item -LiteralPath $partial).Length -ne $Asset.Size) {
            throw "expected $($Asset.Size) bytes but received $((Get-Item -LiteralPath $partial).Length)"
        }
        $actual = Get-Sha256 $partial
        if ($actual -ne $Asset.Sha256) {
            throw "SHA-256 mismatch: expected $($Asset.Sha256), got $actual"
        }
        Move-Item -LiteralPath $partial -Destination $TargetPath -Force
    }
    catch {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        throw "Failed to fetch $($Asset.Name) from $url`n$($_.Exception.Message)"
    }
}

$DestinationDir = [System.IO.Path]::GetFullPath($DestinationDir)
$OcrDir = Join-Path $DestinationDir 'onnx'
$LicensesDir = Join-Path $DestinationDir 'licenses'
$DownloadDir = Join-Path (Split-Path -Parent $DestinationDir) '_ocr-onnx-download'

New-Item -ItemType Directory -Force -Path $OcrDir, $LicensesDir, $DownloadDir | Out-Null

$runtimeAsset = $Assets | Where-Object { $_.Kind -eq 'runtime' }
$runtimeZip = Join-Path $DownloadDir $runtimeAsset.Name

$allPresent = $true
foreach ($asset in ($Assets | Where-Object { $_.Kind -ne 'runtime' })) {
    $target = Join-Path $OcrDir $asset.Name
    if (-not $SkipCheck -and (Test-Asset $target $asset)) {
        Write-Step "up to date: $($asset.Name)"
    }
    else {
        Save-Asset $asset $target
    }
}

$runtimeDll = Join-Path $OcrDir 'onnxruntime.dll'
if ($SkipCheck -or -not (Test-Path -LiteralPath $runtimeDll -PathType Leaf)) {
    if (-not (Test-Asset $runtimeZip $runtimeAsset)) {
        Save-Asset $runtimeAsset $runtimeZip
    }

    $extractDir = Join-Path $DownloadDir 'runtime'
    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $extractDir | Out-Null
    Expand-Archive -LiteralPath $runtimeZip -DestinationPath $extractDir -Force

    $dlls = Get-ChildItem -LiteralPath $extractDir -Recurse -File -Filter 'onnxruntime*.dll'
    if (-not $dlls) {
        throw "The ONNX Runtime archive did not contain onnxruntime.dll."
    }
    foreach ($dll in $dlls) {
        Copy-Item -LiteralPath $dll.FullName -Destination (Join-Path $OcrDir $dll.Name) -Force
    }

    $license = Get-ChildItem -LiteralPath $extractDir -Recurse -File -Filter 'LICENSE' |
        Select-Object -First 1
    if ($license) {
        Copy-Item -LiteralPath $license.FullName -Destination (Join-Path $OcrDir 'LICENSE.onnxruntime.txt') -Force
    }

    $notices = Get-ChildItem -LiteralPath $extractDir -Recurse -File -Filter 'ThirdPartyNotices.txt' |
        Select-Object -First 1
    if ($notices) {
        Copy-Item -LiteralPath $notices.FullName -Destination (Join-Path $OcrDir 'ThirdPartyNotices.onnxruntime.txt') -Force
    }

    Set-Content -LiteralPath (Join-Path $OcrDir 'VERSION.txt') -Encoding ASCII `
        -Value "onnxruntime $RuntimeVersion (win-x64, CPU)"
}

$thirdParty = @"
ONNX OCR runtime
================

Models: PP-OCRv6 small text detector and recognizer, exported by PaddleOCR to
ONNX. Copyright PaddlePaddle, licensed under the Apache License 2.0.
  https://github.com/PaddlePaddle/PaddleOCR
  https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0/

Runtime: ONNX Runtime $RuntimeVersion for Windows x64 (CPU), Copyright Microsoft
Corporation, licensed under the MIT License. See LICENSE.onnxruntime.txt and
ThirdPartyNotices.onnxruntime.txt next to the runtime DLL.

The files are distributed with Aegisub as assets of the $ReleaseTag release.
"@
Set-Content -LiteralPath (Join-Path $LicensesDir 'THIRD_PARTY_ONNX_OCR.txt') -Encoding UTF8 -Value $thirdParty

$required = @(
    'onnxruntime.dll',
    'ppocrv6_small_det.onnx',
    'ppocrv6_small_rec.onnx',
    'ppocrv6_dict.txt'
)
$missing = @()
foreach ($name in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $OcrDir $name) -PathType Leaf)) {
        $missing += $name
    }
}
if ($missing.Count -gt 0) {
    throw "The ONNX OCR runtime is incomplete, missing: $($missing -join ', ')"
}

Write-Step "ONNX OCR runtime ready at $OcrDir"
