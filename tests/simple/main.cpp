#include <filesystem>
#include <iostream>

#include "cosmo_rpc.hpp"

#ifdef __COSMOPOLITAN__

int main() {
    std::string cwd = std::filesystem::current_path();
    std::string objectPath = cwd + "/libnative.so";

    std::cout << "Connecting to shared library..." << std::endl;
    RPCPeer peer(objectPath);

    std::cout << "Calling remote function..." << std::endl;
    int result = peer.call<int>("add", 2, 3);

    std::cout << "Result: " << result << std::endl;
    assert(result == 5);

    return 0;
}

#else

void sharedLibraryInitialization(RPCPeer *peer) {
    peer->registerHandler<int, int, int>("add", std::function([](int a, int b) -> int {
        return a + b;
    }));
}

#endif