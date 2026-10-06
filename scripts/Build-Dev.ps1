<#
.SYNOPSIS
    One-step local build of the AnyFSE offline installer with auto-generated dev signing certificates.

.DESCRIPTION
    - Finds Visual Studio (vswhere) and activates its developer environment when msbuild is not already on PATH.
    - Creates a self-signed code-signing certificate in Cert:\CurrentUser\My when no usable one exists. Its subject is
      the PublisherCN from AnyFSE.Version.props, so AppxManifest.xml, the package family name and Constants.hpp stay unchanged.
    - Signs binaries with the real BinarySigningCN certificate when you have it, otherwise with the dev certificate.
    - Exports the public certificate to build\dev\AnyFSE.Temp.cer so the tracked AnyFSE.Temp.cer is not touched.
      The installer imports that certificate into LocalMachine\TrustedPeople itself and removes it afterwards.
    - Runs the same build steps as the tasks in .vscode\tasks.json, then optionally launches the installer.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Install
    Launch the freshly built offline installer elevated and wait for it to finish.

.EXAMPLE
    .\scripts\Build-Dev.ps1 -Install
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Install
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Get-VersionProp([string] $Name) {
    $xml = [xml](Get-Content (Join-Path $repo 'AnyFSE.Version.props'))
    $node = $xml.Project.PropertyGroup.$Name | Where-Object { $_ } | Select-Object -First 1
    if (-not $node) { throw "Property $Name not found in AnyFSE.Version.props" }
    return $node.Trim()
}

function Find-SigningCert([string] $Subject) {
    Get-ChildItem Cert:\CurrentUser\My -CodeSigningCert -ErrorAction SilentlyContinue |
        Where-Object { $_.Subject -eq "CN=$Subject" -and $_.HasPrivateKey -and $_.NotBefore -le (Get-Date) -and $_.NotAfter -gt (Get-Date) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
}

function Invoke-Step([string] $Title, [scriptblock] $Command) {
    Write-Host "`n==> $Title" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Title failed with exit code $LASTEXITCODE" }
}

# --- Visual Studio environment -------------------------------------------------------------------------------------
if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio 2022 with the Desktop development with C++ workload.' }
    $vsInstall = & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1
    if (-not $vsInstall) { throw 'No Visual Studio 2022 installation with the C++ tools was found.' }
    Write-Host "Using Visual Studio at $vsInstall"
    & (Join-Path $vsInstall 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
    Set-Location $repo
}

# --- Certificates --------------------------------------------------------------------------------------------------
$publisherCn = Get-VersionProp 'PublisherCN'
$binaryCn = Get-VersionProp 'BinarySigningCN'
$overrides = @()

$packageCert = Find-SigningCert $publisherCn
if (-not $packageCert) {
    Write-Host "Creating self-signed dev certificate CN=$publisherCn in Cert:\CurrentUser\My"
    # Settings from Microsoft's documented test-certificate recipe for MSIX/APPX packages
    $packageCert = New-SelfSignedCertificate -Type Custom -Subject "CN=$publisherCn" -FriendlyName 'AnyFSE dev package signing' `
        -KeyUsage DigitalSignature -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable `
        -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(2) `
        -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
}

if (-not (Find-SigningCert $binaryCn)) {
    Write-Host "No certificate for CN=$binaryCn; signing binaries with the dev certificate instead"
    $overrides += "-p:BinarySigningCN=$publisherCn"
}

$devDir = Join-Path $repo 'build\dev'
New-Item -ItemType Directory -Path $devDir -Force | Out-Null
$tempCert = Join-Path $devDir 'AnyFSE.Temp.cer'
Export-Certificate -Cert $packageCert -FilePath $tempCert -Force | Out-Null
$overrides += "-p:TempCertPath=$tempCert"

# --- Build (same steps as .vscode\tasks.json) ----------------------------------------------------------------------
$common = @("-property:Configuration=$Configuration", '-property:Platform=x64') + $overrides
$sln = Join-Path $repo 'AnyFSE.sln'

Invoke-Step "Build AnyFSE $Configuration" {
    msbuild.exe $sln @common '-target:AnyFSE;AnyFSE_Settings;AnyFSE_ACSEFilterHook;AnyFSE_ACSEFilterInjector'
}
Invoke-Step "Build AnyFSE.Uninstaller $Configuration" {
    msbuild.exe (Join-Path $repo 'AnyFSE.Uninstaller.vcxproj') @common
}
Invoke-Step "Package $Configuration" {
    msbuild.exe $sln @common '-target:AnyFSE_Package'
}
Invoke-Step "Build AnyFSE.Installer Offline $Configuration" {
    msbuild.exe (Join-Path $repo 'AnyFSE.Installer.vcxproj') @common '-property:Offline=Offline'
}

$installer = Get-ChildItem (Join-Path $repo "build\$Configuration") -Filter 'AnyFSE.Installer.Offline.*.exe' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $installer) { throw "Offline installer not found in build\$Configuration" }
Write-Host "`nBuilt: $($installer.FullName)" -ForegroundColor Green

# --- Install -------------------------------------------------------------------------------------------------------
if ($Install) {
    Write-Host 'Launching installer (elevation prompt expected)...'
    Start-Process -FilePath $installer.FullName -Verb RunAs -Wait
    Write-Host 'Installer finished.' -ForegroundColor Green
}
