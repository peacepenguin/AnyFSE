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
    # Vendored Zydis amalgamation: compiled on its own without project warnings, as in the vcxproj files.
    & cl.exe /nologo /c /O2 /W0 /DZYDIS_STATIC_BUILD /DZYCORE_STATIC_BUILD "/I$taskRepo\src\Tools\zydis" `
        "$taskRepo\src\Tools\zydis\Zydis.c" /Fo:Zydis.obj
    if ($LASTEXITCODE -ne 0) { throw 'Zydis compilation failed' }
    & cl.exe /nologo /std:c++17 /EHsc /W4 /WX /DZYDIS_STATIC_BUILD /DZYCORE_STATIC_BUILD "/I$taskRepo\src" "$taskRepo\tests\XboxStartupPlanTests.cpp" `
        "$taskRepo\src\Tools\XboxStartupPlan.cpp" Zydis.obj /Fe:XboxStartupPlanTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Patch-plan test compilation failed' }
    & .\XboxStartupPlanTests.exe
    if ($LASTEXITCODE -ne 0) { throw 'Patch-plan regression checks failed' }
} finally { Pop-Location }
