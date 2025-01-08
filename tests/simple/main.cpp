#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "cosmo_plugin.hpp"

struct MyData {
    int a;
    std::string b;
};

using namespace std::literals::string_literals;

#ifdef __COSMOPOLITAN__

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <shared library path>" << std::endl;
        return 1;
    }

    std::string cwd = std::filesystem::current_path();
    std::string objectPath = cwd + "/" + argv[1];

    std::cout << "Populating host functions..." << std::endl;
    PluginHost plugin(objectPath);
    plugin.registerHandler("subtract", std::function([](int a, int b) -> int {
        return a - b;
    }));
    plugin.registerHandler("create", std::function([](int a, std::string b) -> MyData {
        return MyData{a, b};
    }));

    bool done = false;
    plugin.registerHandler("exit", std::function([&done]() -> int {
        done = true;
        return 1;
    }));

    std::cout << "Initializing shared library..." << std::endl;
    plugin.initialize();

    std::cout << "Calling remote function 'add'..." << std::endl;
    int result = plugin.call<int>("add", 2, 3);

    std::cout << "Result: " << result << std::endl;
    assert(result == 5);

    std::cout << "Calling remote function 'create'..." << std::endl;
    MyData data = plugin.call<MyData>("create", 10, "20"s);

    std::cout << "Result: {" << data.a << ", " << data.b << "}" << std::endl;
    assert(data.a == 10);
    assert(data.b == "20");

    plugin.call<int>("exit");

    while(!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}

#else

void plugin_initializer(Plugin *plugin) {
    plugin->registerHandler("add", std::function([](int a, int b) -> int {
        return a + b;
    }));
    plugin->registerHandler("create", std::function([](int a, std::string b) -> MyData {
        return MyData{a, b};
    }));

    bool done = false;
    plugin->registerHandler("exit", std::function([&done]() -> int {
        done = true;
        return 1;
    }));

    std::cout << "Plugin initialized." << std::endl;

    std::thread([plugin, &done]() {
        std::cout << "Calling host function 'subtract'..." << std::endl;
        int result = plugin->call<int>("subtract", 10, 5);

        std::cout << "Result: " << result << std::endl;
        assert(result == 5);

        std::cout << "Calling host function 'create'..." << std::endl;
        MyData data = plugin->call<MyData>("create", 10, "20"s);

        std::cout << "Result: {" << data.a << ", " << data.b << "}" << std::endl;
        assert(data.a == 10);
        assert(data.b == "20");

        plugin->call<int>("exit");

        while(!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

#endif