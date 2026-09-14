@echo off
set "PATH=C:\Program Files (x86)\Microsoft Visual Studio\Installer;%PATH%"
set "JAVA_HOME=C:\Users\David\AppData\Local\Programs\Eclipse Adoptium\jdk-17.0.19.10-hotspot"
set "OPENSSL_ROOT_DIR=C:\Users\David\AppData\Local\Temp\opencode\vcpkg\installed\x64-windows-static"
set "OPENSSL_USE_STATIC_LIBS=TRUE"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
"%CMAKE%" -S cpp -B cpp\build-quick -G Ninja -DBUILD_TESTING=ON -DAMALGAM_STRICT_HARDENING=ON
if errorlevel 1 exit /b 1
"%CMAKE%" --build cpp\build-quick --target amalgam_updater_test amalgam_json_atomic_test
exit /b %errorlevel%
