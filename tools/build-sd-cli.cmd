@echo off
setlocal
set "ROOT=%~dp0.."
set "VSDEV=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
call "%VSDEV%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
"%CMAKE%" -S "%ROOT%\third_party\ai\stable-diffusion.cpp" -B "%ROOT%\third_party\ai\stable-diffusion.cpp\build-cli" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSD_BUILD_EXAMPLES=ON -DSD_BUILD_SHARED_LIBS=ON -DSD_BUILD_SHARED_GGML_LIB=OFF -DSD_VULKAN=OFF -DSD_WEBP=ON -DSD_WEBM=OFF -DCMAKE_CXX_FLAGS=/bigobj
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%ROOT%\third_party\ai\stable-diffusion.cpp\build-cli" --config Release
exit /b %errorlevel%
