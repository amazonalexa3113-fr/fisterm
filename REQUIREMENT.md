# fisterm requirements

## operating system

- Linux
- POSIX-compatible environment

## development requirements

- C++20-compatible compiler
- CMake
- GNU Make
- Git

## compiler

One of the following:

- GCC with C++20 support
- Clang with C++20 support

## libraries and system components

- POSIX PTY support
- Linux terminal and process APIs

Additional libraries required by the project should be listed here as they are added to CMakeLists.txt.

## building dependencies

On Arch Linux:

```bash
sudo pacman -S --needed base-devel cmake gcc git
```

## build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

## notes

- a working C++20 compiler is required
- CMake is used to configure the project
- Runtime dependencies depend on the libraries linked by the build
