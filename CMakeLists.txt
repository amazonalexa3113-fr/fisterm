cmake_minimum_required(VERSION 3.20)

project(fistermpromax
    VERSION 0.3
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(SDL2_TTF REQUIRED IMPORTED_TARGET SDL2_ttf)
pkg_check_modules(VTERM REQUIRED IMPORTED_TARGET vterm)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)

add_executable(fistermpromax
    src/main.cpp
    src/Renderer.cpp
    src/Terminal.cpp
    src/PTY.cpp
)

target_link_libraries(fistermpromax PRIVATE
    PkgConfig::SDL2
    PkgConfig::SDL2_TTF
    PkgConfig::VTERM
    PkgConfig::FONTCONFIG
)

if(UNIX AND NOT APPLE)
    target_link_libraries(fistermpromax PRIVATE util)
endif()

install(TARGETS fistermpromax
    RUNTIME DESTINATION bin
)
