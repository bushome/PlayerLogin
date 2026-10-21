cmake_minimum_required(VERSION 3.24)

# Configure with the vcpkg toolchain and the static-CRT-free static triplet, e.g.:
#   cmake -B build -G "Visual Studio 17 2022" -A x64 ^
#     -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
#     -DVCPKG_TARGET_TRIPLET=x64-windows-static-md ^
#     -DASAAPI_DIR=C:/src/AsaApi -DASAAPI_LIB=C:/src/AsaApi/x64/Release/AsaApi.lib
#   cmake --build build --config Release
#
# x64-windows-static-md links libmariadb statically into PlayerLogin.dll while sharing the
# dynamic VC++ runtime with the server, so the plugin folder is just the DLL + two JSON files.

project(PlayerLogin LANGUAGES CXX C)

set(ASAAPI_DIR "" CACHE PATH "Path to the AsaApi repository root")
set(ASAAPI_LIB "" CACHE FILEPATH "Path to AsaApi.lib (Release build of AsaApi)")
if(NOT ASAAPI_DIR OR NOT ASAAPI_LIB)
  message(FATAL_ERROR "Set -DASAAPI_DIR=<AsaApi repo> and -DASAAPI_LIB=<AsaApi.lib>")
endif()

find_package(unofficial-libmariadb CONFIG REQUIRED)

add_library(PlayerLogin SHARED
  Source/PlayerLogin.cpp
  Source/Database.h
)

set_target_properties(PlayerLogin PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
  MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
)

target_include_directories(PlayerLogin PRIVATE
  "${ASAAPI_DIR}/AsaApi/Core/Public"
  "${ASAAPI_DIR}/AsaApi/Core/Public/API/UE"
)

target_compile_definitions(PlayerLogin PRIVATE NDEBUG _WINDOWS _USRDLL WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(PlayerLogin PRIVATE /utf-8 /permissive- /Zc:__cplusplus /bigobj)

target_link_directories(PlayerLogin PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(ASAAPI_LIB_DIR "${ASAAPI_LIB}" DIRECTORY)
target_link_directories(PlayerLogin PRIVATE "${ASAAPI_LIB_DIR}")
target_link_libraries(PlayerLogin PRIVATE "${ASAAPI_LIB}" unofficial::libmariadb)

# Copy the plugin folder contents next to the DLL after each build.
add_custom_command(TARGET PlayerLogin POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_LIST_DIR}/Plugin/PluginInfo.json"
    "${CMAKE_CURRENT_LIST_DIR}/Plugin/config.json"
    "$<TARGET_FILE_DIR:PlayerLogin>"
)
