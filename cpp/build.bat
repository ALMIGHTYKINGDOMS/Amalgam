@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1
cd /d "C:\Users\David\Documents\Default Project\cpp\build"
cmake --build . --config Release 2>&1