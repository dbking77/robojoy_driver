# Linux RoboDrive link: BlueZ driver library, connectivity CLI, and SDL UI.
#   sudo apt install libglib2.0-dev libsdl2-dev libsdl2-ttf-dev
#   cmake -S . -B build && cmake --build build
#   ./build/robojoy_cli --name ROBODRIVE_2025
#   ./build/robojoy_gui --name ROBODRIVE_2025
#
# Point ROBODRIVE_COMM_DIR at robojoy_fw/main later to share the protocol
# sources instead of the verbatim copy in protocol/.

cmake_minimum_required(VERSION 3.16)
project(robojoy_driver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET gio-2.0)

set(ROBODRIVE_COMM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/protocol"
    CACHE PATH "Directory containing unmodified robodrive_comm.c and robodrive_comm.h")

add_library(robojoy STATIC
    src/ble_client.c
    src/robojoy_session.c
    ${ROBODRIVE_COMM_DIR}/robodrive_comm.c
)

target_include_directories(robojoy
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${ROBODRIVE_COMM_DIR}
)

target_link_libraries(robojoy PUBLIC PkgConfig::GLIB)
target_compile_options(robojoy PRIVATE -Wall -Wextra)

add_executable(robojoy_cli src/cli/main.c)
target_link_libraries(robojoy_cli PRIVATE robojoy)
target_compile_options(robojoy_cli PRIVATE -Wall -Wextra)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF SDL2_ttf)
find_library(SDL2_TTF_LIBRARY NAMES SDL2_ttf)
if(NOT SDL2_TTF_LIBRARY AND EXISTS /usr/lib/x86_64-linux-gnu/libSDL2_ttf-2.0.so.0)
    set(SDL2_TTF_LIBRARY /usr/lib/x86_64-linux-gnu/libSDL2_ttf-2.0.so.0)
endif()
if(NOT SDL2_TTF_LIBRARY)
    message(FATAL_ERROR "SDL2_ttf not found. Install libsdl2-ttf-dev or libsdl2-ttf-2.0-0")
endif()

add_executable(robojoy_gui src/gui/main.c)
target_link_libraries(robojoy_gui PRIVATE robojoy PkgConfig::SDL2 ${SDL2_TTF_LIBRARY})
if(SDL2_TTF_FOUND)
    target_include_directories(robojoy_gui PRIVATE ${SDL2_TTF_INCLUDE_DIRS})
    target_compile_definitions(robojoy_gui PRIVATE ROBOJOY_HAVE_SDL_TTF)
endif()
target_compile_options(robojoy_gui PRIVATE -Wall -Wextra)
