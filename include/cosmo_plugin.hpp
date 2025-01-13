#ifndef COSMO_RPC_HPP
#define COSMO_RPC_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rfl/json.hpp>
#include <rfl.hpp>

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#include "LockingQueue.hpp"

#ifndef __COSMOPOLITAN__

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

extern "C" EXPORT void cosmo_rpc_initialization(long, long);

#endif // __COSMOPOLITAN__

class RPCPeer {
public:
    // Register a handler for a specific method
    template <typename ReturnType, typename... Args>
    void registerHandler(const std::string& method, std::function<ReturnType(Args...)> handler);

    // Call a method on the peer and get a response
    template <typename ReturnType, typename... Args>
    ReturnType call(const std::string& method, Args&&... args);

    // Process incoming requests and responses
    void processMessages();

#ifdef COSMO_PLUGIN_DEBUG_RPC
    // Debug output stream
    std::ostream* debugStream = &std::cerr;

    // Configure debug output
    void setDebugStream(std::ostream* stream) {
        debugStream = stream;
    }
#endif

private:
    // Serialize and deserialize RPC messages
    struct Message {
        enum class Type {
            Request,
            Response
        };

        unsigned long id;
        std::optional<std::string> method;
        std::optional<std::vector<rfl::Generic>> params;
        std::optional<rfl::Generic> result;
        std::optional<std::string> error;
    };

    // Construct an RPC request
    static Message constructRequest(unsigned long id, const std::string& method, const std::vector<rfl::Generic>& params);

    // Construct an RPC response
    static Message constructResponse(unsigned long id, const rfl::Generic& result, const std::string& error);

    // Abstract Transport implementation
    struct Transport {
        ssize_t (*write)(const void* buffer, size_t size, void* context);
        ssize_t (*read)(void* buffer, size_t size, void* context);
        void (*close)(void* context);
        void* context; // User-provided context (e.g., socket, file descriptor)
    };

    // Transport connecting to the peer
    Transport transport;

    // Handlers for incoming requests
    std::unordered_map<std::string, std::function<rfl::Generic(const std::vector<rfl::Generic>&)>> handlers;
    std::mutex handlersMutex;

    // Queue response messages
    std::unordered_map<unsigned long, LockingQueue<Message>*> responseQueue;
    std::mutex responseQueueMutex;

    // Request counter
    std::atomic<unsigned long> requestCounter;

    // Helper messages to send and receive data
    void sendMessage(const Message& message);
    std::mutex sendMutex;
    std::optional<Message> receiveMessage();
    void processRequest(const Message& request);

#ifdef __COSMOPOLITAN__
    friend class PluginHost;
#else
    friend class Plugin;
    friend void cosmo_rpc_initialization(long, long);
#endif // __COSMOPOLITAN__
    friend class MockPeer;
};

#ifdef __COSMOPOLITAN__

class PluginHost : public RPCPeer {
public:
    enum LaunchMethod {
        AUTO = 0,
        DLOPEN,
        FORK
    };

    PluginHost(const std::string& pluginPath, LaunchMethod launchMethod = AUTO);
    ~PluginHost();

    void initialize();

private:
    std::string pluginPath;
    enum LaunchMethod launchMethod;

    struct impl;
    std::unique_ptr<impl> pimpl;
};

#else

class Plugin : public RPCPeer {
public:
    ~Plugin();

private:
    Plugin();

    friend void cosmo_rpc_initialization(long, long);
};

#endif // __COSMOPOLITAN__

class MockPeer : public RPCPeer {
public:
    MockPeer();
    ~MockPeer();

private:
    struct impl;
    std::unique_ptr<impl> pimpl;
};

template <typename ReturnType, typename... Args>
void RPCPeer::registerHandler(const std::string& method, std::function<ReturnType(Args...)> handler) {
    std::lock_guard<std::mutex> lock(handlersMutex);
    handlers[method] = [handler](const std::vector<rfl::Generic>& params) -> rfl::Generic {
        // Deserialize the arguments from the JSON array
        std::tuple<Args...> args = rfl::from_generic<std::tuple<Args...>>(params).value();

        // Call the handler with the deserialized arguments
        ReturnType result = std::apply(handler, args);

        // Serialize the result into a JSON object
        return rfl::to_generic(result);
    };
}

template <typename ReturnType, typename... Args>
ReturnType RPCPeer::call(const std::string& method, Args&&... args) {
    // Generate a unique request ID
    unsigned long requestID = ++requestCounter;

    // Serialize the arguments into a JSON array
    std::vector<rfl::Generic> params;
    params.reserve(sizeof...(Args));

    // https://stackoverflow.com/a/60136761
    ([&] {
        params.push_back(rfl::to_generic(args));
    }(), ...);

    // Build the RPC request
    Message msg = constructRequest(requestID, method, params);

    // Prepare response handler
    LockingQueue<Message> queue;
    {
        std::lock_guard<std::mutex> lock(responseQueueMutex);
        responseQueue[requestID] = &queue;
    }

    sendMessage(msg);

    // Wait for the response
    Message jsonResponse;
    queue.waitAndPop(jsonResponse);

    // Remove the response handler
    {
        std::lock_guard<std::mutex> lock(responseQueueMutex);
        responseQueue.erase(requestID);
    }

    // Check for errors in the response
    if (jsonResponse.error.has_value()) {
        throw std::runtime_error("RPC error: " + jsonResponse.error.value());
    }
    if (!jsonResponse.result.has_value()) {
        throw std::runtime_error("RPC response missing result");
    }

    // Deserialize the result into the expected return type
    return rfl::from_generic<ReturnType>(jsonResponse.result.value()).value();
}

#ifndef __COSMOPOLITAN__

// Must be defined in the shared object
void plugin_initializer(Plugin* plugin);

#endif // __COSMOPOLITAN__

#endif // COSMO_RPC_HPP
