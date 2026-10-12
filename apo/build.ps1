# Build mikraf-apo.dll (x64 COM capture APO) into native\bin.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
$bin = Join-Path $root "native\bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null

$sdk = "C:\Program Files (x86)\Windows Kits\10"
$ver = "10.0.26100.0"
$msvc = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207"
$clang = "C:\Program Files\LLVM\bin\clang-cl.exe"
if (-not (Test-Path $clang)) { throw "clang-cl not found" }

$env:INCLUDE = @(
    "$msvc\include",
    "$sdk\Include\$ver\ucrt",
    "$sdk\Include\$ver\shared",
    "$sdk\Include\$ver\um"
) -join ";"
$env:LIB = @(
    "$msvc\lib\x64",
    "$sdk\Lib\$ver\ucrt\x64",
    "$sdk\Lib\$ver\um\x64"
) -join ";"

$out = Join-Path $bin "mikraf-apo.dll"
& $clang /nologo /O2 /MT /LD /EHsc /std:c++20 /DUNICODE /D_UNICODE /DWIN32 /D_CRT_SECURE_NO_WARNINGS `
    /I $here `
    (Join-Path $here "apo.cpp") `
    (Join-Path $here "dsp.cpp") `
    /link /OUT:$out ole32.lib uuid.lib advapi32.lib mmdevapi.lib propsys.lib user32.lib shell32.lib oleaut32.lib cfgmgr32.lib
if ($LASTEXITCODE -ne 0) { throw "apo build failed" }
Write-Output "built $out"
