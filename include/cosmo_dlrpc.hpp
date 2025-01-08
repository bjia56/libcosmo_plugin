#ifndef COSMO_RPC_HPP
#define COSMO_RPC_HPP

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "rfl/json.hpp"
#include "rfl.hpp"

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

struct Transport {
    ssize_t (*write)(const void* buffer, size_t size, void* context);
    ssize_t (*read)(void* buffer, size_t size, void* context);
    void (*close)(void* context);
    void* context; // User-provided context (e.g., socket, file descriptor)
};

class Protocol {
public:

    struct Message {
        enum class Type {
            Request,
            Response
        };

        std::string id;
        std::string method;
        std::vector<rfl::Generic> params;
        rfl::Generic result;
        std::string error;
    };

    // Serialize an RPC request
    static Message serializeRequest(const std::string& id, const std::string& method, const std::vector<rfl::Generic>& params);

    // Serialize an RPC response
    static Message serializeResponse(const std::string& id, const rfl::Generic& result, const std::string& error);

    // Deserialize an RPC message (request or response)
    static Message deserialize(const std::string& message);
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
    std::unordered_map<std::string, std::function<rfl::Generic(const std::vector<rfl::Generic>&)>> handlers;

    // Mutex for thread safety
    std::mutex handlersMutex;

    // Helper methods
    void sendMessage(const std::string& message);
    std::string receiveMessage();
    void processRequest(const Protocol::Message& request);

#ifdef __COSMOPOLITAN__
    struct impl;
    std::unique_ptr<impl> pimpl;
#endif
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
    static int requestCounter = 0;
    std::string requestID = std::to_string(++requestCounter);

    // Serialize the arguments into a JSON array
    std::vector<rfl::Generic> params;
    params.resize(sizeof...(Args));

    // https://stackoverflow.com/a/60136761
    int j = 0;
    ([&] {
        params[j++] = rfl::to_generic(args);
    }(), ...);

    // Serialize the RPC request
    Protocol::Message msg = Protocol::serializeRequest(requestID, method, params);
    std::string request = rfl::json::write(msg);

    // Send the request to the peer
    sendMessage(request);

    // Wait for the response
    std::string response = receiveMessage();
    Protocol::Message jsonResponse = Protocol::deserialize(response);

    // Validate the response ID
    if (jsonResponse.id != requestID) {
        throw std::runtime_error("Mismatched response ID: expected " + requestID +
                                 ", got " + jsonResponse.id);
    }

    // Check for errors in the response
    if (!jsonResponse.error.empty()) {
        throw std::runtime_error("RPC error: " + jsonResponse.error);
    }

    // Deserialize the result into the expected return type
    return rfl::from_generic<ReturnType>(jsonResponse.result).value();
}

#ifndef __COSMOPOLITAN__

// Must be defined in the shared object
void sharedLibraryInitialization(RPCPeer* peer);

#endif // __COSMOPOLITAN__

#endif // COSMO_RPC_HPP