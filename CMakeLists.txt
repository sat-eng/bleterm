cmake_minimum_required(VERSION 3.21)
project(bleterm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# SimpleBLE (Business Source License 1.1: free for non-commercial use).
include(FetchContent)
set(SIMPLEBLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    simpleble
    GIT_REPOSITORY https://github.com/simpleble/simpleble.git
    GIT_TAG v1.4.0
    GIT_SHALLOW YES
    SOURCE_SUBDIR simpleble
)
# SimpleBLE's own find modules (needed on Linux for DBus).
list(APPEND CMAKE_MODULE_PATH "${FETCHCONTENT_BASE_DIR}/simpleble-src/cmake/find")
FetchContent_MakeAvailable(simpleble)

add_executable(bleterm src/main.cpp)
target_link_libraries(bleterm PRIVATE simpleble::simpleble)

if(APPLE)
    # Compiled by the C++ compiler in Objective-C++ mode (avoids enabling the OBJCXX language).
    target_sources(bleterm PRIVATE src/bt_status_mac.mm)
    set_source_files_properties(src/bt_status_mac.mm PROPERTIES
        LANGUAGE CXX COMPILE_OPTIONS "-xobjective-c++;-fobjc-arc")
    target_link_libraries(bleterm PRIVATE "-framework CoreBluetooth")
else()
    target_sources(bleterm PRIVATE src/bt_status_other.cpp)
endif()

# libedit (built into macOS; libedit-dev on Linux) gives line editing and up/down history.
find_library(EDITLINE_LIB edit)
find_path(EDITLINE_INCLUDE editline/readline.h)
if(EDITLINE_LIB AND EDITLINE_INCLUDE)
    target_compile_definitions(bleterm PRIVATE HAVE_EDITLINE)
    target_include_directories(bleterm PRIVATE ${EDITLINE_INCLUDE})
    target_link_libraries(bleterm PRIVATE ${EDITLINE_LIB})
else()
    message(WARNING "libedit not found: bleterm will build without command history")
endif()

# By default macOS attributes bleterm's Bluetooth use to the app that launched it. iTerm2 and
# CLion declare Bluetooth for the programs they run, and their permission survives rebuilds.
# Apple's Terminal.app doesn't, and macOS kills Bluetooth users without a usage description,
# so for Terminal.app embed bleterm's own Info.plist instead. That identity is tied to each
# (ad-hoc signed) build, so the permission has to be granted again after every rebuild.
option(BLETERM_OWN_BLUETOOTH_IDENTITY "Embed Info.plist so bleterm asks for Bluetooth itself (needed in Terminal.app)" OFF)
if(APPLE AND BLETERM_OWN_BLUETOOTH_IDENTITY)
    target_link_options(bleterm PRIVATE
        "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
    set_property(TARGET bleterm APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
endif()
