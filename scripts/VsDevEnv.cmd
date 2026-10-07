@echo off
rem Activates the newest Visual Studio C++ developer environment found with vswhere.
rem Used as the shell prefix in .vscode\tasks.json:  cmd.exe /C scripts\VsDevEnv.cmd && <command>
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" echo VsDevEnv: vswhere.exe not found at "%VSWHERE%" 1>&2 & exit /b 1
set "VSINSTALL="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL echo VsDevEnv: no Visual Studio with the C++ x64 tools was found 1>&2 & exit /b 1
call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
exit /b %errorlevel%
