#include <filesystem>
#include <iostream>
#include <string>

#include "cosmo_plugin.hpp"

struct MyData {
    int a;
    std::string b;
};

#ifdef __COSMOPOLITAN__

using namespace std::literals::string_literals;

int main(int argc, char *argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <shared library path>" << std::endl;
        return 1;
    }

    std::string cwd = std::filesystem::current_path();
    std::string objectPath = cwd + "/" + argv[1];

    std::cout << "Connecting to shared library..." << std::endl;
    PluginHost plugin(objectPath);
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
}

#endif