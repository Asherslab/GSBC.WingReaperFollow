cmake_minimum_required(VERSION 3.20)

project(WingReaperFollow VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # no VC++ redist needed

# Release builds pass the tag here (-DWF_VERSION=1.2.3); local builds use the project version.
set(WF_VERSION "${PROJECT_VERSION}-dev" CACHE STRING "Version string shown in the extension")

option(WF_BUILD_EXTENSION "Build the REAPER extension (needs the REAPER SDK + WDL)" ON)
option(WF_BUILD_TESTS "Build unit tests" ON)

# ---- core (no REAPER dependency) ----------------------------------------------------------------
add_library(wingcore STATIC
  src/wing/osc.cpp
  src/wing/udp.cpp
  src/wing/wing_client.cpp
  src/wing/wing_model.cpp)
target_include_directories(wingcore PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(wingcore PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(wingcore PUBLIC ws2_32)
endif()
if(MSVC)
  target_compile_options(wingcore PRIVATE /W4)
else()
  target_compile_options(wingcore PRIVATE -Wall -Wextra)
endif()

if(WF_BUILD_TESTS)
  enable_testing()
  add_executable(test_core tests/test_core.cpp)
  target_link_libraries(test_core PRIVATE wingcore)
  add_test(NAME test_core COMMAND test_core)
  add_executable(test_client tests/test_client.cpp)
  target_link_libraries(test_client PRIVATE wingcore)
  add_test(NAME test_client COMMAND test_client)
endif()

if(NOT WF_BUILD_EXTENSION)
  return()
endif()

# ---- dependencies (pinned) ----------------------------------------------------------------------
include(FetchContent)
set(WF_REAPER_SDK_COMMIT a98b8bf959f99702fe4aeae045b7434c76188683)
set(WF_WDL_COMMIT 0c2af4f68b052ce974cb53dc510f88978dc99dbd)
FetchContent_Declare(reaper_sdk
  URL https://github.com/justinfrankel/reaper-sdk/archive/${WF_REAPER_SDK_COMMIT}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_Declare(wdl
  URL https://github.com/justinfrankel/WDL/archive/${WF_WDL_COMMIT}.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(reaper_sdk wdl)
set(WDL_ROOT ${wdl_SOURCE_DIR})

# ---- the mixer-strip JSFX, embedded so the extension can install it -----------------------------
set(WF_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
add_custom_command(
  OUTPUT ${WF_GEN_DIR}/wing_follow_jsfx.cpp
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/src/jsfx/wing_follow.jsfx
          -DOUT=${WF_GEN_DIR}/wing_follow_jsfx.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS src/jsfx/wing_follow.jsfx cmake/embed_file.cmake
  COMMENT "Embedding wing_follow.jsfx")

# ---- extension ----------------------------------------------------------------------------------
set(EXT_SOURCES
  src/ext/engine.cpp
  src/ext/main.cpp
  src/ext/panel.cpp
  src/ext/reaper_api.cpp
  ${WF_GEN_DIR}/wing_follow_jsfx.cpp
  ${WDL_ROOT}/WDL/wingui/wndsize.cpp)

if(WIN32)
  enable_language(RC)
  list(APPEND EXT_SOURCES src/ext/wingfollow.rc ${WDL_ROOT}/WDL/win32_utf8.c)
elseif(APPLE)
  enable_language(OBJCXX)
  # Convert the Win32 dialog resource into SWELL's form (perl ships with macOS).
  add_custom_command(
    OUTPUT ${WF_GEN_DIR}/wingfollow.rc_mac_dlg ${WF_GEN_DIR}/wingfollow.rc_mac_menu
    COMMAND ${CMAKE_COMMAND} -E make_directory ${WF_GEN_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/src/ext/wingfollow.rc ${WF_GEN_DIR}/wingfollow.rc
    COMMAND perl ${WDL_ROOT}/WDL/swell/swell_resgen.pl ${WF_GEN_DIR}/wingfollow.rc
    DEPENDS src/ext/wingfollow.rc
    COMMENT "Generating SWELL dialog resources")
  list(APPEND EXT_SOURCES ${WDL_ROOT}/WDL/swell/swell-modstub.mm ${WF_GEN_DIR}/wingfollow.rc_mac_dlg)
else()
  list(APPEND EXT_SOURCES ${WDL_ROOT}/WDL/swell/swell-modstub-generic.cpp)
endif()

add_library(reaper_wingfollow MODULE ${EXT_SOURCES})
set_target_properties(reaper_wingfollow PROPERTIES PREFIX "")
if(APPLE)
  set_target_properties(reaper_wingfollow PROPERTIES SUFFIX ".dylib")
endif()
target_include_directories(reaper_wingfollow PRIVATE
  ${reaper_sdk_SOURCE_DIR}/sdk
  ${WDL_ROOT}        # "WDL/wingui/wndsize.h"
  ${WDL_ROOT}/WDL    # "swell/swell.h", and reaper_plugin.h's "../WDL/swell/swell.h"
  ${WF_GEN_DIR}      # generated SWELL resources
  src/ext)
target_link_libraries(reaper_wingfollow PRIVATE wingcore)
target_compile_definitions(reaper_wingfollow PRIVATE WF_VERSION="${WF_VERSION}")

if(WIN32)
  target_compile_definitions(reaper_wingfollow PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(MINGW)
    # WDL casts FARPROC to void*, which MSVC accepts and GCC only with -fpermissive.
    set_source_files_properties(${WDL_ROOT}/WDL/wingui/wndsize.cpp PROPERTIES COMPILE_OPTIONS -fpermissive)
    target_link_options(reaper_wingfollow PRIVATE -static -static-libgcc -static-libstdc++)
  endif()
elseif(APPLE)
  target_compile_definitions(reaper_wingfollow PRIVATE SWELL_PROVIDED_BY_APP)
  target_link_libraries(reaper_wingfollow PRIVATE "-framework AppKit")
else()
  target_compile_definitions(reaper_wingfollow PRIVATE SWELL_PROVIDED_BY_APP)
endif()

# Copy into REAPER's UserPlugins folder:  cmake --build build --target install_to_reaper
if(WIN32)
  set(WF_USERPLUGINS "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(APPLE)
  set(WF_USERPLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
else()
  set(WF_USERPLUGINS "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()
add_custom_target(install_to_reaper
  COMMAND ${CMAKE_COMMAND} -E make_directory "${WF_USERPLUGINS}"
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:reaper_wingfollow> "${WF_USERPLUGINS}/"
  DEPENDS reaper_wingfollow
  COMMENT "Copying to ${WF_USERPLUGINS}")
