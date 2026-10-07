[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'The C++ developer environment is required. Use the Test Pending I/O Recovery VS Code task.'
}
$taskOutput = Join-Path $taskRepo 'build\tests'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
Push-Location $taskOutput
try {
    & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /D_WIN32_WINNT=0x0A00 "/I$taskRepo\src" `
        "$taskRepo\tests\PendingIoRecoveryTests.cpp" /Fe:PendingIoRecoveryTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Pending I/O recovery test compilation failed' }
    & .\PendingIoRecoveryTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Pending I/O recovery regression checks failed' }
} finally { Pop-Location }
