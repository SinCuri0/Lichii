@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if "%1"=="configure" (
  cmake -S "D:\Games\Emulators\lichii repo" -B D:\lxc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/clang-cl.exe -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/clang-cl.exe -DYUZU_TESTS=OFF -DENABLE_WERROR=OFF -DENABLE_UPDATE_CHECKER=OFF -DENABLE_WEB_SERVICE=OFF -DYUZU_ROOM=OFF
  exit /b %errorlevel%
)
cmake --build D:\lxc --parallel
set rc=%errorlevel%
echo BUILD DONE exit %rc%
exit /b %rc%
