#include "cosmo_plugin.hpp"

#ifdef __COSMOPOLITAN__

#include <cerrno>
#include <cstring>
#include <iostream>
#include <random>
#include <signal.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#include <cosmo.h>
#include <libc/dlopen/dlfcn.h>
#include <libc/nt/ipc.h>
#include <libc/nt/runtime.h>

class PipeManager {
public:
    PipeManager() {
        if (IsWindows()) {
            // nt
            // ensure the pipes are created with inheritable handles
            struct NtSecurityAttributes sa = {sizeof(struct NtSecurityAttributes), nullptr, true};
            auto result = CreatePipe(&hostPipeFDs[0], &hostPipeFDs[1], &sa, 0);
            if (!result) {
                throw std::runtime_error("Failed to create host pipe: " + std::to_string(GetLastError()));
            }
            result = CreatePipe(&pluginPipeFDs[0], &pluginPipeFDs[1], &sa, 0);
            if (!result) {
                throw std::runtime_error("Failed to create plugin pipe: " + std::to_string(GetLastError()));
            }
        } else {
            // unix
            int fds[2];
            if (pipe(fds) == -1) {
                throw std::runtime_error("Failed to create host pipe: " + std::string(strerror(errno)));
            }
            hostPipeFDs[0] = fds[0];
            hostPipeFDs[1] = fds[1];
            if (pipe(fds) == -1) {
                throw std::runtime_error("Failed to create plugin pipe: " + std::string(strerror(errno)));
            }
            pluginPipeFDs[0] = fds[0];
            pluginPipeFDs[1] = fds[1];
        }
    }

    ~PipeManager() {
        closePipes();
    }

    void closePipes() {
        closing = true;

        if (IsWindows()) {
            // nt
            if (hostPipeFDs[0] != -1) {
                CloseHandle(hostPipeFDs[0]);
                hostPipeFDs[0] = -1;
            }
            if (hostPipeFDs[1] != -1) {
                CloseHandle(hostPipeFDs[1]);
                hostPipeFDs[1] = -1;
            }
            if (pluginPipeFDs[0] != -1) {
                CloseHandle(pluginPipeFDs[0]);
                pluginPipeFDs[0] = -1;
            }
            if (pluginPipeFDs[1] != -1) {
                CloseHandle(pluginPipeFDs[1]);
                pluginPipeFDs[1] = -1;
            }
        } else {
            // unix
            if (hostPipeFDs[0] != -1) {
                close((int)hostPipeFDs[0]);
                hostPipeFDs[0] = -1;
            }
            if (hostPipeFDs[1] != -1) {
                close((int)hostPipeFDs[1]);
                hostPipeFDs[1] = -1;
            }
            if (pluginPipeFDs[0] != -1) {
                close((int)pluginPipeFDs[0]);
                pluginPipeFDs[0] = -1;
            }
            if (pluginPipeFDs[1] != -1) {
                close((int)pluginPipeFDs[1]);
                pluginPipeFDs[1] = -1;
            }
        }
    }

    long getHostReadFD() const {
        return hostPipeFDs[0];
    }

    long getHostWriteFD() const {
        return pluginPipeFDs[1];
    }

    long getPluginReadFD() const {
        return pluginPipeFDs[0];
    }

    long getPluginWriteFD() const {
        return hostPipeFDs[1];
    }

    bool isClosing() const {
        return closing;
    }

private:
    // use longs to match Windows handles
    long hostPipeFDs[2] = {-1, -1};
    long pluginPipeFDs[2] = {-1, -1};

    // indicate to Windows when we want to interrupt reads
    bool closing = false;
};

