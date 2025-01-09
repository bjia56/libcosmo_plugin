#include "cosmo_plugin.hpp"

#ifdef __COSMOPOLITAN__

#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <libc/dlopen/dlfcn.h>

class SocketManager {
public:
    SocketManager() {
        // Create the server socket
        serverSocket = socket(AF_INET, SOCK_STREAM, 0);
        if (serverSocket == -1) {
            throw std::runtime_error("Failed to create socket: " + std::string(strerror(errno)));
        }

        // Allow reuse of the address
        int opt = 1;
        if (setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error("Failed to set socket options: " + std::string(strerror(errno)));
        }

        // Initialize the server address structure
        memset(&serverAddress, 0, sizeof(serverAddress));
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = 0; // Let the OS choose a random port

        // Bind to 127.0.0.1
        if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) <= 0) {
            throw std::runtime_error("Failed to set server address to 127.0.0.1");
        }
    }

    ~SocketManager() {
        closeSocket();
    }

    // Starts the server and listens for a connection
    void startServer() {
        // Bind the server socket to the address and port
        if (bind(serverSocket, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) < 0) {
            throw std::runtime_error("Failed to bind socket: " + std::string(strerror(errno)));
        }

        // Retrieve the assigned port
        socklen_t addrLen = sizeof(serverAddress);
        if (getsockname(serverSocket, (struct sockaddr*)&serverAddress, &addrLen) == -1) {
            throw std::runtime_error("Failed to get socket name: " + std::string(strerror(errno)));
        }
        serverPort = ntohs(serverAddress.sin_port);

        // Start listening for incoming connections
        if (listen(serverSocket, 1) < 0) {
            throw std::runtime_error("Failed to listen on socket: " + std::string(strerror(errno)));
        }
    }

    // Accept a client connection
    void acceptConnection() {
        sockaddr_in clientAddress;
        socklen_t clientAddrLen = sizeof(clientAddress);

        // Accept a connection from a client
        clientSocket = accept(serverSocket, (struct sockaddr*)&clientAddress, &clientAddrLen);
        if (clientSocket < 0) {
            throw std::runtime_error("Failed to accept connection: " + std::string(strerror(errno)));
        }
    }

    // Getters for the connected socket
    int getSocketFD() const {
        return clientSocket;
    }

    // Close the socket
    void closeSocket() {
        if (clientSocket != -1) {
            close(clientSocket);
            clientSocket = -1;
        }
        if (serverSocket != -1) {
            close(serverSocket);
            serverSocket = -1;
        }
    }

    // Get the port the server is listening on
    int getServerPort() const {
        return serverPort;
    }

private:
    int serverSocket = -1;  // Server socket
    int clientSocket = -1;  // Connected client socket
    int serverPort = 0;     // Port the server is listening on

    sockaddr_in serverAddress{}; // Server address struct
};

struct PluginHost::impl {
    void* dynlibHandle = nullptr;
    void (*cosmo_rpc_initialization)(int, int);
    void (*cosmo_rpc_teardown)();


    SocketManager* toPlugin = nullptr;
    SocketManager* toHost = nullptr;

    std::thread* messageThread = nullptr;

    ~impl() {
        if (toPlugin) {
            delete toPlugin;
        }
        if (toHost) {
            delete toHost;
        }

        if (dynlibHandle) {
            cosmo_rpc_teardown();
            cosmo_dlclose(dynlibHandle);
        }

        if (messageThread) {
            messageThread->join();
            delete messageThread;
        }
    }
};

PluginHost::PluginHost(const std::string& dynlibPath) : pimpl(new impl) {
    this->dynlibPath = dynlibPath;
}

PluginHost::~PluginHost() {}

