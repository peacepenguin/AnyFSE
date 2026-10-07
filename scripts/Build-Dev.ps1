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

.PARAMETER AllowUpdates
    Keep the updater enabled. By default dev builds compile it out (ANYFSE_DISABLE_UPDATES).

.PARAMETER Lto
    Build with whole program optimization (-p:Lto=true). Slower to build; used by the release workflow.

.PARAMETER Revision
    Fixed build revision (the fourth version number). CI passes this so builds are reproducible and Package no longer relies on the
    revision bumped in AnyFSE.Version.props. Omit it locally to keep the auto-increment behaviour.

.PARAMETER Install
    Launch the freshly built offline installer elevated and wait for it to finish.

.EXAMPLE
    .\scripts\Build-Dev.ps1 -Install
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string] $Configuration = 'Release',
    [switch] $Install,
    [switch] $Lto,
    [Nullable[int]] $Revision = $null,
    [switch] $AllowUpdates
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
    $cppTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    # Prefer VS 2022 (the projects use toolset v143), then any VS 2022 product, then the newest VS with the C++ tools
    $vsInstall = @(
        { & $vswhere -version '[17.0,18.0)' -products * -requires $cppTools -property installationPath },
        { & $vswhere -version '[17.0,18.0)' -products * -property installationPath },
        { & $vswhere -latest -products * -requires $cppTools -property installationPath }
    ) | ForEach-Object { & $_ | Select-Object -First 1 } | Where-Object { $_ } | Select-Object -First 1
    if (-not $vsInstall) {
        Write-Host 'Visual Studio instances found by vswhere:'
        & $vswhere -all -products * -format text | Write-Host
        throw 'No Visual Studio installation with the C++ tools was found.'
    }
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
# Dev builds must not offer or install upstream releases over the build under test
if (-not $AllowUpdates) { $overrides += '-p:DisableUpdates=true' }
if ($Lto) { $overrides += '-p:Lto=true' }

# The project files shell out to Windows PowerShell (powershell.exe) to find the signing certificate. When this script runs
# under PowerShell 7 that child inherits pwsh's module path and fails to load the certificate provider's parameters
# (e.g. "A parameter cannot be found that matches ... CodeSigningCert"), so hand it the machine's default module path.
if ($PSVersionTable.PSEdition -eq 'Core') {
    $env:PSModulePath = [Environment]::GetEnvironmentVariable('PSModulePath', 'Machine')
}

# --- Build (same steps as .vscode\tasks.json) ----------------------------------------------------------------------
$common = @("-property:Configuration=$Configuration", '-property:Platform=x64', '-maxcpucount') + $overrides
# Package stamps the appx with VersionRevision and then bumps it, and the installer uses VersionRevision - 1 to name itself after
# that package, so with a fixed revision R the installer must be built with R + 1.
$installerRevision = @()
if ($null -ne $Revision) {
    $common += "-property:VersionRevision=$Revision"
    $installerRevision = @("-property:VersionRevision=$($Revision + 1)")
}
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
    msbuild.exe (Join-Path $repo 'AnyFSE.Installer.vcxproj') @common '-property:Offline=Offline' @installerRevision
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
