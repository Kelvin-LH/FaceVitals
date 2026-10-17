param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$OrtRoot = "$PSScriptRoot/../external/onnxruntime-win-x64-1.23.2",
    [string]$CMake = 'cmake'
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot/..").Path
& $CMake -S $projectRoot -B "$projectRoot/build/windows" -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot" "-DONNXRUNTIME_ROOT=$OrtRoot"
if ($LASTEXITCODE) { throw 'Configure failed' }
& $CMake --build "$projectRoot/build/windows" --config Release --parallel
if ($LASTEXITCODE) { throw 'Build failed' }
$env:PATH = "$QtRoot/bin;$env:PATH"
$env:QT_PLUGIN_PATH = "$QtRoot/Qt6/plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = ''
$env:QT_QPA_FONTDIR = "$env:SystemRoot/Fonts"
& (Join-Path (Split-Path (Get-Command $CMake).Source) 'ctest.exe') --test-dir "$projectRoot/build/windows" -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
