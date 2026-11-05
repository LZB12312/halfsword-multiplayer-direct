param([string]$Toolchain = $env:LLVM_MINGW, [ValidateSet('Release','Debug')][string]$Configuration = 'Release', [switch]$Incremental)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot
if (!$Toolchain) { throw 'Set LLVM_MINGW or pass -Toolchain pointing to a Windows LLVM-MinGW distribution.' }
$cc = Join-Path $Toolchain 'bin/x86_64-w64-mingw32-clang.exe'
$cxx = Join-Path $Toolchain 'bin/x86_64-w64-mingw32-clang++.exe'
$opt = if ($Configuration -eq 'Debug') { '-O0' } else { '-O2' }
$common = @($opt, '-Wall', '-Wextra', '-D_WIN32_WINNT=0x0A00', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-fstack-protector-strong')
$inc = @('-Isrc','-I../../shared','-I../../shared/protocol','-Ithird_party/minhook/include','-Ithird_party/enet/include','-Ithird_party/miniupnpc/include','-iquotethird_party/miniupnpc','-DMINIUPNP_STATICLIB')
$describe = (git describe --always --dirty --exclude='*') -replace '[^A-Za-z0-9._+-]', ''
New-Item -ItemType Directory -Force build/obj | Out-Null
$objects = @()
$sources = @('third_party/minhook/src/buffer.c','third_party/minhook/src/hook.c','third_party/minhook/src/trampoline.c','third_party/minhook/src/hde/hde64.c')
$sources += @('callbacks','compress','host','list','packet','peer','protocol','win32') | ForEach-Object { "third_party/enet/$_.c" }
$sources += @('igd_desc_parse','miniupnpc','minixml','minisoap','minissdpc','miniwget','upnpcommands','upnpdev','upnpreplyparse','upnperrors','connecthostport','portlistingparse','receivedata','addr_is_reserved') | ForEach-Object { "third_party/miniupnpc/src/$_.c" }
foreach ($file in $sources) {
    $obj = 'build/obj/' + ($file -replace '[/\\.]','_') + '.o'
    if ($Incremental -and (Test-Path $obj) -and (Get-Item $obj).LastWriteTime -gt (Get-Item $file).LastWriteTime) { $objects += $obj; continue }
    & $cc @common @inc -w -c $file -o $obj
    if ($LASTEXITCODE) { throw "Compilation failed: $file" }
    $objects += $obj
}
foreach ($file in Get-ChildItem src -Recurse -Filter *.cpp) {
    $relative = [IO.Path]::GetRelativePath($PSScriptRoot,$file.FullName)
    $obj = 'build/obj/' + ($relative -replace '[/\\.]','_') + '.o'
    if ($Incremental -and (Test-Path $obj) -and (Get-Item $obj).LastWriteTime -gt $file.LastWriteTime) { $objects += $obj; continue }
    & $cxx '-std=c++20' @common @inc '-Werror' "-DHSM_GIT_DESCRIBE=`"$describe`"" -c $relative -o $obj
    if ($LASTEXITCODE) { throw "Compilation failed: $relative" }
    $objects += $obj
}
& $cxx -shared -o build/dwmapi.dll @objects src/dwmapi.def '-fstack-protector-strong' '-s' '-Wl,-Map,build/dwmapi.map' '-static' '-static-libgcc' '-static-libstdc++' '-Wl,--kill-at' '-lws2_32' '-lwinmm' '-liphlpapi'
if ($LASTEXITCODE) { throw 'Link failed' }
'Built build/dwmapi.dll'
