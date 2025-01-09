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

void registerTestHandlers(RPCPeer& peer, bool* done) {
    peer.registerHandler("add", std::function([](int a, int b) -> int {
        return a + b;
    }));
    peer.registerHandler("subtract", std::function([](int a, int b) -> int {
        return a - b;
    }));
    peer.registerHandler("createData", std::function([](int a, std::string b) -> MyData {
        return MyData{a, b};
    }));
    peer.registerHandler("processComplexData", std::function([](ComplexData cd) -> std::string {
        int total = 0;
        for (const auto &item : cd.dataList) {
            total += item.a;
        }
        for (const auto &[key, value] : cd.dataMap) {
            total += value;
        }
        return "Total: " + std::to_string(total);
    }));
    peer.registerHandler("peerArithmetic", std::function([&peer](int a, int b, int c) -> int {
        int result = peer.call<int>("add", a, b);
        result = peer.call<int>("subtract", result, c);
        return result;
    }));
    peer.registerHandler("done", std::function([done]() -> int {
        *done = true;
        return 1;
    }));
}

void testPeerHandlers(RPCPeer& peer) {
    int result = peer.call<int>("add", 2, 3);
    assert(result == 5);

    result = peer.call<int>("subtract", 10, 5);
    assert(result == 5);

    MyData data = peer.call<MyData>("createData", 10, "20"s);
    assert(data.a == 10);
    assert(data.b == "20");

    ComplexData complexData{
        .dataList = {{1, "One"}, {2, "Two"}},
        .dataMap = {{"key1", 10}, {"key2", 20}}
    };
    std::string summary = peer.call<std::string>("processComplexData", complexData);
    assert(summary == "Total: 33");

    result = peer.call<int>("peerArithmetic", 10, 20, 5);
    assert(result == 25);

    peer.call<int>("done");
}

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
    bool done = false;
    registerTestHandlers(plugin, &done);

    std::cout << "Initializing shared library..." << std::endl;
    plugin.initialize();

    std::cout << "Testing plugin functions..." << std::endl;
    testPeerHandlers(plugin);

    while(!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Host exiting..." << std::endl;
    return 0;
}

#else

void plugin_initializer(Plugin *plugin) {
    bool *done = new bool;
    *done = false;
    registerTestHandlers(*plugin, done);

    std::cout << "Plugin initialized." << std::endl;

    std::thread([plugin, done]() {
        std::cout << "Testing host functions..." << std::endl;
        testPeerHandlers(*plugin);

        while(!*done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        delete done;
        std::cout << "Plugin exiting..." << std::endl;
    }).detach();
}

#endif

