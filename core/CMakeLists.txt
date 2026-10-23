cmake_minimum_required(VERSION 3.16)
project(ncx_core VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static CRT on MSVC: the DLL loads into SolidWorks/Fusion with no redistributable to install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(miniz STATIC third_party/miniz/miniz.c)
target_include_directories(miniz PUBLIC third_party/miniz)
set_target_properties(miniz PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(miniz PRIVATE /W0)
endif()

# One set of objects, linked into the DLL and (whole) into the self-test, so the test can
# reach internal functions the DLL doesn't export.
add_library(ncx_obj OBJECT
  src/api.cpp
  src/codec.cpp
  src/container.cpp
  src/fingerprint.cpp
  src/matcher.cpp
  src/notice.cpp
  src/step21.cpp
  src/validate.cpp
  src/verify.cpp)
target_include_directories(ncx_obj PUBLIC include src third_party third_party/miniz)
target_compile_definitions(ncx_obj PUBLIC NCX_CORE_BUILD)
set_target_properties(ncx_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
  target_compile_options(ncx_obj PRIVATE /W4 /permissive- /utf-8 /EHsc)
else()
  target_compile_options(ncx_obj PRIVATE -Wall -Wextra -fvisibility=hidden)
endif()

add_library(ncx_core SHARED $<TARGET_OBJECTS:ncx_obj>)
target_include_directories(ncx_core PUBLIC include)
target_link_libraries(ncx_core PRIVATE miniz)

# Small C++ unit test executable (no framework): exercised by ctest and by build.ps1.
add_executable(ncx_core_selftest tests/selftest.cpp $<TARGET_OBJECTS:ncx_obj>)
target_link_libraries(ncx_core_selftest PRIVATE miniz)
target_include_directories(ncx_core_selftest PRIVATE include src third_party third_party/miniz)
target_compile_definitions(ncx_core_selftest PRIVATE NCX_CORE_BUILD)
if(MSVC)
  target_compile_options(ncx_core_selftest PRIVATE /utf-8 /EHsc)
endif()
enable_testing()
add_test(NAME selftest COMMAND ncx_core_selftest)
