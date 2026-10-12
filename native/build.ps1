# Builds mikraf-speex.dll and mikraf-rnnoise.dll next to this script's bin folder.
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here "bin"
New-Item -ItemType Directory -Force -Path $bin | Out-Null

$speex = Join-Path $env:TEMP "speexdsp"
$rnnRoot = Join-Path $env:TEMP "obs-rnnoise"
$rnn = Join-Path $rnnRoot "plugins\obs-filters\rnnoise"
if (-not (Test-Path (Join-Path $speex "libspeexdsp\preprocess.c"))) {
    git clone --depth 1 https://github.com/xiph/speexdsp.git $speex
}
if (-not (Test-Path (Join-Path $rnn "src\rnn_data.c"))) {
    if (Test-Path $rnnRoot) { Remove-Item $rnnRoot -Recurse -Force }
    git clone --depth 1 --branch 32.2.1 --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git $rnnRoot
    Push-Location $rnnRoot
    git sparse-checkout set plugins/obs-filters/rnnoise
    Pop-Location
}

$clang = (Get-Command clang).Source
$spxOut = Join-Path $bin "mikraf-speex.dll"
& $clang -O2 -shared -o $spxOut `
    -I (Join-Path $speex "include") `
    -I (Join-Path $speex "libspeexdsp") `
    -include (Join-Path $here "speex_config.h") `
    (Join-Path $here "mikraf_speex.c") `
    (Join-Path $speex "libspeexdsp\preprocess.c") `
    (Join-Path $speex "libspeexdsp\fftwrap.c") `
    (Join-Path $speex "libspeexdsp\filterbank.c") `
    (Join-Path $speex "libspeexdsp\kiss_fft.c") `
    (Join-Path $speex "libspeexdsp\kiss_fftr.c")
if ($LASTEXITCODE -ne 0) { throw "speex build failed" }

$rnnOut = Join-Path $bin "mikraf-rnnoise.dll"
& $clang -O2 -shared -o $rnnOut `
    -DCOMPILE_OPUS `
    -I (Join-Path $rnn "include") `
    -I (Join-Path $rnn "src") `
    (Join-Path $here "mikraf_rnnoise.c") `
    (Join-Path $rnn "src\denoise.c") `
    (Join-Path $rnn "src\rnn.c") `
    (Join-Path $rnn "src\rnn_data.c") `
    (Join-Path $rnn "src\pitch.c") `
    (Join-Path $rnn "src\celt_lpc.c") `
    (Join-Path $rnn "src\kiss_fft.c")
if ($LASTEXITCODE -ne 0) { throw "rnnoise build failed" }

Write-Output "built $spxOut"
Write-Output "built $rnnOut"
