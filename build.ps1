param (
    [string]$Config # "debug" or "release"
)

# Load Visual Studio environment
$vsPath = &(Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") -latest -property installationPath
$modulePath = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $modulePath
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch amd64 -HostArch amd64
Write-Host "Visual Studio Environment Active." -ForegroundColor Cyan

# Increment build number
Write-Host "Incrementing build number..." -ForegroundColor Cyan
$version_header = "src/version.h"
$header_content = Get-Content $version_header -Raw
$pattern = '(?m)(#define\s+VER_BUILD\s+)(\d+)'
$new_content = [regex]::Replace($header_content, $pattern, {
        param($m) 
        $prefix = $m.Groups[1].Value
        $value = [int]$m.Groups[2].Value + 1
        return "${prefix}${value}"
    })
Set-Content -Path $version_header -Value $new_content -Encoding UTF8

# Build K8-LRT
Write-Host "Running nmake..." -ForegroundColor Cyan
nmake clean
nmake CONF=$Config
