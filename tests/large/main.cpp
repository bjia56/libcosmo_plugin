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

std::vector<std::string> generateLargeVector(size_t size) {
    std::vector<std::string> result;
    result.reserve(size);

    for (size_t i = 0; i < size; ++i) {
        std::ostringstream oss;
        oss << "String_" << i;
        result.emplace_back(oss.str());
    }

    return result;
}

bool areVectorsEquivalent(const std::vector<std::string>& vec1, const std::vector<std::string>& vec2) {
    return vec1.size() == vec2.size() && std::equal(vec1.begin(), vec1.end(), vec2.begin());
}


void registerTestHandlers(RPCPeer& peer, bool* done) {
    peer.registerHandler("generate", std::function([](size_t size) -> std::vector<std::string> {
        return generateLargeVector(size);
    }));
    peer.registerHandler("done", std::function([done]() -> int {
        *done = true;
        return 1;
    }));
}

void testPeerHandlers(RPCPeer& peer) {
	for (int i = 0; i < 1000; i++) {
        auto expected = generateLargeVector(i);
		auto actual = peer.call<std::vector<std::string>>("generate", i);
        assert(areVectorsEquivalent(expected, actual));
	}
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

