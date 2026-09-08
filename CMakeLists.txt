cmake_minimum_required(VERSION 3.28)

project(Unfurl VERSION 0.1.1.1 LANGUAGES CXX)

include(CTest)
option(UNFURL_BUILD_APP "Build the WinUI 3 application" ${WIN32})
option(UNFURL_BUILD_INSTALLER "Build the single-file WinUI 3 setup executable" OFF)
option(UNFURL_BUILD_TESTS "Build archive core tests" ${BUILD_TESTING})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No target uses C++ modules. Avoid an extra compiler scan for every translation unit.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(UNFURL_BUILD_APP OR UNFURL_BUILD_TESTS)
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
    target_compile_options(unfurl_core PRIVATE "$<$<CONFIG:Release>:/Gw;/Gy>")
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

endif()

if(UNFURL_BUILD_APP OR UNFURL_BUILD_INSTALLER)
  if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "The WinUI 3 target requires the Microsoft C++ toolchain on Windows.")
  endif()
  set(WINDOWS_APP_SDK_ROOT "${CMAKE_SOURCE_DIR}/.cache/windowsappsdk/2.4.0" CACHE PATH "Extracted Windows App SDK 2.4.0 package")
  if(NOT EXISTS "${WINDOWS_APP_SDK_ROOT}/include/MddBootstrap.h")
    message(FATAL_ERROR "Windows App SDK 2.4.0 is missing. Run eng/restore-windows-app-sdk.ps1 first.")
  endif()
  add_library(unfurl_winui INTERFACE)
  target_include_directories(unfurl_winui INTERFACE "${WINDOWS_APP_SDK_ROOT}/include")
  if(DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSdkVersion})
    target_include_directories(unfurl_winui INTERFACE "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSdkVersion}/cppwinrt")
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
    add_custom_command(OUTPUT "${UNFURL_WINRT_OUTPUT}/projection.stamp"
      COMMAND "${CPPWINRT_EXECUTABLE}" ${UNFURL_WINRT_INPUTS} -output "${UNFURL_WINRT_OUTPUT}"
      COMMAND "${CMAKE_COMMAND}" -E touch "${UNFURL_WINRT_OUTPUT}/projection.stamp"
      DEPENDS ${WINDOWS_APP_SDK_WINMDS} "${CPPWINRT_EXECUTABLE}"
      COMMENT "Generating Windows App SDK C++/WinRT projections"
      VERBATIM)
    add_custom_target(unfurl_winrt_projection DEPENDS "${UNFURL_WINRT_OUTPUT}/projection.stamp")
    add_dependencies(unfurl_winui unfurl_winrt_projection)
    target_include_directories(unfurl_winui INTERFACE "${UNFURL_WINRT_OUTPUT}")
  endif()
  target_link_directories(unfurl_winui INTERFACE "${WINDOWS_APP_SDK_ROOT}/lib/native/x64")
  target_link_libraries(unfurl_winui INTERFACE windowsapp shell32 ole32)
endif()

if(UNFURL_BUILD_APP)
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

  target_link_libraries(Unfurl PRIVATE unfurl_core unfurl_winui Microsoft.WindowsAppRuntime.Bootstrap)
  target_compile_definitions(Unfurl PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(Unfurl PRIVATE /W4 /permissive- /EHsc /await:strict /utf-8)
  target_precompile_headers(Unfurl PRIVATE src/app/pch.h)
  target_compile_options(Unfurl PRIVATE "$<$<CONFIG:Release>:/Gw;/Gy>")
  target_link_options(Unfurl PRIVATE "$<$<CONFIG:Release>:/OPT:REF;/OPT:ICF;/INCREMENTAL:NO>")
  # The application manifest is embedded by Unfurl.rc; disable linker's second copy.
  target_link_options(Unfurl PRIVATE /MANIFEST:NO)
  set_target_properties(Unfurl PROPERTIES OUTPUT_NAME "Unfurl")
endif()

if(UNFURL_BUILD_INSTALLER)
  set(UNFURL_SETUP_PAYLOAD_DIR "" CACHE PATH "Generated public setup resources from eng/package-setup.ps1")
  if(NOT EXISTS "${UNFURL_SETUP_PAYLOAD_DIR}/SetupPayload.h")
    message(FATAL_ERROR "Use eng/package-msix.ps1 to build setup with its signed release payload.")
  endif()
  add_executable(UnfurlSetup WIN32
    installer/UnfurlSetup.cpp
    installer/SetupDeployment.cpp
    installer/SetupWindow.cpp
    "${UNFURL_SETUP_PAYLOAD_DIR}/SetupPayload.rc"
  )
  set_source_files_properties("${UNFURL_SETUP_PAYLOAD_DIR}/SetupPayload.rc" PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/installer/UnfurlSetup.manifest;${CMAKE_SOURCE_DIR}/installer/UnfurlSetup.rc;${CMAKE_SOURCE_DIR}/installer/SetupWindow.xaml;${CMAKE_SOURCE_DIR}/src/app/Unfurl.ico;${UNFURL_SETUP_PAYLOAD_DIR}/Unfurl.cer;${UNFURL_SETUP_PAYLOAD_DIR}/Unfurl.appinstaller;${WINDOWS_APP_SDK_ROOT}/runtimes/win-x64/native/Microsoft.WindowsAppRuntime.Bootstrap.dll")
  target_include_directories(UnfurlSetup PRIVATE "${UNFURL_SETUP_PAYLOAD_DIR}" installer)
  target_compile_definitions(UnfurlSetup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_compile_options(UnfurlSetup PRIVATE /W4 /permissive- /EHsc /utf-8 /Gy /Gw /guard:cf)
  target_precompile_headers(UnfurlSetup PRIVATE src/app/pch.h)
  target_link_options(UnfurlSetup PRIVATE /MANIFEST:NO /OPT:REF /OPT:ICF /INCREMENTAL:NO /guard:cf)
  target_link_libraries(UnfurlSetup PRIVATE unfurl_winui crypt32 winhttp bcrypt wintrust comctl32 advapi32)
  set_target_properties(UnfurlSetup PROPERTIES OUTPUT_NAME "UnfurlSetup" MSVC_RUNTIME_LIBRARY "MultiThreaded")
endif()

if(TARGET unfurl_core)
  install(TARGETS unfurl_core ARCHIVE DESTINATION lib)
endif()
if(TARGET Unfurl)
  install(TARGETS Unfurl RUNTIME DESTINATION .)
endif()
if(TARGET UnfurlSetup)
  install(TARGETS UnfurlSetup RUNTIME DESTINATION .)
endif()
