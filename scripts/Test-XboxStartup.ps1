[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw 'Run this test script from an x64 Visual Studio Developer PowerShell with the C++ workload installed.'
}
$taskOutput = Join-Path $taskRepo 'build\tests'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
Push-Location $taskOutput
try {
    & cl.exe /nologo /std:c++17 /EHsc /W4 /WX "/I$taskRepo\src" "$taskRepo\tests\XboxStartupPlanTests.cpp" `
        "$taskRepo\src\Tools\XboxStartupPlan.cpp" /Fe:XboxStartupPlanTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Patch-plan test compilation failed' }
    & .\XboxStartupPlanTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Patch-plan regression checks failed' }
} finally { Pop-Location }
