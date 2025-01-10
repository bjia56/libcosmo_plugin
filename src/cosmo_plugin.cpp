#include "cosmo_plugin.hpp"

#ifdef __COSMOPOLITAN__

#include <cerrno>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <random>
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
        // Generate a random cookie
        generateCookie();

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

        // Verify the client's auth cookie
        int clientCookie;
        ssize_t bytesReceived = recv(clientSocket, &clientCookie, sizeof(clientCookie), 0);
        if (bytesReceived != sizeof(clientCookie) || clientCookie != cookie) {
            closeSocket();
            throw std::runtime_error("Invalid auth cookie.");
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

    // Get the auth cookie
    int getCookie() const {
        return cookie;
    }

private:
    int serverSocket = -1;  // Server socket
    int clientSocket = -1;  // Connected client socket
    int serverPort = 0;     // Port the server is listening on

    int cookie = 0;        // Auth cookie for client connecting to the server

    sockaddr_in serverAddress{}; // Server address struct

    void generateCookie() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dis(0, std::numeric_limits<int>::max());
        cookie = dis(gen);
    }
};

static pid_t launchSubprocessWithEnv(const char* program, const char* argv[], const char* newEnvVar) {
    // Step 1: Count existing environment variables
    size_t envCount = 0;
    while (environ[envCount] != nullptr) {
        envCount++;
    }

    // Step 2: Allocate memory for the new environment
    std::vector<const char*> newEnv(envCount + 2); // +1 for the new variable, +1 for null terminator

    // Step 3: Copy existing environment variables
    for (size_t i = 0; i < envCount; i++) {
        newEnv[i] = environ[i];
    }

    // Step 4: Add the new environment variable
    newEnv[envCount] = newEnvVar;
    newEnv[envCount + 1] = nullptr; // Null terminator

    // Step 5: Spawn the subprocess with the new environment
    pid_t pid;
    int status = posix_spawn(&pid, program, nullptr, nullptr, const_cast<char* const*>(argv), const_cast<char* const*>(newEnv.data()));

    if (status != 0) {
        throw std::runtime_error("Failed to spawn process: " + std::string(strerror(status)));
    }

    return pid;
}

struct PluginHost::impl {
    void* dynlibHandle = nullptr;
    void (*cosmo_rpc_initialization)(int, long);
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
        pimpl->cosmo_rpc_initialization = reinterpret_cast<void(*)(int, long)>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_initialization")));
        if (!pimpl->cosmo_rpc_initialization) {
            throw std::runtime_error("Failed to find symbol: cosmo_rpc_initialization: " + std::string(cosmo_dlerror()));
        }

        // Get the address of the cosmo_rpc_teardown function
        pimpl->cosmo_rpc_teardown = reinterpret_cast<void(*)()>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_teardown")));
        if (!pimpl->cosmo_rpc_teardown) {
            throw std::runtime_error("Failed to find symbol: cosmo_rpc_teardown: " + std::string(cosmo_dlerror()));
        }

