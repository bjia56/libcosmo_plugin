#ifndef COSMO_RPC_HPP
#define COSMO_RPC_HPP

#include <mutex>

#include "./nlohmann/json.hpp"

struct Transport {
    ssize_t (*write)(const void* buffer, size_t size, void* context);
    ssize_t (*read)(void* buffer, size_t size, void* context);
    void* context; // User-provided context (e.g., socket, file descriptor)
};

class Protocol {
public:

    // Serialize an RPC request
    static std::string serializeRequest(const std::string& id, const std::string& method, const nlohmann::json& params);

    // Serialize an RPC response
    static std::string serializeResponse(const std::string& id, const nlohmann::json& result, const std::string& error);

    // Deserialize an RPC message (request or response)
    static nlohmann::json deserialize(const std::string& message);
};

class RPCPeer {
public:
#ifdef __COSMOPOLITAN__
    RPCPeer(const std::string& dynlibPath);
#endif

    RPCPeer(Transport transport);
    ~RPCPeer();

    // Register a handler for a specific method
    template <typename ReturnType, typename... Args>
    void registerHandler(const std::string& method, std::function<ReturnType(Args...)> handler);

    // Call a method on the peer and get a response
    template <typename ReturnType, typename... Args>
    ReturnType call(const std::string& method, Args&&... args);

    // Process incoming requests and responses
    void processMessages();

private:
    Transport transport;

    // Handlers for incoming requests
    std::unordered_map<std::string, std::function<nlohmann::json(const nlohmann::json&)>> handlers;

    // Mutex for thread safety
    std::mutex handlersMutex;

    // Helper methods
    void sendMessage(const std::string& message);
    std::string receiveMessage();
    void processRequest(const nlohmann::json& request);

#ifdef __COSMOPOLITAN__
    struct impl;
    std::unique_ptr<impl> pimpl;
#endif
};

template <typename ReturnType, typename... Args>
void RPCPeer::registerHandler(const std::string& method, std::function<ReturnType(Args...)> handler) {
    std::lock_guard<std::mutex> lock(handlersMutex);
    handlers[method] = [handler](const nlohmann::json& params) -> nlohmann::json {
        // Deserialize the arguments from the JSON array
        std::tuple<Args...> args = params.get<std::tuple<Args...>>();

        // Call the handler with the deserialized arguments
        ReturnType result = std::apply(handler, args);

        // Serialize the result into a JSON object
        return nlohmann::json(result);
    };
}

template <typename ReturnType, typename... Args>
ReturnType RPCPeer::call(const std::string& method, Args&&... args) {
    // Generate a unique request ID
    static int requestCounter = 0;
    std::string requestID = std::to_string(++requestCounter);

    // Serialize the arguments into a JSON array
    nlohmann::json params = nlohmann::json::array({std::forward<Args>(args)...});

    // Serialize the RPC request
    std::string request = Protocol::serializeRequest(requestID, method, params);

    // Send the request to the peer
    sendMessage(request);

    // Wait for the response
    std::string response = receiveMessage();
    nlohmann::json jsonResponse = Protocol::deserialize(response);

    // Validate the response ID
    if (jsonResponse["id"] != requestID) {
        throw std::runtime_error("Mismatched response ID: expected " + requestID +
                                 ", got " + jsonResponse["id"].get<std::string>());
    }

    // Check for errors in the response
    if (!jsonResponse["error"].is_null()) {
        throw std::runtime_error("RPC error: " + jsonResponse["error"].get<std::string>());
    }

    // Deserialize the result into the expected return type
    try {
        return jsonResponse["result"].get<ReturnType>();
    } catch (const nlohmann::json::exception& ex) {
        throw std::runtime_error("Failed to deserialize result: " + std::string(ex.what()));
    }
}

#ifndef __COSMOPOLITAN__

// Must be defined in the shared object
void sharedLibraryInitialization(RPCPeer *peer);

#endif // __COSMOPOLITAN__

#endif // COSMO_RPC_HPP



#ifndef COSMO_RPC_CPP
#define COSMO_RPC_CPP

#include "cosmo_rpc.hpp"

#ifdef __COSMOPOLITAN__

#include <cerrno>
#include <cstring>
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

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

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
    // Step 1: Create a socket
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }

    // Step 2: Set up the server address structure for localhost
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &serverAddress.sin_addr) <= 0) {
        std::cerr << "Invalid address or address not supported." << std::endl;
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Step 3: Connect to the server
    if (connect(sockfd, (struct sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
        std::cerr << "Failed to connect to server: " << strerror(errno) << std::endl;
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Step 4: Define the Transport struct using the connected socket
    Transport transport;
    transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
        int sock = *static_cast<int*>(context);
        return send(sock, buffer, size, 0);
    };
    transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
        int sock = *static_cast<int*>(context);
        return recv(sock, buffer, size, 0);
    };
    transport.context = new int;
    *static_cast<int*>(transport.context) = sockfd;

    // Step 5: Create an RPCPeer using the Transport
    RPCPeer *peer = new RPCPeer(transport);

    // Step 6: Pass the RPCPeer to the shared library initialization function
    try {
        sharedLibraryInitialization(peer);
    } catch (const std::exception& ex) {
        std::cerr << "Error during shared library initialization: " << ex.what() << std::endl;
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Step 7: Process incoming messages in a thread
    std::thread *messageThread = new std::thread([peer]() {
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
        nlohmann::json jsonMessage = Protocol::deserialize(message);

        if (jsonMessage.contains("method")) {
            processRequest(jsonMessage);
        } else if (jsonMessage.contains("result") || jsonMessage.contains("error")) {
            throw std::runtime_error("Unexpected response message.");
        } else {
            throw std::runtime_error("Invalid RPC message format.");
        }
    }
}

void RPCPeer::processRequest(const nlohmann::json& request) {
    std::string method = request["method"];
    nlohmann::json params = request["params"];
    std::string id = request["id"];

    nlohmann::json response;

    try {
        std::lock_guard<std::mutex> lock(handlersMutex);
        if (handlers.find(method) == handlers.end()) {
            throw std::runtime_error("Method not found: " + method);
        }

        response = handlers[method](params);
        sendMessage(Protocol::serializeResponse(id, response, ""));
    } catch (const std::exception& ex) {
        std::cerr << "Error processing request: " << ex.what() << std::endl;
        sendMessage(Protocol::serializeResponse(id, nullptr, ex.what()));
    }
}


// Serialize an RPC request
std::string Protocol::serializeRequest(const std::string& id, const std::string& method, const nlohmann::json& params) {
    nlohmann::json request = {
        {"id", id},
        {"method", method},
        {"params", params},
        {"type", "request"}
    };
    return request.dump(); // Convert JSON object to a string
}

// Serialize an RPC response
std::string Protocol::serializeResponse(const std::string& id, const nlohmann::json& result, const std::string& error) {
    nlohmann::json response = {
        {"id", id},
        {"result", result},
        {"type", "response"}
    };
    if (!error.empty()) {
        response["error"] = error;
    }
    return response.dump(); // Convert JSON object to a string
}

// Deserialize an RPC message (request or response)
nlohmann::json Protocol::deserialize(const std::string& message) {
    try {
        return nlohmann::json::parse(message); // Convert string back to JSON
    } catch (const nlohmann::json::parse_error& ex) {
        throw std::runtime_error("Failed to parse JSON: " + std::string(ex.what()));
    }
}

#endif // COSMO_RPC_CPP