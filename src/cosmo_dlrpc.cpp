#include "cosmo_dlrpc.hpp"

#ifdef __COSMOPOLITAN__

#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
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
        serverAddress.sin_addr.s_addr = INADDR_ANY; // Bind to all available interfaces
        serverAddress.sin_port = 0;                 // Let the OS choose a random port
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

struct RPCPeer::impl {
    void* dynlibHandle = nullptr;
    void (*cosmo_rpc_initialization)(int);
    void (*cosmo_rpc_teardown)();
    SocketManager* mgr = nullptr;
};

RPCPeer::RPCPeer(const std::string& dynlibPath) {
    pimpl = std::make_unique<impl>();

    pimpl->dynlibHandle = cosmo_dlopen(dynlibPath.c_str(), RTLD_LOCAL | RTLD_NOW);
    if (!pimpl->dynlibHandle) {
        throw std::runtime_error("Failed to load shared object: " + std::string(cosmo_dlerror()));
    }

    // Get the address of the cosmo_rpc_initialization function
    pimpl->cosmo_rpc_initialization = reinterpret_cast<void(*)(int)>(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_initialization"));
    if (!pimpl->cosmo_rpc_initialization) {
        throw std::runtime_error("Failed to find symbol: cosmo_rpc_initialization: " + std::string(cosmo_dlerror()));
    }

    // Get the address of the cosmo_rpc_teardown function
    pimpl->cosmo_rpc_teardown = reinterpret_cast<void(*)()>(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_teardown"));
    if (!pimpl->cosmo_rpc_teardown) {
        throw std::runtime_error("Failed to find symbol: cosmo_rpc_teardown: " + std::string(cosmo_dlerror()));
    }

    // Create our socket manager
    pimpl->mgr = new SocketManager();
    pimpl->mgr->startServer();

    // Call the cosmo_rpc_initialization function
    pimpl->cosmo_rpc_initialization(pimpl->mgr->getServerPort());

    // Accept a connection from the client
    pimpl->mgr->acceptConnection();

    // Create the transport
    transport = Transport();
    transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
        SocketManager* mgr = static_cast<SocketManager*>(context);
        return send(mgr->getSocketFD(), buffer, size, 0);
    };
    transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
        SocketManager* mgr = static_cast<SocketManager*>(context);
        return recv(mgr->getSocketFD(), buffer, size, 0);
    };
    transport.context = pimpl->mgr;
}

RPCPeer::~RPCPeer() {
    if (pimpl->mgr) {
        delete pimpl->mgr;
    }

    if (pimpl->dynlibHandle) {
        pimpl->cosmo_rpc_teardown();
        cosmo_dlclose(pimpl->dynlibHandle);
    }
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

#if defined(_MSC_VER)
    //  Microsoft
    #define EXPORT __declspec(dllexport)
    #define IMPORT __declspec(dllimport)
#elif defined(__GNUC__)
    //  GCC
    #define EXPORT __attribute__((visibility("default")))
    #define IMPORT
#else
    //  do nothing and hope for the best?
    #define EXPORT
    #define IMPORT
    #pragma warning Unknown dynamic link import/export semantics.
#endif

RPCPeer::~RPCPeer() {
    if (transport.context) {
        int sockfd = *static_cast<int*>(transport.context);
        close(sockfd);
    }
}

struct SharedObjectContext {
    RPCPeer *peer;
    std::thread *messageThread;
};

SharedObjectContext *sharedObjectContext = nullptr;

extern "C" EXPORT void cosmo_rpc_initialization(int port) {
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
    if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) <= 0) {
        std::cerr << "Invalid address or address not supported." << std::endl;
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
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
    Transport transport;
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
    transport.context = new int;
    *static_cast<int*>(transport.context) = sockfd;

    // Step 5: Create an RPCPeer using the Transport
    RPCPeer* peer = new RPCPeer(transport);

    // Step 6: Pass the RPCPeer to the shared library initialization function
    try {
        sharedLibraryInitialization(peer);
    } catch (const std::exception& ex) {
        std::cerr << "Error during shared library initialization: " << ex.what() << std::endl;
#ifdef _WIN32
        closesocket(sock);
        WSACleanup();
#else
        close(sockfd);
#endif
        exit(EXIT_FAILURE);
    }

    // Step 7: Process incoming messages in a thread
    std::thread* messageThread = new std::thread([peer]() {
        try {
            peer->processMessages();
        } catch (const std::exception& ex) {
            std::cerr << "Error processing messages: " << ex.what() << std::endl;
        }
    });

    // Step 8: Store the shared object context
    sharedObjectContext = new SharedObjectContext{peer, messageThread};
}

extern "C" EXPORT void cosmo_rpc_teardown() {
    if (sharedObjectContext) {
        if (sharedObjectContext->peer) {
            delete sharedObjectContext->peer;
        }
        if (sharedObjectContext->messageThread) {
            sharedObjectContext->messageThread->join();
            delete sharedObjectContext->messageThread;
        }
        delete sharedObjectContext;
    }
}

#endif // __COSMOPOLITAN__

RPCPeer::RPCPeer(Transport transport) : transport(transport) {}

void RPCPeer::sendMessage(const std::string& message) {
    ssize_t bytesSent = transport.write(message.c_str(), message.size(), transport.context);
    if (bytesSent == -1 || static_cast<size_t>(bytesSent) != message.size()) {
        throw std::runtime_error("Failed to send message.");
    }
}

std::string RPCPeer::receiveMessage() {
    char buffer[1024];
    ssize_t bytesReceived = transport.read(buffer, sizeof(buffer) - 1, transport.context);

    if (bytesReceived == -1) {
        throw std::runtime_error("Failed to receive message.");
    } else if (bytesReceived == 0) {
        throw std::runtime_error("Connection closed by peer.");
    }

    buffer[bytesReceived] = '\0';
    return std::string(buffer);
}

void RPCPeer::processMessages() {
    while (true) {
        std::string message = receiveMessage();
        Protocol::Message jsonMessage = Protocol::deserialize(message);

        if (!jsonMessage.method.empty()) {
            processRequest(jsonMessage);
        } else {
            throw std::runtime_error("Invalid RPC message format.");
        }
    }
}

void RPCPeer::processRequest(const Protocol::Message& request) {
    Protocol::Message msg;
    try {
        std::lock_guard<std::mutex> lock(handlersMutex);
        if (handlers.find(request.method) == handlers.end()) {
            throw std::runtime_error("Method not found: " + request.method);
        }

        rfl::Generic response = handlers[request.method](request.params);
        msg = Protocol::serializeResponse(request.id, response, "");
    } catch (const std::exception& ex) {
        std::cerr << "Error processing request: " << ex.what() << std::endl;
        msg = Protocol::serializeResponse(request.id, nullptr, ex.what());
    }
    sendMessage(rfl::json::write(msg));
}


// Serialize an RPC request
Protocol::Message Protocol::serializeRequest(const std::string& id, const std::string& method, const std::vector<rfl::Generic>& params) {
    return Message{
        .id = id,
        .method = method,
        .params = params,
    };
}

// Serialize an RPC response
Protocol::Message Protocol::serializeResponse(const std::string& id, const rfl::Generic& result, const std::string& error) {
    return Message{
        .id = id,
        .result = result,
        .error = error,
    };
}

// Deserialize an RPC message (request or response)
Protocol::Message Protocol::deserialize(const std::string& message) {
    return rfl::json::read<Protocol::Message>(message).value();
}