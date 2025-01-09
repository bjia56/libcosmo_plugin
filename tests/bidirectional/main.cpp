#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <cassert>

#include "cosmo_plugin.hpp"

struct MyData {
    int a;
    std::string b;
};

struct ComplexData {
    std::vector<MyData> dataList;
    std::map<std::string, int> dataMap;
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
    plugin.registerHandler("processComplexData", std::function([](ComplexData cd) -> std::string {
        int total = 0;
        for (const auto &item : cd.dataList) {
            total += item.a;
        }
        for (const auto &[key, value] : cd.dataMap) {
            total += value;
        }
        return "Total: " + std::to_string(total);
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

    std::cout << "Calling remote function 'processComplexData'..." << std::endl;
    ComplexData complexData{
        .dataList = {{1, "One"}, {2, "Two"}},
        .dataMap = {{"key1", 10}, {"key2", 20}}
    };
    std::string summary = plugin.call<std::string>("processComplexData", complexData);
    std::cout << "Result: " << summary << std::endl;
    assert(summary == "Total: 33");

    plugin.call<int>("exit");

    while(!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Exiting..." << std::endl;

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
    plugin->registerHandler("processComplexData", std::function([](ComplexData cd) -> std::string {
        int total = 0;
        for (const auto &item : cd.dataList) {
            total += item.a;
        }
        for (const auto &[key, value] : cd.dataMap) {
            total += value;
        }
        return "Total: " + std::to_string(total);
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

        std::cout << "Calling host function 'processComplexData'..." << std::endl;
        ComplexData complexData{
            .dataList = {{3, "Three"}, {4, "Four"}},
            .dataMap = {{"key3", 30}, {"key4", 40}}
        };
        std::string summary = plugin->call<std::string>("processComplexData", complexData);
        std::cout << "Result: " << summary << std::endl;
        assert(summary == "Total: 77");

        plugin->call<int>("exit");

        while(!done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

#endif

