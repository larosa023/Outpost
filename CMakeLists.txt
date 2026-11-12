# Build Outpost on Windows with Visual Studio 2022 (Desktop C++ workload):
#   bash sync-version.sh   (or run it once from Git Bash; it lists the templates as resources)
#   cmake -S . -B build -A x64
#   cmake --build build --config Release
# Or cross-compile from Linux/macOS with ./build.sh (Zig).
cmake_minimum_required(VERSION 3.20)
project(Outpost LANGUAGES C CXX RC)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
file(GLOB OUTPOST_SOURCES CONFIGURE_DEPENDS src/*.cpp)
set(MINIZ src/miniz/miniz.c src/miniz/miniz_tdef.c src/miniz/miniz_tinfl.c src/miniz/miniz_zip.c)
add_executable(Outpost WIN32 ${OUTPOST_SOURCES} ${MINIZ} res/outpost.rc)
target_compile_definitions(Outpost PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 MINIZ_NO_TIME)
if(MSVC)
  target_compile_options(Outpost PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W4 /utf-8 /permissive->)
  target_link_options(Outpost PRIVATE /MANIFEST:NO /ENTRY:wWinMainCRTStartup)
endif()
target_link_libraries(Outpost PRIVATE d2d1 dwrite ole32 oleaut32 uuid dwmapi shcore shell32 user32 gdi32 advapi32 shlwapi
  windowscodecs comctl32 uxtheme winhttp bcrypt version comdlg32 ws2_32 iphlpapi psapi)
set_target_properties(Outpost PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
