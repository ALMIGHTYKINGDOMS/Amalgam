@echo off
set "JAVA_HOME=C:\Users\David\AppData\Local\Programs\Eclipse Adoptium\jdk-17.0.19.10-hotspot"
set "OPENSSL_ROOT_DIR=C:\Users\David\vcpkg\installed\x64-windows-static"
set "PATH=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul 2>&1
if errorlevel 1 (
    echo VSCMD FAILED
    exit /b 1
)
set "CMAKE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"

echo === STEP 1: CMAKE CONFIGURE ===
cd /d "C:\Users\David\Documents\Default Project"
rmdir /s /q cpp\build-vs 2>nul
mkdir cpp\build-vs
"%CMAKE%" -S cpp -B cpp\build-vs -G Ninja -DBUILD_TESTING=ON -DAMALGAM_BUILD_LAUNCHER=ON -DAMALGAM_STRICT_HARDENING=ON -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 (
    echo CMAKE CONFIGURE FAILED
    exit /b 1
)

echo.
echo === STEP 2: BUILD ===
"%CMAKE%" --build cpp\build-vs --config Release
if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)

echo.
echo === STEP 3: TEST ===
cd /d "C:\Users\David\Documents\Default Project\cpp\build-vs"
ctest --output-on-failure -C Release
if errorlevel 1 (
    echo SOME TESTS FAILED
    exit /b 1
)

echo.
echo === BUILD + TEST COMPLETE ===
