#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <map>
#include <cassert>

#include "cosmo_plugin.hpp"

const int64_t signedValue = 8331091968;
const uint64_t unsignedValue = 8331091968;

void registerTestHandlers(RPCPeer& peer, bool* done) {
    peer.registerHandler("signed", std::function([=](int64_t value) -> int64_t {
        return value;
    }));
    peer.registerHandler("unsigned", std::function([=](uint64_t value) -> uint64_t {
        return value;
    }));
    peer.registerHandler("done", std::function([done]() -> int {
        *done = true;
        return 1;
    }));
}

void testPeerHandlers(RPCPeer& peer) {
    assert(signedValue == peer.call<int64_t>("signed", signedValue));
    assert(unsignedValue == peer.call<uint64_t>("unsigned", unsignedValue));
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

