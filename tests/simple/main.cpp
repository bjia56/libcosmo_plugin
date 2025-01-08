#include <filesystem>
#include <iostream>
#include <string>

#include "cosmo_rpc.hpp"

struct MyData {
    int a;
    std::string b;
};

#ifdef __COSMOPOLITAN__

using namespace std::literals::string_literals;

int main() {
    std::string cwd = std::filesystem::current_path();
    std::string objectPath = cwd + "/libnative.so";

    std::cout << "Connecting to shared library..." << std::endl;
    RPCPeer peer(objectPath);

    std::cout << "Calling remote function 'add'..." << std::endl;
    int result = peer.call<int>("add", 2, 3);

    std::cout << "Result: " << result << std::endl;
    assert(result == 5);

    std::cout << "Calling remote function 'create'..." << std::endl;
    MyData data = peer.call<MyData>("create", 10, "20"s);

    std::cout << "Result: {" << data.a << ", " << data.b << "}" << std::endl;
    assert(data.a == 10);
    assert(data.b == "20");

    return 0;
}

#else

void sharedLibraryInitialization(RPCPeer *peer) {
    peer->registerHandler("add", std::function([](int a, int b) -> int {
        return a + b;
    }));
    peer->registerHandler("create", std::function([](int a, std::string b) -> MyData {
        return MyData{a, b};
    }));
}

#endif