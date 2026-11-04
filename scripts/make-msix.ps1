# Builds the Microsoft Store package (MSIX) from a Release build.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make-msix.ps1 -BinDir <dir> -Version 1.0.0 -OutDir <dir> [-Test]
#
# Writes <OutDir>\InstanceManagerForWSL-<version>-x64.msix: unsigned, for uploading
# to Partner Center, which signs it.
#
# With -Test it also writes InstanceManagerForWSL-<version>-x64-test.msix, signed
# with a self-signed test certificate that has the Store publisher's subject. The
# certificate is created on first use in Cert:\CurrentUser\My, and its public part
# is exported to <OutDir>\InstanceManagerForWSL-test.cer. To install the test
# package, trust that certificate once (from an elevated PowerShell), then add the
# package:
#
#   Import-Certificate -FilePath <test.cer> -CertStoreLocation Cert:\LocalMachine\TrustedPeople
#   Add-AppxPackage -Path <test.msix>
#
# Needs makeappx.exe, makepri.exe and signtool.exe from the Windows 10/11 SDK.

param(
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$OutDir,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$source = Join-Path $root 'packaging\msix'

# The publisher from Partner Center (Product management -> Product identity).
$storePublisher = 'CN=370AC4C1-5592-49EF-8306-6C22A78CD3A9'
$testCertName = 'Instance Manager for WSL test signing'

function Find-SdkTool([string]$name) {
    $bin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $tool = Get-ChildItem $bin -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.' } | Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$name" } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $tool) { throw "$name not found. Install the Windows SDK (Visual Studio Installer -> Individual components)." }
    return $tool
}

function Invoke-Tool([string]$tool, [string[]]$arguments) {
    $output = & $tool @arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "$([System.IO.Path]::GetFileName($tool)) failed (exit code $LASTEXITCODE)."
    }
}

$makeappx = Find-SdkTool 'makeappx.exe'
$makepri = Find-SdkTool 'makepri.exe'

# The Store wants a four-part version whose last part is 0.
$parts = $Version.Split('.')
while ($parts.Count -lt 3) { $parts += '0' }
$packageVersion = ($parts[0..2] -join '.') + '.0'

New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

function Build-Package([string]$publisher, [string]$file) {
    $layout = Join-Path $OutDir 'msix-layout'
    if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
    New-Item -ItemType Directory -Force (Join-Path $layout 'Assets') | Out-Null

    Copy-Item (Join-Path $BinDir 'wslmgr-gui.exe'), (Join-Path $BinDir 'wslmgr.exe') $layout
    Copy-Item (Join-Path $source 'Assets\*.png') (Join-Path $layout 'Assets')
    Copy-Item (Join-Path $root 'LICENSE') (Join-Path $layout 'LICENSE.txt')
    $manifest = (Get-Content -Raw (Join-Path $source 'AppxManifest.xml')).
        Replace('{VERSION}', $packageVersion).Replace('{PUBLISHER}', $publisher)
    [System.IO.File]::WriteAllText((Join-Path $layout 'AppxManifest.xml'), $manifest, (New-Object System.Text.UTF8Encoding $false))

    # resources.pri maps Assets\Square44x44Logo.png and friends to their scale and size variants.
    $config = Join-Path $OutDir 'priconfig.xml'
    Invoke-Tool $makepri @('createconfig', '/cf', $config, '/dq', 'en-US', '/pv', '10.0.0', '/o')
    Invoke-Tool $makepri @('new', '/pr', $layout, '/cf', $config, '/mn', (Join-Path $layout 'AppxManifest.xml'),
                           '/of', (Join-Path $layout 'resources.pri'), '/o')
    Remove-Item $config

    Invoke-Tool $makeappx @('pack', '/d', $layout, '/p', $file, '/o')
    Remove-Item -Recurse -Force $layout
    Write-Host "Built: $file"
}

$name = "InstanceManagerForWSL-$Version-x64"
Build-Package $storePublisher (Join-Path $OutDir "$name.msix")

if ($Test) {
    $testFile = Join-Path $OutDir "$name-test.msix"
    Copy-Item (Join-Path $OutDir "$name.msix") $testFile -Force
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $storePublisher -and $_.FriendlyName -eq $testCertName } |
        Sort-Object NotAfter -Descending | Select-Object -First 1
    if (-not $cert) {
        # Code signing (1.3.6.1.5.5.7.3.3), not a CA (empty basic constraints).
        $cert = New-SelfSignedCertificate -Type Custom -Subject $storePublisher -FriendlyName $testCertName `
            -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(2) `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
        Write-Host "Created test certificate $($cert.Thumbprint) in Cert:\CurrentUser\My"
    }
    Export-Certificate -Cert $cert -FilePath (Join-Path $OutDir 'InstanceManagerForWSL-test.cer') | Out-Null
    Invoke-Tool (Find-SdkTool 'signtool.exe') @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, '/s', 'My', $testFile)
    Write-Host "Built: $testFile (signed with the test certificate)"
}
