@echo off
rem Start the OpenTTD 3D build from the right folder.
rem The game looks for its language and data files in the working directory,
rem which must be the build folder, not the folder that holds openttd.exe.
setlocal
set "BUILD=%~dp0build"
set "EXE="
for %%c in (RelWithDebInfo Release Debug) do if not defined EXE if exist "%BUILD%\%%c\openttd.exe" set "EXE=%BUILD%\%%c\openttd.exe"
if not defined EXE if exist "%BUILD%\openttd.exe" set "EXE=%BUILD%\openttd.exe"
if not defined EXE (
	echo No openttd.exe found under "%BUILD%". Build the game first, see docs\viewport3d.md.
	pause
	exit /b 1
)
if not exist "%BUILD%\lang\english.lng" (
	echo The language files are missing from "%BUILD%\lang". Build the "openttd" target again.
	pause
	exit /b 1
)
cd /d "%BUILD%"
start "" "%EXE%" %*
