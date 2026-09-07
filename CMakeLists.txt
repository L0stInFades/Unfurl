cmake_minimum_required(VERSION 3.28)

project(Unfurl VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
option(UNFURL_BUILD_APP "Build the WinUI 3 application" ${WIN32})
option(UNFURL_BUILD_TESTS "Build archive core tests" ${BUILD_TESTING})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LibArchive REQUIRED)
if(TARGET LibArchive::LibArchive)
  set(UNFURL_LIBARCHIVE_TARGET LibArchive::LibArchive)
elseif(TARGET libarchive::libarchive)
  set(UNFURL_LIBARCHIVE_TARGET libarchive::libarchive)
else()
  message(FATAL_ERROR "The vcpkg libarchive port did not expose a known CMake target.")
endif()

add_library(unfurl_core STATIC src/core/archive_engine.cpp)
target_include_directories(unfurl_core PUBLIC include)
target_link_libraries(unfurl_core PRIVATE ${UNFURL_LIBARCHIVE_TARGET})
target_compile_features(unfurl_core PUBLIC cxx_std_23)
target_compile_definitions(unfurl_core PRIVATE _FILE_OFFSET_BITS=64)

if(MSVC)
  target_compile_options(unfurl_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(unfurl_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
endif()

if(UNFURL_BUILD_TESTS)
  enable_testing()
  add_executable(unfurl_core_tests tests/core_tests.cpp)
  target_link_libraries(unfurl_core_tests PRIVATE unfurl_core)
  target_compile_features(unfurl_core_tests PRIVATE cxx_std_23)
  if(MSVC)
    target_compile_options(unfurl_core_tests PRIVATE /utf-8 /EHsc)
  endif()
  add_test(NAME unfurl.core COMMAND unfurl_core_tests)
endif()

if(UNFURL_BUILD_APP)
  if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The WinUI 3 target requires the Microsoft C++ toolchain on Windows.")
  endif()
  set(WINDOWS_APP_SDK_ROOT "${CMAKE_SOURCE_DIR}/.cache/windowsappsdk/2.4.0" CACHE PATH "Extracted Windows App SDK 2.4.0 package")
  if(NOT EXISTS "${WINDOWS_APP_SDK_ROOT}/include/MddBootstrap.h")
    message(FATAL_ERROR "Windows App SDK 2.4.0 is missing. Run eng/restore-windows-app-sdk.ps1 first.")
  endif()
  add_executable(Unfurl WIN32
    src/app/main.cpp
    src/app/App.cpp
    src/app/MainPage.cpp
    src/app/App.h
    src/app/MainPage.h
    src/app/Unfurl.rc
  )
  set_source_files_properties(src/app/Unfurl.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/app/Unfurl.manifest;${CMAKE_SOURCE_DIR}/src/app/Unfurl.ico;${CMAKE_SOURCE_DIR}/src/app/MainPage.xaml")
  target_include_directories(Unfurl PRIVATE "${WINDOWS_APP_SDK_ROOT}/include")
  if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSdkVersion})
    target_include_directories(Unfurl PRIVATE "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSdkVersion}/cppwinrt")
    find_program(CPPWINRT_EXECUTABLE cppwinrt
      PATHS "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSdkVersion}/x64"
      NO_DEFAULT_PATH)
    if(NOT CPPWINRT_EXECUTABLE)
      message(FATAL_ERROR "cppwinrt.exe was not found in the selected Windows SDK.")
    endif()
    file(GLOB_RECURSE WINDOWS_APP_SDK_WINMDS
      "${WINDOWS_APP_SDK_ROOT}/metadata/*.winmd"
      "${WINDOWS_APP_SDK_ROOT}/lib/*.winmd")
    set(UNFURL_WINRT_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated-winrt")
    set(UNFURL_WINRT_INPUTS)
    foreach(WINMD IN LISTS WINDOWS_APP_SDK_WINMDS)
      list(APPEND UNFURL_WINRT_INPUTS -input "${WINMD}")
    endforeach()
    list(APPEND UNFURL_WINRT_INPUTS -input sdk+)
    execute_process(
      COMMAND "${CPPWINRT_EXECUTABLE}" ${UNFURL_WINRT_INPUTS} -output "${UNFURL_WINRT_OUTPUT}"
      RESULT_VARIABLE CPPWINRT_RESULT)
    if(NOT CPPWINRT_RESULT EQUAL 0)
      message(FATAL_ERROR "cppwinrt.exe failed while generating WinUI projections.")
    endif()
    target_include_directories(Unfurl PRIVATE "${UNFURL_WINRT_OUTPUT}")
  endif()
  target_link_directories(Unfurl PRIVATE "${WINDOWS_APP_SDK_ROOT}/lib/native/x64")
  target_link_libraries(Unfurl PRIVATE unfurl_core Microsoft.WindowsAppRuntime.Bootstrap windowsapp shell32 ole32)
  target_compile_definitions(Unfurl PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(Unfurl PRIVATE /W4 /permissive- /EHsc /await:strict /utf-8)
  # The application manifest is embedded by Unfurl.rc; disable linker's second copy.
  target_link_options(Unfurl PRIVATE /MANIFEST:NO)
  set_target_properties(Unfurl PROPERTIES OUTPUT_NAME "Unfurl")
endif()

install(TARGETS unfurl_core ARCHIVE DESTINATION lib)
if(TARGET Unfurl)
  install(TARGETS Unfurl RUNTIME DESTINATION .)
endif()
