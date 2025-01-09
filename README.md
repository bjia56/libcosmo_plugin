# libcosmo_plugin

![GitHub License](https://img.shields.io/github/license/bjia56/libcosmo_plugin)
[![Generic badge](https://img.shields.io/badge/C++-20-blue.svg)](https://shields.io/) 

`libcosmo_plugin` is a C++ library for building platform-native plugins usable from Cosmopolitan Libc.

## Motivation

While Cosmopolitan Libc (and the Actually Portable Executable file format) allows creating portable binaries that can run on multiple OSes and architectures, sometimes it is necessary for a program to use platform-specific shared libraries or functions. Cosmopolitan's `cosmo_dlopen` function was introduced to allow calling platform-specific shared libraries, but is limited by only allowing one way communication and cannot expose host process symbols to the library. `libcosmo_plugin` aims to address this limitation by introducing a plugin architecture between host process and native plugin, allowing for bidirectional communication between both parties.

## Usage

Using `libcosmo_plugin` takes different forms depending on whether the library is being built by the Cosmopolitan Libc toolchain (`cosmoc++` and its variants) or by a stock C++ compiler.

With Cosmopolitan, `libcosmo_plugin` is in "Plugin Host" mode and exposes the `PluginHost` class, which is used to load a plugin shared library (or executable; see [below](#Building)) and export host process functions that will be exposed to the plugin. This class can also be used to call functions exported by the plugin.

```c++
std::string pluginPath = ...; // path to the plugin shared library or executable
PluginHost plugin(pluginPath);

// configure handlers callable by the plugin
plugin.registerHandler("add", std::function([](int a, int b) -> int {
    return a + b;
}));

// launch the plugin
plugin.initialize();

// call functions exposed by the plugin
int result = plugin.call<int>("subtract", 10, 5);
```

With a stock compiler, `libcosmo_plugin` is in "Plugin" mode and exposes the `Plugin` class, which is used to export shared library functions that will be exposed to the host. This class can also be used to call functions exported by the host. Plugin mode also exports certain shared library symbols, which will be used to bootstrap the bidirectional plugin connection.

Plugins must implement the `void plugin_initializer(Plugin* plugin)` function, which is called by `libcosmo_plugin` on initialization. This is an opportunity to register handlers, etc. before plugin loading is considered complete by the host program. This function *must return* for the host to continue execution, so any long-running tasks should be delegated to additional threads. Notably, calling exposed functions in the host can only be done after the initializer finishes.

```c++
void plugin_initializer(Plugin *plugin) {
    // configure handlers callable by the host
    plugin->registerHandler("subtract", std::function([](int a, int b) -> int {
        return a - b;
    }));

    // do long-running tasks and call host functions in a separate thread
    std::thread([plugin]() {
        // call functions exposed by the host
        int result = plugin.call<int>("add", 2, 3);
    }).detach();
}
```

## Building

`libcosmo_plugin` relies heavily on C++ templates, so it is recommended to build and link it with your application directly, instead of as a separate library.

The easiest way to build `libcosmo_plugin` is with CMake. Add this repository as a submodule to your project, then include it as a subdirectory in your `CMakeLists.txt`. The variables `LIBCOSMO_PLUGIN_SOURCES` and `LIBCOSMO_PLUGIN_INCLUDE_DIRS` will be populated with the source files and include directories, respectively, which can be added to your application and shared library (plugin) builds.

`libcosmo_plugin` relies on `cosmo_dlopen`, which is not available on all platforms supported by Cosmopolitan. Plugins can therefore be built either in "shared library" mode for `cosmo_dlopen` to load the plugin in the process's address space, or in "executable mode" for `posix_spawn` to launch the plugin as a subprocess. Sensible defaults are enabled by `libcosmo_plugin` to leverage `cosmo_dlopen` wherever reliably implemented, and `posix_spawn` elsewhere. The following table lists the defaults:

| Platform       | Launch method  |
|-|-|
| Linux          | `cosmo_dlopen` |
| MacOS (x86_64) | `posix_spawn`  |
| MacOS (arm64)  | `cosmo_dlopen` |
| Windows        | `cosmo_dlopen` |
| FreeBSD        | `cosmo_dlopen` |
| NetBSD         | `posix_spawn`  |
| OpenBSD        | `posix_spawn`  |

Compiling native plugins will follow these defaults when enabling code. Specifically, platforms where `posix_spawn` is default will generate a `main()` function that calls the plugin initialization routines within `libcosmo_plugin`, effectively turning the plugins into executables. To disable this, define `COSMO_PLUGIN_DONT_GENERATE_MAIN` when building `libcosmo_plugin`. On platforms where `cosmo_dlopen` is the default, enabling the `main()` function can be done by defining `COSMO_PLUGIN_WANT_MAIN` when building `libcosmo_plugin`.

## License

`libcosmo_plugin` is released under the MIT License. It also relies on code from the [`reflect-cpp`](https://github.com/getml/reflect-cpp) library; refer to their documentation for up to date licensing information.
