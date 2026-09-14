@echo off
rem Fresh closed-beta build: brand-new build dir, production flags.
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "JAVA_HOME=C:\Users\David\AppData\Local\Programs\Eclipse Adoptium\jdk-17.0.19.10-hotspot"
set "OPENSSL_ROOT_DIR=C:\Users\David\AppData\Local\Temp\opencode\vcpkg\installed\x64-windows-static"
rmdir /s /q cpp\build-beta 2>nul
"%CMAKE%" -S cpp -B cpp\build-beta -G Ninja ^
  -DBUILD_TESTING=ON -DAMALGAM_BUILD_LAUNCHER=ON -DAMALGAM_STRICT_HARDENING=ON ^
  -DCMAKE_BUILD_TYPE=Debug
if errorlevel 1 exit /b 1
"%CMAKE%" --build cpp\build-beta
exit /b %errorlevel%
