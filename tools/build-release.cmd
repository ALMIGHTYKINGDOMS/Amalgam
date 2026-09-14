@echo off
rem Release build with Visual Studio for production-quality executable
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "JAVA_HOME=C:\Users\David\AppData\Local\Programs\Eclipse Adoptium\jdk-17.0.19.10-hotspot"
set "OPENSSL_ROOT_DIR=C:\Users\David\vcpkg\installed\x64-windows-static"
rem Update-manifest verification public key. Private key stays in the local
rem release vault and must never enter the repository or the package.
set "UPDATER_KEY=C:\Users\David\.amalgam-release\update-signing-public.pem"
set "UPDATER_KEY_ARG="
if exist "%UPDATER_KEY%" set "UPDATER_KEY_ARG=-DAMALGAM_UPDATER_PUBLIC_KEY_FILE="%UPDATER_KEY%""
if exist cpp\build-release rmdir /s /q cpp\build-release
"%CMAKE%" -S cpp -B cpp\build-release -G Ninja ^
  -DBUILD_TESTING=ON -DAMALGAM_BUILD_LAUNCHER=ON -DAMALGAM_STRICT_HARDENING=ON ^
  %UPDATER_KEY_ARG% ^
  -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
"%CMAKE%" --build cpp\build-release
if errorlevel 1 exit /b 1
rem Stage the supported Bedrock client package beside the launcher when it has
rem been validated by the Bedrock packaging tool. Prefer the stable copy, then
rem fall back to the versioned package produced by the packager.
if exist bedrock\AmalgamBedrockClient\dist\AmalgamBedrockClient.mcaddon (
  if not exist cpp\build-release\bedrock mkdir cpp\build-release\bedrock
  copy /y bedrock\AmalgamBedrockClient\dist\AmalgamBedrockClient.mcaddon cpp\build-release\bedrock\AmalgamBedrockClient.mcaddon >nul
  copy /y bedrock\AmalgamBedrockClient\dist\package-inventory.json cpp\build-release\bedrock\package-inventory.json >nul
  copy /y bedrock\AmalgamBedrockClient\dist\package-sha256.txt cpp\build-release\bedrock\package-sha256.txt >nul
) else (
  for %%F in (bedrock\AmalgamBedrockClient\dist\AmalgamBedrockClient-*.mcaddon) do (
    if exist "%%~fF" (
      if not exist cpp\build-release\bedrock mkdir cpp\build-release\bedrock
      copy /y "%%~fF" cpp\build-release\bedrock\AmalgamBedrockClient.mcaddon >nul
      copy /y bedrock\AmalgamBedrockClient\dist\package-inventory.json cpp\build-release\bedrock\package-inventory.json >nul
      copy /y bedrock\AmalgamBedrockClient\dist\package-sha256.txt cpp\build-release\bedrock\package-sha256.txt >nul
      goto :bedrock_staged
    )
  )
)
:bedrock_staged
rem CMake does not build Java bridges. Stage the validated bridge set explicitly.
set "BRIDGE_SOURCE=cpp\build-verify\bridges"
if not exist "%BRIDGE_SOURCE%\amalgam-fabric-1.21.11.jar" (
  echo Validated Java bridge set is missing: %BRIDGE_SOURCE%
  exit /b 1
)
if not exist cpp\build-release\bridges mkdir cpp\build-release\bridges
copy /y "%BRIDGE_SOURCE%\*.jar" cpp\build-release\bridges\ >nul
if errorlevel 1 exit /b 1
exit /b 0
