#include "cosmo_plugin.hpp"

#ifdef __COSMOPOLITAN__

#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include <cosmo.h>
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

        // Select the socket with a timeout
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSocket, &readfds);
        timeval timeout{.tv_sec = 2, .tv_usec = 0};
        if (select(serverSocket + 1, &readfds, nullptr, nullptr, &timeout) < 0) {
            throw std::runtime_error("Failed to select socket: " + std::string(strerror(errno)));
        }
        if (!FD_ISSET(serverSocket, &readfds)) {
            throw std::runtime_error("Timeout waiting for connection.");
        }

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
    void (*cosmo_rpc_initialization)(int);
    void (*cosmo_rpc_teardown)();

    int childPID = 0;

    SocketManager* mgr = nullptr;

    ~impl() {
        if (mgr) {
            delete mgr;
        }

        if (dynlibHandle) {
            cosmo_rpc_teardown();
            cosmo_dlclose(dynlibHandle);
        }

        if (childPID) {
            kill(childPID, SIGKILL);
            waitpid(childPID, nullptr, 0);
        }
    }
};

PluginHost::PluginHost(const std::string& pluginPath, PluginHost::LaunchMethod launchMethod) : pluginPath(pluginPath), pimpl(new impl) {
    if (launchMethod == AUTO) {
        if (IsXnu() && !IsXnuSilicon()) {
            launchMethod = FORK;
        } else if (IsOpenbsd() || IsNetbsd()) { // netbsd dlopen seems broken
            launchMethod = FORK;
        } else {
            launchMethod = DLOPEN;
        }
    }
    this->launchMethod = launchMethod;
}

PluginHost::~PluginHost() {}

void PluginHost::initialize() {
    // Create our socket manager
    pimpl->mgr = new SocketManager();
    pimpl->mgr->startServer();

    if (launchMethod == DLOPEN) {
        // Load the shared object
        pimpl->dynlibHandle = cosmo_dlopen(pluginPath.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (!pimpl->dynlibHandle) {
            throw std::runtime_error("Failed to load shared object: " + std::string(cosmo_dlerror()));
        }

        // Get the address of the cosmo_rpc_initialization function
        pimpl->cosmo_rpc_initialization = reinterpret_cast<void(*)(int)>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_initialization")));
        if (!pimpl->cosmo_rpc_initialization) {
            throw std::runtime_error("Failed to find symbol: cosmo_rpc_initialization: " + std::string(cosmo_dlerror()));
        }

        // Get the address of the cosmo_rpc_teardown function
        pimpl->cosmo_rpc_teardown = reinterpret_cast<void(*)()>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_teardown")));
        if (!pimpl->cosmo_rpc_teardown) {
            throw std::runtime_error("Failed to find symbol: cosmo_rpc_teardown: " + std::string(cosmo_dlerror()));
        }

        // Call the cosmo_rpc_initialization function
        pimpl->cosmo_rpc_initialization(pimpl->mgr->getServerPort());
    } else if (launchMethod == FORK) {
        // posix_spawn a child process
        int pid;
        std::string port = std::to_string(pimpl->mgr->getServerPort());

        int res = posix_spawn(&pid, pluginPath.c_str(), nullptr, nullptr, (char* const[]){pluginPath.data(), port.data(), nullptr}, nullptr);
        if (res != 0) {
            throw std::runtime_error("Failed to spawn process: " + std::string(strerror(res)));
        }

        pimpl->childPID = pid;
    } else {
        throw std::runtime_error("Unsupported launch method.");
    }

    // Accept from the client
    pimpl->mgr->acceptConnection();

    // Create the transport
    transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
        SocketManager* mgr = static_cast<SocketManager*>(context);
        return send(mgr->getSocketFD(), buffer, size, 0);
    };
    transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
        SocketManager* mgr = static_cast<SocketManager*>(context);
        return recv(mgr->getSocketFD(), buffer, size, 0);
    };
    transport.close = [](void* context) {
        SocketManager* mgr = static_cast<SocketManager*>(context);
        mgr->closeSocket();
    };
    transport.context = pimpl->mgr;

    // Start thread
   std::thread([this]() {
        try {
            processMessages();
            std::cout << "Host thread ended." << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Error processing messages: " << ex.what() << std::endl;
        }
    }).detach();
}

#else // __COSMOPOLITAN__

