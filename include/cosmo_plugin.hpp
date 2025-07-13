#ifndef COSMO_RPC_HPP
#define COSMO_RPC_HPP

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rfl/msgpack.hpp>
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

private:
    // Serialize and deserialize RPC messages
    struct Message {
        enum class Type {
            Request,
            Response
        };

        unsigned long id;
        std::optional<std::string> method;
        std::optional<rfl::Bytestring> params;
        std::optional<rfl::Bytestring> result;
        std::optional<std::string> error;
    };

    inline rfl::Bytestring toBytestring(const std::vector<char>& data) {
        const char* dataPtr = data.data();
        size_t dataSize = data.size();
        return rfl::Bytestring(reinterpret_cast<const std::byte*>(dataPtr), reinterpret_cast<const std::byte*>(dataPtr + dataSize));
    }

    inline std::vector<char> fromBytestring(const rfl::Bytestring& bstr) {
        const std::byte* dataPtr = bstr.data();
        size_t dataSize = bstr.size();
        return std::vector<char>(reinterpret_cast<const char*>(dataPtr), reinterpret_cast<const char*>(dataPtr + dataSize));
    }

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
    std::unordered_map<std::string, std::function<std::vector<char>(const std::vector<char>&)>> handlers;
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
    handlers[method] = [handler](const std::vector<char>& params) -> std::vector<char> {
        // Deserialize the arguments from the MessagePack format
        std::tuple<Args...> args = rfl::msgpack::read<std::tuple<Args...>, rfl::NoFieldNames>(params).value();

        // Call the handler with the deserialized arguments
        ReturnType result = std::apply(handler, args);

        // Serialize the result into a MessagePack format
        return rfl::msgpack::write<rfl::NoFieldNames>(result);
    };
}

template <typename ReturnType, typename... Args>
ReturnType RPCPeer::call(const std::string& method, Args&&... args) {
    // Generate a unique request ID
    unsigned long requestID = ++requestCounter;

    // Serialize the arguments into a MessagePack format
    const std::vector<char> params = rfl::msgpack::write<rfl::NoFieldNames>(std::make_tuple(std::forward<Args>(args)...));

    // Build the RPC request
    Message msg{
        .id = requestID,
        .method = method,
        .params = toBytestring(params)
    };

    // Prepare response handler
    LockingQueue<Message> queue;
    {
        std::lock_guard<std::mutex> lock(responseQueueMutex);
        responseQueue[requestID] = &queue;
    }

    sendMessage(msg);

    // Wait for the response
    Message msgResponse;
    queue.waitAndPop(msgResponse);

    // Remove the response handler
    {
        std::lock_guard<std::mutex> lock(responseQueueMutex);
        responseQueue.erase(requestID);
    }

    // Check for errors in the response
    if (msgResponse.error.has_value()) {
        throw std::runtime_error("RPC error: " + msgResponse.error.value());
    }
    if (!msgResponse.result.has_value()) {
        throw std::runtime_error("RPC response missing result");
    }

    // Deserialize the result into the expected return type
    return rfl::msgpack::read<ReturnType, rfl::NoFieldNames>(fromBytestring(msgResponse.result.value())).value();
}

#ifndef __COSMOPOLITAN__

// Must be defined in the shared object
void plugin_initializer(Plugin* plugin);

#endif // __COSMOPOLITAN__

#endif // COSMO_RPC_HPP