void PluginHost::initialize() {
    pimpl->dynlibHandle = cosmo_dlopen(dynlibPath.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!pimpl->dynlibHandle) {
        throw std::runtime_error("Failed to load shared object: " + std::string(cosmo_dlerror()));
    }

    // Get the address of the cosmo_rpc_initialization function
    pimpl->cosmo_rpc_initialization = reinterpret_cast<void(*)(int, int)>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_initialization")));
    if (!pimpl->cosmo_rpc_initialization) {
        throw std::runtime_error("Failed to find symbol: cosmo_rpc_initialization: " + std::string(cosmo_dlerror()));
    }

    // Get the address of the cosmo_rpc_teardown function
    pimpl->cosmo_rpc_teardown = reinterpret_cast<void(*)()>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_teardown")));
    if (!pimpl->cosmo_rpc_teardown) {
        throw std::runtime_error("Failed to find symbol: cosmo_rpc_teardown: " + std::string(cosmo_dlerror()));
    }

    // Create our socket managers
    pimpl->toPlugin = new SocketManager();
    pimpl->toPlugin->startServer();
    pimpl->toHost = new SocketManager();
    pimpl->toHost->startServer();

    // Call the cosmo_rpc_initialization function
    pimpl->cosmo_rpc_initialization(pimpl->toPlugin->getServerPort(), pimpl->toHost->getServerPort());

    // Accept from the client
    pimpl->toPlugin->acceptConnection();
    pimpl->toHost->acceptConnection();

    // Create the transports
    for (auto &transport : {&toPlugin, &toHost}) {
        transport->write = [](const void* buffer, size_t size, void* context) -> ssize_t {
            SocketManager* mgr = static_cast<SocketManager*>(context);
            return send(mgr->getSocketFD(), buffer, size, 0);
        };
        transport->read = [](void* buffer, size_t size, void* context) -> ssize_t {
            SocketManager* mgr = static_cast<SocketManager*>(context);
            return recv(mgr->getSocketFD(), buffer, size, 0);
        };
        transport->close = [](void* context) {
            SocketManager* mgr = static_cast<SocketManager*>(context);
            mgr->closeSocket();
        };
        transport->context = transport == &toPlugin ? pimpl->toPlugin : pimpl->toHost;
    }

    // Start thread
    pimpl->messageThread = new std::thread([this]() {
        try {
            processMessages();
        } catch (const std::exception& ex) {
            std::cerr << "Error processing messages: " << ex.what() << std::endl;
        }
    });
}

#else // __COSMOPOLITAN__

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib") // Link against Winsock library
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cerrno>
#endif

#include <cstring>
#include <iostream>
#include <string>
#include <thread>

Plugin::Plugin() {}

Plugin::~Plugin() {
    for (auto &transport : {&toHost, &toPlugin}) {
        if (transport->context) {
            transport->close(transport->context);
            delete static_cast<int*>(transport->context);
        }
    }
}

struct SharedObjectContext {
    Plugin *plugin;
    std::thread *messageThread;
};

SharedObjectContext *sharedObjectContext = nullptr;

extern "C" EXPORT void cosmo_rpc_initialization(int toPlugin, int toHost) {
    Plugin* plugin = new Plugin();

    for (auto port : {toPlugin, toHost}) {
#ifdef _WIN32
        // Initialize Winsock
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed with error: " << WSAGetLastError() << std::endl;
            exit(EXIT_FAILURE);
        }
#endif

        // Step 1: Create a socket
        int sockfd;
#ifdef _WIN32
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == INVALID_SOCKET) {
            std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
            WSACleanup();
            exit(EXIT_FAILURE);
        }
        sockfd = static_cast<int>(sock); // Cast SOCKET to int for consistency
#else
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd == -1) {
            std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
            exit(EXIT_FAILURE);
        }
#endif

        // Step 2: Set up the server address structure for localhost
        sockaddr_in serverAddress{};
        serverAddress.sin_family = AF_INET;
        serverAddress.sin_port = htons(port);
#ifdef _WIN32
        if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) != 1) {
            std::cerr << "Invalid address or address not supported." << std::endl;
            closesocket(sock);
            WSACleanup();
#else
        if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) <= 0) {
            std::cerr << "Invalid address or address not supported." << std::endl;
            close(sockfd);
#endif
            exit(EXIT_FAILURE);
        }

        // Step 3: Connect to the server
        if (connect(sockfd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
#ifdef _WIN32
            std::cerr << "Failed to connect to server: " << WSAGetLastError() << std::endl;
            closesocket(sock);
            WSACleanup();
#else
            std::cerr << "Failed to connect to server: " << strerror(errno) << std::endl;
            close(sockfd);
#endif
            exit(EXIT_FAILURE);
        }

        // Step 4: Define the Transport struct using the connected socket
        RPCPeer::Transport transport;
        transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
            int sock = *static_cast<int*>(context);
#ifdef _WIN32
            return send(sock, static_cast<const char*>(buffer), static_cast<int>(size), 0);
#else
            return send(sock, buffer, size, 0);
#endif
        };
        transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
            int sock = *static_cast<int*>(context);
