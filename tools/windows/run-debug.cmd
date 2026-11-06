@echo off
rem Starts the debug build from Explorer. It needs MSYS2's Qt and other DLLs, which aren't copied
rem beside the program until there's a proper Windows package (M12).
set "PATH=C:\msys64\ucrt64\bin;%PATH%"
start "" "%~dp0..\..\build\windows-debug\papertrail.exe"
