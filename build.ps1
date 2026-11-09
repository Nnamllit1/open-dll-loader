param(
    [ValidateSet('x64', 'Win32', 'all')][string]$Architecture = 'all',
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
$env:MSBUILDDISABLENODEREUSE = '1'
$projectRoot = $PSScriptRoot
$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) { $cmakeExe = $cmakeCommand.Source }
else {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio with Desktop development with C++ and CMake.' }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmakeExe = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakeExe)) { throw 'CMake was not found.' }
$ctestExe = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
$architectures = if ($Architecture -eq 'all') { @('x64', 'Win32') } else { @($Architecture) }
foreach ($arch in $architectures) {
    $buildPath = Join-Path $projectRoot "build\$arch"
    & $cmakeExe -S $projectRoot -B $buildPath -G 'Visual Studio 17 2022' -A $arch
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($arch)." }
    & $cmakeExe --build $buildPath --config Release --parallel 4
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($arch)." }
    if (-not $SkipTests) {
        & $ctestExe --test-dir $buildPath -C Release --output-on-failure
        if ($LASTEXITCODE -ne 0) { throw "Tests failed ($arch)." }
    }
    $label = if ($arch -eq 'Win32') { 'x86' } else { 'x64' }
    $distPath = Join-Path $projectRoot "dist\$label"
    New-Item -ItemType Directory -Path $distPath -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $buildPath 'Release\OpenDllLoader.exe'),(Join-Path $buildPath 'Release\open-dll-loader.exe') -Destination $distPath
    Copy-Item -LiteralPath (Join-Path $buildPath 'Release\odl-demo-launcher.exe') -Destination $distPath
    foreach($file in @('odl-plugin-host.exe','reloadable-plugin-v1.dll','reloadable-plugin-v2.dll')) {
        Copy-Item -LiteralPath (Join-Path $buildPath "examples\reloadable-host\Release\$file") -Destination $distPath
    }
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'README.md'),(Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $projectRoot 'dist')
$distRoot = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path (Join-Path $distRoot 'docs') -Force | Out-Null
if (Test-Path -LiteralPath (Join-Path $projectRoot 'docs\interface.png')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\interface.png') -Destination (Join-Path $distRoot 'docs')
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\linux-wine-proton.md') -Destination (Join-Path $distRoot 'docs')
Copy-Item -LiteralPath (Join-Path $projectRoot 'examples\reloadable-host\README.md') -Destination (Join-Path $distRoot 'docs\hot-reload.md')
& python (Join-Path $projectRoot 'scripts\checksums.py') write $distRoot
if ($LASTEXITCODE -ne 0) { throw 'Checksum generation failed.' }