static pid_t launchSubprocessWithEnv(const char* program, const char* argv[], const char* newEnvVar) {
    if (!newEnvVar) {
        pid_t pid;
        int status = posix_spawn(&pid, program, nullptr, nullptr, const_cast<char* const*>(argv), nullptr);
        if (status != 0) {
            throw std::runtime_error("Failed to spawn process: " + std::string(strerror(status)));
        }
        return pid;
    }

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
    void (*cosmo_rpc_initialization)(long, long);
    void (*cosmo_rpc_teardown)();

    int childPID = 0;

    PipeManager* pipeMgr = nullptr;

    ~impl() {
        if (pipeMgr) {
            delete pipeMgr;
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
    // Create our pipe manager
    pimpl->pipeMgr = new PipeManager();

    if (launchMethod == DLOPEN) {
        // Load the shared object
        pimpl->dynlibHandle = cosmo_dlopen(pluginPath.c_str(), RTLD_LOCAL | RTLD_NOW);
        if (!pimpl->dynlibHandle) {
            throw std::runtime_error("Failed to load shared object: " + std::string(cosmo_dlerror()));
        }

        // Get the address of the cosmo_rpc_initialization function
        pimpl->cosmo_rpc_initialization = reinterpret_cast<void(*)(long, long)>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_initialization")));
        if (!pimpl->cosmo_rpc_initialization) {
            throw std::runtime_error("Failed to find symbol: cosmo_rpc_initialization: " + std::string(cosmo_dlerror()));
        }

        // Get the address of the cosmo_rpc_teardown function
        pimpl->cosmo_rpc_teardown = reinterpret_cast<void(*)()>(cosmo_dltramp(cosmo_dlsym(pimpl->dynlibHandle, "cosmo_rpc_teardown")));
        if (!pimpl->cosmo_rpc_teardown) {
            throw std::runtime_error("Failed to find symbol: cosmo_rpc_teardown: " + std::string(cosmo_dlerror()));
        }

        // Call the cosmo_rpc_initialization function
        pimpl->cosmo_rpc_initialization(pimpl->pipeMgr->getPluginReadFD(), pimpl->pipeMgr->getPluginWriteFD());
    } else if (launchMethod == FORK) {
        // posix_spawn a child process
        int pid;
        std::string readFD = std::to_string(pimpl->pipeMgr->getPluginReadFD());
        std::string writeFD = std::to_string(pimpl->pipeMgr->getPluginWriteFD());

        const char* argv[] = {pluginPath.c_str(), readFD.c_str(), writeFD.c_str(), nullptr};

        pid = launchSubprocessWithEnv(pluginPath.c_str(), argv, nullptr);
        pimpl->childPID = pid;
    } else {
        throw std::runtime_error("Unsupported launch method.");
    }

    // Create the transport
    if (IsWindows()) {
        transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
            PipeManager* mgr = static_cast<PipeManager*>(context);
            uint32_t bytesWritten;
            return WriteFile(mgr->getHostWriteFD(), buffer, size, &bytesWritten, nullptr) ? bytesWritten : -1;
        };
        transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
            PipeManager* mgr = static_cast<PipeManager*>(context);

            // loop peek until we get data
            while (true) {
                if (mgr->isClosing()) {
                    return -1;
                }
                uint32_t bytesAvailable;
                if (!PeekNamedPipe(mgr->getHostReadFD(), nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
                    return -1;
                }
                if (bytesAvailable > 0) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            uint32_t bytesRead;
            return ReadFile(mgr->getHostReadFD(), buffer, size, &bytesRead, nullptr) ? bytesRead : -1;
        };
    } else {
        transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
            PipeManager* mgr = static_cast<PipeManager*>(context);
            return write((int)mgr->getHostWriteFD(), buffer, size);
        };
        transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
            PipeManager* mgr = static_cast<PipeManager*>(context);
            return read((int)mgr->getHostReadFD(), buffer, size);
        };
    }
    transport.close = [](void* context) {
        PipeManager* mgr = static_cast<PipeManager*>(context);
        mgr->closePipes();
    };
    transport.context = pimpl->pipeMgr;

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
#include <windows.h>
#include <fileapi.h>
#include <namedpipeapi.h>
#else
#include <unistd.h>
#include <cerrno>
#endif

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

struct IOManager {
#ifdef _WIN32
    std::pair<HANDLE, HANDLE> fds;
#else
    std::pair<int, int> fds;
#endif
    bool isClosing = false;
};

Plugin::Plugin() {}

Plugin::~Plugin() {
    if (transport.context) {
        IOManager* mgr = static_cast<IOManager*>(transport.context);
        mgr->isClosing = true;
        transport.close(transport.context);
        delete mgr;
        transport.context = nullptr;
    }
}

struct SharedObjectContext {
    Plugin *plugin;
};

SharedObjectContext *sharedObjectContext = nullptr;

extern "C" EXPORT void cosmo_rpc_initialization(long readFD, long writeFD) {
    Plugin* plugin = new Plugin();

    RPCPeer::Transport transport;
#ifdef _WIN32
    transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
        HANDLE writeFD = static_cast<IOManager*>(context)->fds.second;
        DWORD bytesWritten;
        if (!WriteFile(writeFD, buffer, size, &bytesWritten, nullptr)) {
            return -1;
        }
        return bytesWritten;
    };
    transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
        HANDLE readFD = static_cast<IOManager*>(context)->fds.first;

        // loop peek until we get data
        while (true) {
            if (static_cast<IOManager*>(context)->isClosing) {
                return -1;
            }
            DWORD bytesAvailable;
            if (!PeekNamedPipe(readFD, nullptr, 0, nullptr, &bytesAvailable, nullptr)) {
                return -1;
            }
            if (bytesAvailable > 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        DWORD bytesRead;
        if (!ReadFile(readFD, buffer, size, &bytesRead, nullptr)) {
            return -1;
        }
        return bytesRead;
    };
    transport.close = [](void* context) {
        IOManager* mgr = static_cast<IOManager*>(context);
        CloseHandle(mgr->fds.first);
        CloseHandle(mgr->fds.second);
    };
    transport.context = new IOManager{{(HANDLE)readFD, (HANDLE)writeFD}};
#else
    transport.write = [](const void* buffer, size_t size, void* context) -> ssize_t {
        int writeFD = static_cast<IOManager*>(context)->fds.second;
        return write(writeFD, buffer, size);
    };
    transport.read = [](void* buffer, size_t size, void* context) -> ssize_t {
        int readFD = static_cast<IOManager*>(context)->fds.first;
        return read(readFD, buffer, size);
    };
    transport.close = [](void* context) {
        IOManager* mgr = static_cast<IOManager*>(context);
        close(mgr->fds.first);
        close(mgr->fds.second);
    };
    transport.context = new IOManager{{(int)readFD, (int)writeFD}};
#endif
    plugin->transport = transport;

    // Pass the Plugin to the shared library initialization function
    try {
        plugin_initializer(plugin);
    } catch (const std::exception& ex) {
        std::cerr << "Error during shared library initialization: " << ex.what() << std::endl;
        delete plugin;
        exit(EXIT_FAILURE);
    }

    // Process incoming messages in a thread
    std::thread([plugin]() {
        try {
            plugin->processMessages();
            std::cout << "Client thread ended." << std::endl;
        } catch (const std::exception& ex) {
            std::cerr << "Error processing messages: " << ex.what() << std::endl;
        }
    }).detach();

    // Store the shared object context
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
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <readFD> <writeFD>" << std::endl;
        return 1;
    }

    long readFD = atol(argv[1]);
    long writeFD = atol(argv[2]);

    if (readFD <= 0 || writeFD <= 0) {
        std::cerr << "Invalid file descriptor." << std::endl;
        return 1;
    }

    cosmo_rpc_initialization(readFD, writeFD);

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
    *debugStream << "Host sending: " << messageStr << std::endl;
# else
    *debugStream << "Plugin sending: " << messageStr << std::endl;
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
                *debugStream << "Host received: " << jsonEnd << std::endl;
# else
                *debugStream << "Plugin received: " << jsonEnd << std::endl;
# endif
#endif
                unprocessedBuffer = unprocessedBuffer.substr(res + 1);
                return parsed.value();
            }
        }

        // No complete JSON in buffer; continue reading more data
        // Read more data from the transport
        ssize_t bytesReceived = transport.read(buffer, sizeof(buffer) - 1, transport.context);
        if (bytesReceived <= 0) {
            return {};
        }

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
            if (responseQueue.find(jsonMessage.id) != responseQueue.end()) {
                responseQueue[jsonMessage.id]->push(jsonMessage);
            }
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