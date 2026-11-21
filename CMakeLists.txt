cmake_minimum_required(VERSION 3.24)
project(TelegramFS VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "TelegramFS requires a Windows x64 target")
endif()

include(FetchContent)
set(JSON_BuildTests OFF CACHE BOOL "")
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG 55f93686c01528224f448c19128836e7df245f72)
FetchContent_Declare(tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG 30172438cee64926dc41fdd9c11fb3ba5b2ba9de)
FetchContent_MakeAvailable(nlohmann_json tomlplusplus)

find_path(WINFSP_INCLUDE_DIR winfsp/winfsp.h
    HINTS "$ENV{ProgramFiles}/WinFsp/inc" "C:/Program Files (x86)/WinFsp/inc"
    REQUIRED)

add_executable(telegramfs
    src/common.cpp src/crypto.cpp src/telegram.cpp src/legacy.cpp
    src/store.cpp src/filesystem.cpp src/main.cpp)
target_compile_features(telegramfs PRIVATE cxx_std_20)
target_include_directories(telegramfs SYSTEM PRIVATE "${WINFSP_INCLUDE_DIR}")
target_compile_definitions(telegramfs PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN
    NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_link_libraries(telegramfs PRIVATE nlohmann_json::nlohmann_json
    tomlplusplus::tomlplusplus bcrypt crypt32 advapi32 shell32 ole32 uuid)
if(MSVC)
    set_property(TARGET telegramfs PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options(telegramfs PRIVATE /W4 /permissive- /utf-8 /EHsc
        $<$<CONFIG:Release>:/O2>)
else()
    target_compile_options(telegramfs PRIVATE -municode -Wall -Wextra)
    target_link_options(telegramfs PRIVATE -municode -static)
endif()

install(TARGETS telegramfs RUNTIME DESTINATION .)
install(FILES telegramfs.toml LICENSE NOTICE DESTINATION .)
install(DIRECTORY licenses/ DESTINATION licenses)
option(TELEGRAMFS_PACKAGE_RUNTIME "Bundle the pinned Windows x64 TDLib runtime" OFF)
set(TELEGRAMFS_TDLIB_BIN "" CACHE PATH "Directory containing TDLib and its runtime DLLs")
if(TELEGRAMFS_PACKAGE_RUNTIME AND NOT TELEGRAMFS_TDLIB_BIN)
    FetchContent_Declare(tdlib_runtime
        URL https://api.nuget.org/v3-flatcontainer/tdlib.native.win-x64/1.8.70/tdlib.native.win-x64.1.8.70.nupkg
        URL_HASH SHA256=6cb1986ef52e01df9a150923e266ee53ba9cb3514f00c95dc595234f3dee144f
        DOWNLOAD_NAME tdlib-runtime.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(tdlib_runtime)
    set(TELEGRAMFS_TDLIB_BIN "${tdlib_runtime_SOURCE_DIR}/runtimes/win-x64/native")
endif()
if(TELEGRAMFS_TDLIB_BIN)
    if(NOT EXISTS "${TELEGRAMFS_TDLIB_BIN}/tdjson.dll")
        message(FATAL_ERROR "TELEGRAMFS_TDLIB_BIN must contain tdjson.dll")
    endif()
    install(DIRECTORY "${TELEGRAMFS_TDLIB_BIN}/" DESTINATION . FILES_MATCHING PATTERN "*.dll")
endif()