#if !defined(COSMO_PLUGIN_DONT_GENERATE_MAIN) && !defined(COSMO_PLUGIN_WANT_MAIN)
# if defined(__APPLE__) && defined(__x86_64__)
#  define COSMO_PLUGIN_WANT_MAIN
# elif defined(__OpenBSD__) || defined(__NetBSD__)
#  define COSMO_PLUGIN_WANT_MAIN
# endif
#endif // COSMO_PLUGIN_DONT_GENERATE_MAIN

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

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

Plugin::Plugin() {}

Plugin::~Plugin() {
    if (transport.context) {
        transport.close(transport.context);
        delete static_cast<int*>(transport.context);
    }
}

struct SharedObjectContext {
    Plugin *plugin;
};

SharedObjectContext *sharedObjectContext = nullptr;

extern "C" EXPORT void cosmo_rpc_initialization(int port) {
    Plugin* plugin = new Plugin();

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

    // Step 4: Populate Transport struct using the connected socket
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
    plugin->transport = transport;

    // Step 5: Pass the Plugin to the shared library initialization function
    try {
        plugin_initializer(plugin);
    } catch (const std::exception& ex) {
        std::cerr << "Error during shared library initialization: " << ex.what() << std::endl;
        delete plugin;
        exit(EXIT_FAILURE);
    }

    // Step 7: Process incoming messages in a thread
    std::thread([plugin]() {
        try {
            plugin->processMessages();
            std::cout << "Client thread ended." << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Error processing messages: " << ex.what() << std::endl;
        }
    }).detach();

    // Step 8: Store the shared object context
    sharedObjectContext = new SharedObjectContext{plugin};
}

extern "C" EXPORT void cosmo_rpc_teardown() {
    if (sharedObjectContext) {
        if (sharedObjectContext->plugin) {
            delete sharedObjectContext->plugin;
        }
        delete sharedObjectContext;
        sharedObjectContext = nullptr;
    }
}

#ifdef COSMO_PLUGIN_WANT_MAIN

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <port>" << std::endl;
        return 1;
    }

    int port = atoi(argv[1]);

    if (port == 0) {
        std::cerr << "Invalid port number." << std::endl;
        return 1;
    }

    cosmo_rpc_initialization(port);

    while(sharedObjectContext) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}

#endif // COSMO_PLUGIN_WANT_MAIN

#endif // __COSMOPOLITAN__

void RPCPeer::sendMessage(const std::string& message) {
    ssize_t bytesSent = transport.write(message.c_str(), message.size(), transport.context);
    if (bytesSent == -1 || static_cast<size_t>(bytesSent) != message.size()) {
        throw std::runtime_error("Failed to send message.");
    }
}

std::string RPCPeer::receiveMessage() {
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
        std::string message = receiveMessage();
        if (message.empty()) {
            // connection aborted, shut down
            break;
        }

        Message jsonMessage = deserialize(message);
        if (!jsonMessage.method.empty()) {
            std::thread([this, jsonMessage]() {
                processRequest(jsonMessage);
            }).detach();
        } else if (jsonMessage.id) {
            std::lock_guard<std::mutex> lock(responseQueueMutex);
            responseQueue[jsonMessage.id] = jsonMessage;
        } else {
            throw std::runtime_error("Invalid RPC message format.");
        }
    }
}

void RPCPeer::processRequest(const Message& request) {
    Message msg;
    try {
        std::function<rfl::Generic(const std::vector<rfl::Generic>&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            if (handlers.find(request.method) == handlers.end()) {
                throw std::runtime_error("Method not found: " + request.method);
            }

            handler = handlers[request.method];
        }

        rfl::Generic response = handler(request.params);
        msg = constructResponse(request.id, response, "");
    } catch (const std::exception& ex) {
        std::cerr << "Error processing request: " << ex.what() << std::endl;
        msg = constructResponse(request.id, nullptr, ex.what());
    }
    sendMessage(rfl::json::write(msg));
}

RPCPeer::Message RPCPeer::waitForResponse(unsigned long id) {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(responseQueueMutex);
            if (responseQueue.find(id) != responseQueue.end()) {
                Message response = responseQueue[id];
                responseQueue.erase(id);
                return response;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Construct an RPC request
RPCPeer::Message RPCPeer::constructRequest(unsigned long id, const std::string& method, const std::vector<rfl::Generic>& params) {
    return Message{
        .id = id,
        .method = method,
        .params = params,
    };
}

// Construct an RPC response
RPCPeer::Message RPCPeer::constructResponse(unsigned long id, const rfl::Generic& result, const std::string& error) {
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
