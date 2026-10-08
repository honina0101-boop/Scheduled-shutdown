param([string]$Python = 'python')
$ErrorActionPreference = 'Stop'
$taskRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location -LiteralPath $taskRoot
try {
    $taskBin = Join-Path $taskRoot '.tools\w64devkit\bin'
    if (-not (Test-Path -LiteralPath (Join-Path $taskBin 'g++.exe'))) {
        & $Python 'scripts/bootstrap.py'
        if ($LASTEXITCODE -ne 0) { throw 'Tool preparation failed.' }
    }
    $env:PATH = $taskBin + ';' + $env:PATH
    & (Join-Path $taskBin 'cmake.exe') -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
    & (Join-Path $taskBin 'cmake.exe') --build build -j 2
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
    & (Join-Path $taskBin 'ctest.exe') --test-dir build --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    & $Python 'scripts/release.py'
    if ($LASTEXITCODE -ne 0) { throw 'Packaging failed.' }
} finally { Pop-Location }
