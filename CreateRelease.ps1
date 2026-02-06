# For internal use only - not meant for contributors or users

$targetName = "K8Tool"
$platform = "Win64"
$buildDir = "build/Release"
$coreCount = $env:NUMBER_OF_PROCESSORS

$cmakeContent = Get-Content "CMakeLists.txt" -Raw
$major = if ($cmakeContent -match 'set\(VER_MAJOR\s+(?<v>\d+)\)') { $Matches['v'] } else { "0" }
$minor = if ($cmakeContent -match 'set\(VER_MINOR\s+(?<v>\d+)\)') { $Matches['v'] } else { "0" }
$patch = if ($cmakeContent -match 'set\(VER_PATCH\s+(?<v>\d+)\)') { $Matches['v'] } else { "0" }

$buildNum = if (Test-Path ".build") { Get-Content ".build" | Out-String } else { "0" }
$buildNum = $buildNum.Trim()

$version     = "$major.$minor.$patch.$buildNum"
$releaseName = "K8Tool-$version-$platform"
$stagingDir  = Join-Path $buildDir $releaseName
$zipName     = "$releaseName.zip"
$zipPath     = Join-Path $buildDir $zipName
$hashPath    = Join-Path $buildDir "$zipName.sha256"

Write-Host "Detected Version: $version" -ForegroundColor Cyan

$vsPath = &(Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe") -latest -property installationPath
$modulePath = Join-Path $vsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Import-Module $modulePath
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -Arch amd64 -HostArch amd64

Write-Host "--- VS Environment Active ---" -ForegroundColor Green

if (Test-Path "build")
{
    Write-Host "Cleaning up old build directory..." -ForegroundColor Cyan
    Remove-Item -Recurse -Force "build"
}


New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Write-Host "Configuring with MSVC and Ninja..." -ForegroundColor Cyan
cmake -S . -B $buildDir `
    -G "Ninja" `
    -D CMAKE_BUILD_TYPE=Release `
    -D CMAKE_C_COMPILER=cl `
    -D CMAKE_CXX_COMPILER=cl

if ($LASTEXITCODE -eq 0)
{
    Write-Host "Using $coreCount cores for parallel build." -ForegroundColor Magenta

    Write-Host "Starting build..." -ForegroundColor Cyan
    cmake --build $buildDir --config Release -j $coreCount

    $exePath = Join-Path $buildDir "bin/$targetName.exe"

    if (Test-Path $exePath) {
        New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null
        Copy-Item -Path $exePath -Destination (Join-Path $stagingDir "$releaseName.exe")

        if (Test-Path "LICENSE") {
            Copy-Item -Path "LICENSE" -Destination $stagingDir
        }

        Compress-Archive -Path "$stagingDir\*" -DestinationPath $zipPath -Force

        Write-Host "Generating SHA256 checksum..." -ForegroundColor Cyan
        # Format: [HASH] *[FILENAME] (Standard format for verification tools)
        $hash = (Get-FileHash -Path $zipPath -Algorithm SHA256).Hash.ToLower()
        "$hash *$zipName" | Out-File -FilePath $hashPath -Encoding ascii

        Remove-Item -Recurse -Force $stagingDir

        Write-Host "`nPackage Created: $zipPath" -ForegroundColor Green
        Write-Host "Checksum Created: $hashPath" -ForegroundColor Green
    } else {
        Write-Error "Could not find executable at $exePath"
    }
}
else
{
    Write-Error "CMake configuration failed. Build aborted."
}