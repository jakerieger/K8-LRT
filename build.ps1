param (
    [string]$Config # "debug" or "release"
)

$vsPath = &(Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") -latest -property installationPath

$modulePath = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"

Import-Module $modulePath
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch amd64 -HostArch amd64

Write-Host "VS Environment Active. Running nmake..." -ForegroundColor Cyan
nmake clean
nmake CONF=$Config