#ifdef _WIN32
            return recv(sock, static_cast<char*>(buffer), static_cast<int>(size), 0);
#else
            return recv(sock, buffer, size, 0);
#endif
        };
        transport.close = [](void* context) {
            int sock = *static_cast<int*>(context);
#ifdef _WIN32
            closesocket(sock);
            WSACleanup();
#else
            close(sock);
#endif
        };
        transport.context = new int;
        *static_cast<int*>(transport.context) = sockfd;

        // Step 5: Set the Transport in the Plugin
        if (port == toPlugin) {
            plugin->toPlugin = transport;
        } else {
            plugin->toHost = transport;
        }
    }

    // Step 6: Pass the Plugin to the shared library initialization function
    try {
        plugin_initializer(plugin);
    } catch (const std::exception& ex) {
        std::cerr << "Error during shared library initialization: " << ex.what() << std::endl;
        delete plugin;
        exit(EXIT_FAILURE);
    }

    // Step 7: Process incoming messages in a thread
    std::thread* messageThread = new std::thread([plugin]() {
        try {
            plugin->processMessages();
        } catch (const std::exception& ex) {
            std::cerr << "Error processing messages: " << ex.what() << std::endl;
        }
    });

    // Step 8: Store the shared object context
    sharedObjectContext = new SharedObjectContext{plugin, messageThread};
}

extern "C" EXPORT void cosmo_rpc_teardown() {
    if (sharedObjectContext) {
        if (sharedObjectContext->plugin) {
            delete sharedObjectContext->plugin;
        }
        if (sharedObjectContext->messageThread) {
            sharedObjectContext->messageThread->join();
            delete sharedObjectContext->messageThread;
        }
        delete sharedObjectContext;
    }
}

#endif // __COSMOPOLITAN__

void RPCPeer::sendMessage(const std::string& message, RPCPeer::Transport &transport) {
    ssize_t bytesSent = transport.write(message.c_str(), message.size(), transport.context);
    if (bytesSent == -1 || static_cast<size_t>(bytesSent) != message.size()) {
        throw std::runtime_error("Failed to send message.");
    }
}

std::string RPCPeer::receiveMessage(RPCPeer::Transport &transport) {
    char buffer[1024];
    ssize_t bytesReceived = transport.read(buffer, sizeof(buffer) - 1, transport.context);

#ifdef _WIN32
    if (bytesReceived == SOCKET_ERROR) {
        return "";
    }
#else
    if (bytesReceived <= 0) {
        return "";
    }
#endif

    buffer[bytesReceived] = '\0';
    return std::string(buffer);
}

void RPCPeer::processMessages() {
    while (true) {
        std::string message = receiveMessage(getInboundTransport());
        if (message.empty()) {
            // connection aborted, shut down
            break;
        }

        Message jsonMessage = deserialize(message);
        if (!jsonMessage.method.empty()) {
            processRequest(jsonMessage);
        } else {
            throw std::runtime_error("Invalid RPC message format.");
        }
    }
}

void RPCPeer::processRequest(const Message& request) {
    Message msg;
    try {
        std::lock_guard<std::mutex> lock(handlersMutex);
        if (handlers.find(request.method) == handlers.end()) {
            throw std::runtime_error("Method not found: " + request.method);
        }

        rfl::Generic response = handlers[request.method](request.params);
        msg = constructResponse(request.id, response, "");
    } catch (const std::exception& ex) {
        std::cerr << "Error processing request: " << ex.what() << std::endl;
        msg = constructResponse(request.id, nullptr, ex.what());
    }
    sendMessage(rfl::json::write(msg), getInboundTransport());
}

// Construct an RPC request
RPCPeer::Message RPCPeer::constructRequest(const std::string& id, const std::string& method, const std::vector<rfl::Generic>& params) {
    return Message{
        .id = id,
        .method = method,
        .params = params,
    };
}

// Construct an RPC response
RPCPeer::Message RPCPeer::constructResponse(const std::string& id, const rfl::Generic& result, const std::string& error) {
    return Message{
        .id = id,
        .result = result,
        .error = error,
    };
}

// Serialize an RPC message (request or response)
std::string RPCPeer::serialize(const Message& message) {
    return rfl::json::write(message);
}

// Deserialize an RPC message (request or response)
RPCPeer::Message RPCPeer::deserialize(const std::string& message) {
    return rfl::json::read<Message>(message).value();
}