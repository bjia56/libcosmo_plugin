# libcosmo_plugin

![GitHub License](https://img.shields.io/github/license/bjia56/libcosmo_plugin)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/) 

`libcosmo_plugin` is a C++ library for building platform-native plugins usable from Cosmopolitan Libc.

## Motivation

While Cosmopolitan Libc (and the Actually Portable Executable file format) allows creating portable binaries that can run on multiple OSes and architectures, sometimes it is necessary for a program to use platform-specific shared libraries or functions. Cosmopolitan's `cosmo_dlopen` function was introduced to allow calling platform-specific shared libraries, but is limited by only allowing one way communication and cannot expose host process symbols to the library. `libcosmo_plugin` aims to address this limitation by introducing a plugins architecture between host process and shared library, allowing for bidirectional communication between both parties.

## Building

`libcosmo_plugin` relies heavily on C++ templates, so it is recommended to build and link it with your application directly, instead of as a separate library.

The easiest way to build `libcosmo_plugin` is with CMake. Add this repository as a submodule to your project, then include it as a subdirectory in your `CMakeLists.txt`. The variables `LIBCOSMO_PLUGIN_SOURCES` and `LIBCOSMO_PLUGIN_INCLUDE_DIRS` will be populated with the source files and include directories, respectively, which can be added to your application.
