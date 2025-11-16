#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <cassert>

#include "cosmo_plugin.hpp"

void registerTestHandlers(RPCPeer& peer, bool* done) {
    peer.registerHandler("add", std::function([](int a, int b) -> int {
        return a + b;
    }));
    peer.registerHandler("peerAdd", std::function([&peer](int a, int b) -> int {
        return peer.call<int>("add", a, b);
    }));
    peer.registerHandler("done", std::function([done]() -> int {
        *done = true;
        return 1;
    }));
}

void testPeerHandlers(RPCPeer& peer) {
    // Define the task for each thread
    auto task = [&peer]() {
        for (int i = 0; i < 1000; i++) {
            int result = peer.call<int>("add", 2, 3);
            assert(result == 5);
        }
        for (int i = 0; i < 1000; i++) {
            int result = peer.call<int>("peerAdd", 2, 3);
            assert(result == 5);
        }
    };

    // Create 20 threads
    std::vector<std::thread> threads;
    for (int t = 0; t < 20; t++) {
        threads.emplace_back(task);
    }

    // Join all threads
    for (auto& thread : threads) {
        thread.join();
    }

    // Call "done" after all threads are complete
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
    PluginHost::ProtocolEncoding encoding = PluginHost::ProtocolEncoding::MSGPACK;
#ifdef USE_JSON_ENCODING
    encoding = PluginHost::ProtocolEncoding::JSON;
#endif
    PluginHost plugin(objectPath, PluginHost::LaunchMethod::AUTO, encoding);
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

