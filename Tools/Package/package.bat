@echo off
rem Packages the game for Windows into Packaged\Windows; WEM_2026.exe there starts it. See Structures\BUILD.md.
rem Save and close the editor first: this builds the editor too (the cook runs on it), which Live Coding blocks.
rem Options after it go to BuildCookRun as well. The engine is expected at UE_ROOT, D:\Unreal engine\UE_5.8 unless set.
setlocal
if not defined UE_ROOT set "UE_ROOT=D:\Unreal engine\UE_5.8"
for %%I in ("%~dp0..\..") do set "PROJECT=%%~fI"
call "%UE_ROOT%\Engine\Build\BatchFiles\RunUAT.bat" BuildCookRun -project="%PROJECT%\WEM_2026.uproject" -platform=Win64 -clientconfig=Development -build -cook -stage -pak -archive -archivedirectory="%PROJECT%\Packaged" -utf8output -nop4 %*
if errorlevel 1 (
	echo Packaging failed; the first "Error" line above says why.
	exit /b 1
)
copy /y "%PROJECT%\Structures\BUILD.md" "%PROJECT%\Packaged\Windows\README.md" >nul
echo Done: %PROJECT%\Packaged\Windows\WEM_2026.exe
