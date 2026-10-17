param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$OrtRoot = "$PSScriptRoot/../external/onnxruntime-win-x64-1.23.2",
    [string]$Destination = "$PSScriptRoot/../dist/FaceVitals-0.6.0-windows-x64"
)
$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path "$PSScriptRoot/..").Path
if (Test-Path -LiteralPath $Destination) { throw 'Destination exists; choose a new directory.' }
New-Item -ItemType Directory -Path $Destination | Out-Null
$Destination = (Resolve-Path -LiteralPath $Destination).Path
Copy-Item -LiteralPath "$projectRoot/build/windows/Release/FaceVitals.exe" -Destination $Destination
Copy-Item -LiteralPath "$OrtRoot/lib/onnxruntime.dll" -Destination $Destination
$env:PATH = "$QtRoot/bin;$env:PATH"
& "$QtRoot/tools/Qt6/bin/windeployqt.exe" --release --no-translations --no-opengl-sw --no-system-d3d-compiler --no-system-dxc-compiler --no-compiler-runtime --skip-plugin-types generic,networkinformation,tls --exclude-plugins qsqlpsql,qdirect2d "$Destination/FaceVitals.exe"
if ($LASTEXITCODE) { throw 'Qt deployment failed' }
# 随程序分发 MSVC 运行库，不捆绑编译器。
$vswhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$vs = & $vswhere -latest -property installationPath
$crt = Get-ChildItem "$vs/VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1
if (!$crt) { throw 'MSVC redistributable DLLs not found' }
Copy-Item -Path "$($crt.FullName)/*.dll" -Destination $Destination
# vcpkg 的 Qt 还链接了字体、图像与压缩 DLL；递归分析 PE 依赖，补齐实际使用的库。
$dumpbin = Get-ChildItem "$vs/VC/Tools/MSVC/*/bin/Hostx64/x64/dumpbin.exe" | Sort-Object FullName -Descending | Select-Object -First 1
$checked = @{}
do {
    $pending = @(Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object { $_.Extension -in '.dll','.exe' -and !$checked.ContainsKey($_.FullName) })
    foreach ($binary in $pending) {
        $checked[$binary.FullName] = $true
        foreach ($line in (& $dumpbin.FullName /dependents $binary.FullName)) {
            if ($line -match '^\s+([a-zA-Z0-9_.-]+\.dll)\s*$') {
                $dependency = $Matches[1]
                if ((Test-Path -LiteralPath "$QtRoot/bin/$dependency") -and !(Test-Path -LiteralPath "$Destination/$dependency")) {
                    Copy-Item -LiteralPath "$QtRoot/bin/$dependency" -Destination $Destination
                }
            }
        }
    }
} while ($pending.Count -gt 0)
# 发布门槛：所有 PE 依赖必须能在包内或 Windows 系统目录中找到。
foreach ($binary in (Get-ChildItem -LiteralPath $Destination -Recurse -File | Where-Object Extension -in '.dll','.exe')) {
    foreach ($line in (& $dumpbin.FullName /dependents $binary.FullName)) {
        if ($line -match '^\s+([a-zA-Z0-9_.-]+\.dll)\s*$') {
            $dependency=$Matches[1]
            if ($dependency -notmatch '^(api-ms-|ext-ms-)' -and !(Test-Path -LiteralPath "$Destination/$dependency") -and !(Test-Path -LiteralPath "$env:SystemRoot/System32/$dependency")) {
                throw "Missing dependency: $($binary.Name) -> $dependency"
            }
        }
    }
}
@('[Paths]','Prefix=.','Plugins=.') | Set-Content -LiteralPath "$Destination/qt.conf" -Encoding ascii
@('@echo off','set "QT_PLUGIN_PATH="','set "QT_QPA_PLATFORM_PLUGIN_PATH="','set "QT_QPA_PLATFORM=windows"','start "FaceVitals" "%~dp0FaceVitals.exe" %*') | Set-Content -LiteralPath "$Destination/Start-FaceVitals.cmd" -Encoding ascii
New-Item -ItemType Directory -Path "$Destination/models","$Destination/licenses" | Out-Null
Copy-Item -LiteralPath "$projectRoot/models/bootstrap" -Destination "$Destination/models" -Recurse
Copy-Item -LiteralPath "$QtRoot/share/qtbase/copyright" -Destination "$Destination/licenses/Qt.txt"
foreach ($port in @('icu','libpng','harfbuzz','md4c','freetype','zlib','pcre2','double-conversion','sqlite3','brotli','bzip2','zstd','libjpeg-turbo','openssl')) {
    if (Test-Path -LiteralPath "$QtRoot/share/$port/copyright") {
        Copy-Item -LiteralPath "$QtRoot/share/$port/copyright" -Destination "$Destination/licenses/$port.txt"
    }
}
Copy-Item -LiteralPath "$OrtRoot/LICENSE" -Destination "$Destination/licenses/ONNXRuntime.txt"
Copy-Item -LiteralPath "$OrtRoot/ThirdPartyNotices.txt" -Destination "$Destination/licenses/ONNXRuntime-ThirdPartyNotices.txt"
Copy-Item -LiteralPath "$projectRoot/README.md","$projectRoot/DEPENDENCIES.md","$projectRoot/LICENSE","$projectRoot/THIRD_PARTY.md","$projectRoot/更新说明_0.6.md","$projectRoot/摄像头与户外演示指南.md" -Destination $Destination
New-Item -ItemType Directory -Path "$Destination/validation" | Out-Null
Copy-Item -LiteralPath "$projectRoot/validation/v06" -Destination "$Destination/validation" -Recurse
$inventory = Get-ChildItem -LiteralPath $Destination -File -Recurse | ForEach-Object {
    [PSCustomObject]@{ path = [IO.Path]::GetRelativePath($Destination, $_.FullName); bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower() }
}
$inventory | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath "$Destination/files.json" -Encoding utf8
Write-Output ('Package size: {0:N2} MiB' -f (($inventory | Measure-Object bytes -Sum).Sum / 1MB))
