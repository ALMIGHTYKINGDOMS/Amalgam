@echo off
rem Visual Studio Release build for local beta testing
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "JAVA_HOME=C:\Users\David\AppData\Local\Programs\Eclipse Adoptium\jdk-17.0.19.10-hotspot"
set "OPENSSL_ROOT_DIR=C:\Users\David\vcpkg\installed\x64-windows-static"
rmdir /s /q cpp\build-vs 2>nul
"%CMAKE%" -S cpp -B cpp\build-vs -G Ninja ^
  -DBUILD_TESTING=ON -DAMALGAM_BUILD_LAUNCHER=ON -DAMALGAM_STRICT_HARDENING=ON ^
  -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE%" --build cpp\build-vs --config Release
if errorlevel 1 exit /b 1
rem Stage the validated Bedrock client package and its matching release metadata.
if exist bedrock\AmalgamBedrockClient\dist\AmalgamBedrockClient.mcaddon (
  if not exist cpp\build-vs\bedrock mkdir cpp\build-vs\bedrock
  copy /y bedrock\AmalgamBedrockClient\dist\AmalgamBedrockClient.mcaddon cpp\build-vs\bedrock\AmalgamBedrockClient.mcaddon >nul
  copy /y bedrock\AmalgamBedrockClient\dist\package-inventory.json cpp\build-vs\bedrock\package-inventory.json >nul
  copy /y bedrock\AmalgamBedrockClient\dist\package-sha256.txt cpp\build-vs\bedrock\package-sha256.txt >nul
) else (
  echo Validated Bedrock package is missing.
  exit /b 1
)
rem CMake does not build Java bridges. Stage the previously validated bridge
rem set explicitly and fail rather than producing a partial Java release.
set "BRIDGE_SOURCE=cpp\build-verify\bridges"
if not exist "%BRIDGE_SOURCE%\amalgam-fabric-1.21.11.jar" (
  echo Validated Java bridge set is missing: %BRIDGE_SOURCE%
  exit /b 1
)
if not exist cpp\build-vs\bridges mkdir cpp\build-vs\bridges
copy /y "%BRIDGE_SOURCE%\*.jar" cpp\build-vs\bridges\ >nul
if errorlevel 1 exit /b 1
exit /b 0
