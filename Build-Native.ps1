param([switch]$StagingOnly)
$ErrorActionPreference = 'Stop'
$toolchain = Get-ChildItem (Join-Path $PSScriptRoot '.toolchain') -Directory -Filter 'llvm-mingw-*-ucrt-x86_64' | Sort-Object Name -Descending | Select-Object -First 1
if (-not $toolchain) { throw '请先将官方 LLVM-MinGW x64 UCRT 工具链解压到 .toolchain，详见 native/README.md。' }
$bin = Join-Path $toolchain.FullName 'bin'
$outDir = Join-Path $PSScriptRoot '.build'
[void][IO.Directory]::CreateDirectory($outDir)
Push-Location (Join-Path $PSScriptRoot 'native')
try {
    & "$bin\x86_64-w64-mingw32-windres.exe" app.rc -O coff -o "$outDir\native.res"
    if ($LASTEXITCODE -ne 0) { throw '资源编译失败' }
    & "$bin\clang++.exe" --target=x86_64-w64-mingw32 -std=c++17 -O2 -Wall -Wextra -municode -mwindows -static main.cpp "$outDir\native.res" -o "$outDir\WSLTunnel-Native.exe" -lcomctl32 -luser32 -lgdi32 -lshell32 -lole32 -ladvapi32 -luxtheme -lws2_32 -luuid
    if ($LASTEXITCODE -ne 0) { throw '原生 C++ 构建失败' }
} finally { Pop-Location }
if (-not $StagingOnly) {
    $publishDir = Join-Path $PSScriptRoot 'dist'
    [void][IO.Directory]::CreateDirectory($publishDir)
    Copy-Item "$outDir\WSLTunnel-Native.exe","$PSScriptRoot\wsl_tunnel.py","$PSScriptRoot\start.sh","$PSScriptRoot\README.md" -Destination $publishDir -Force
}
Get-FileHash "$outDir\WSLTunnel-Native.exe" -Algorithm SHA256