        // Call the cosmo_rpc_initialization function
        pimpl->cosmo_rpc_initialization(pimpl->mgr->getServerPort(), pimpl->mgr->getCookie());
    } else if (launchMethod == FORK) {
        // posix_spawn a child process
        int pid;
        std::string port = std::to_string(pimpl->mgr->getServerPort());

        // Add cookie to child's environment
        std::stringstream cookieEnv;
        cookieEnv << "COSMO_PLUGIN_COOKIE=" << pimpl->mgr->getCookie();

        const char* argv[] = {pluginPath.c_str(), port.c_str(), nullptr};
        std::string cookieStr = cookieEnv.str();
        const char* newEnvVar = cookieStr.c_str();

        pid = launchSubprocessWithEnv(pluginPath.c_str(), argv, newEnvVar);
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

extern "C" EXPORT void cosmo_rpc_initialization(int port, int cookie) {
    Plugin* plugin = new Plugin();

#ifdef _WIN32
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed with error: " << WSAGetLastError() << std::endl;
        delete plugin;
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
        delete plugin;
        exit(EXIT_FAILURE);
    }
    sockfd = static_cast<int>(sock); // Cast SOCKET to int for consistency
#else
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        std::cerr << "Failed to create socket: " << strerror(errno) << std::endl;
        delete plugin;
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
        delete plugin;
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
        delete plugin;
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

    // Step 5: Send the auth cookie to the server
    if (transport.write(&cookie, sizeof(cookie), transport.context) == -1) {
        std::cerr << "Failed to send auth cookie." << std::endl;
        delete plugin;
        exit(EXIT_FAILURE);
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

    int cookie = 0;
    const char* cookieStr = getenv("COSMO_PLUGIN_COOKIE");
    if (cookieStr) {
        cookie = atoi(cookieStr);
    } else {
        std::cerr << "Missing auth cookie." << std::endl;
        return 1;
    }

    cosmo_rpc_initialization(port, cookie);

    while(sharedObjectContext) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return 0;
}

#endif // COSMO_PLUGIN_WANT_MAIN

#endif // __COSMOPOLITAN__

void RPCPeer::sendMessage(const Message& message) {
    std::lock_guard<std::mutex> lock(sendMutex);
    std::string messageStr = rfl::json::write(message);
#ifdef COSMO_PLUGIN_DEBUG_RPC
# ifdef __COSMOPOLITAN__
    std::cerr << "Host sending: " << messageStr << std::endl;
# else
    std::cerr << "Plugin sending: " << messageStr << std::endl;
# endif
#endif
    ssize_t bytesSent = transport.write(messageStr.c_str(), messageStr.size(), transport.context);
    if (bytesSent == -1 || static_cast<size_t>(bytesSent) != messageStr.size()) {
        throw std::runtime_error("Failed to send message.");
    }
}

std::optional<RPCPeer::Message> RPCPeer::receiveMessage() {
    static std::string unprocessedBuffer; // Buffer to store leftover data from previous reads
    char buffer[1024];

    while (true) {
        // Check if we already have a complete JSON document in the unprocessed buffer
        // First look for the end of the JSON document
        int res = 0;
        while ((res = unprocessedBuffer.find("}", res + 1)) != std::string::npos) {
            // Check if we already have a complete JSON document in this substring
            std::string jsonEnd = unprocessedBuffer.substr(0, res + 1);
            auto parsed = rfl::json::read<Message>(jsonEnd);
            if (!parsed.error().has_value()) {
                // Found a complete JSON document
#ifdef COSMO_PLUGIN_DEBUG_RPC
# ifdef __COSMOPOLITAN__
                std::cerr << "Host received: " << jsonEnd << std::endl;
# else
                std::cerr << "Plugin received: " << jsonEnd << std::endl;
# endif
#endif
                unprocessedBuffer = unprocessedBuffer.substr(res + 1);
                return parsed.value();
            }
        }

        // No complete JSON in buffer; continue reading more data
        // Read more data from the socket
        ssize_t bytesReceived = transport.read(buffer, sizeof(buffer) - 1, transport.context);

#ifdef _WIN32
        if (bytesReceived == SOCKET_ERROR) {
            return {};
        }
#else
        if (bytesReceived <= 0) {
            return {};
        }
#endif

        // Null-terminate and append to the buffer
        buffer[bytesReceived] = '\0';
        unprocessedBuffer += buffer;
    }
}

void RPCPeer::processMessages() {
    while (true) {
        std::optional<Message> maybeMessage = receiveMessage();
        if (!maybeMessage.has_value()) {
            break;
        }

        Message jsonMessage = maybeMessage.value();

        if (jsonMessage.method.has_value()) {
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
    std::string method = request.method.value();
    try {
        std::function<rfl::Generic(const std::vector<rfl::Generic>&)> handler;
        {
            std::lock_guard<std::mutex> lock(handlersMutex);
            if (handlers.find(method) == handlers.end()) {
                throw std::runtime_error("Method not found: " + method);
            }

            handler = handlers[method];
        }

        rfl::Generic response = handler(request.params.value());
        msg = constructResponse(request.id, response, "");
    } catch (const std::exception& ex) {
        std::cerr << "Error processing request: " << ex.what() << std::endl;
        msg = constructResponse(request.id, nullptr, ex.what());
    }
    sendMessage(msg);
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
    Message msg{
        .id = id,
        .result = result,
    };
    if (!error.empty()) {
        msg.error = error;
    }
    return msg;
}