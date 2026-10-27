# NCX add-in for Autodesk Fusion (C++).
#
# Three DLLs end up in the add-in folder:
#   NCX.dll            the add-in Fusion loads (run/stop): commands, menu entries, the developer job watcher,
#                      and a loader for the implementation DLL (so a rebuilt implementation can be reloaded
#                      without restarting Fusion).
#   NcxFusionImpl.dll  everything else: import (replay), verification, export.
#   ncx_core.dll       the shared NCX core (built from ../core, unchanged).
#
# Configure/build with the VS 2019 Build Tools' CMake (see build.ps1). The Fusion SDK headers and import
# libraries are taken from FUSION_SDK (default: %APPDATA%\Autodesk\Autodesk Fusion 360\API\CPP, which Fusion
# keeps current with every update).
cmake_minimum_required(VERSION 3.16)
project(ncx_fusion VERSION 0.2.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# the add-in's version: this one place. The DLLs get it as NCX_FUSION_VERSION; Fusion's manifest is written from
# NCX/NCX.manifest.in into the build folder, and the installer takes its version from that manifest.
add_compile_definitions(NCX_FUSION_VERSION="${PROJECT_VERSION}")
configure_file(NCX/NCX.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/NCX.manifest @ONLY NEWLINE_STYLE CRLF)

if(NOT FUSION_SDK)
  file(TO_CMAKE_PATH "$ENV{APPDATA}/Autodesk/Autodesk Fusion 360/API/CPP" FUSION_SDK)
endif()
if(NOT EXISTS "${FUSION_SDK}/include/Core/CoreAll.h")
  message(FATAL_ERROR "Fusion C++ SDK not found at ${FUSION_SDK} (set -DFUSION_SDK=...)")
endif()
message(STATUS "Fusion SDK: ${FUSION_SDK}")

# the shared core, built unchanged from the repository
add_subdirectory(../core core)

set(NCX_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../core/third_party)
set(FUSION_LIBS ${FUSION_SDK}/lib/core.lib ${FUSION_SDK}/lib/fusion.lib)

# --- the add-in (stub) --------------------------------------------------------------------------------
add_library(NCX SHARED src/addin/NCX.cpp)
target_include_directories(NCX PRIVATE ${FUSION_SDK}/include ${NCX_THIRD_PARTY} src)
target_link_libraries(NCX PRIVATE ${FUSION_LIBS})
target_compile_options(NCX PRIVATE /utf-8 /EHsc /W3 /bigobj)
target_compile_definitions(NCX PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)

# --- the implementation ------------------------------------------------------------------------------
file(GLOB IMPL_SOURCES CONFIGURE_DEPENDS src/impl/*.cpp)
add_library(NcxFusionImpl SHARED ${IMPL_SOURCES})
target_include_directories(NcxFusionImpl PRIVATE ${FUSION_SDK}/include ${NCX_THIRD_PARTY} src ../core/include)
target_link_libraries(NcxFusionImpl PRIVATE ${FUSION_LIBS} ncx_core)
# /EHa: a structured exception (access violation) inside the implementation is caught at the job boundary
# instead of taking Fusion down with it
target_compile_options(NcxFusionImpl PRIVATE /utf-8 /EHa /W3 /bigobj)
target_compile_definitions(NcxFusionImpl PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
