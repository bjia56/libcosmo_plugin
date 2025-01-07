#ifndef COSMO_RPC_HPP
#define COSMO_RPC_HPP

#include <mutex>

#include <nlohmann/json.hpp>

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
    static std::string serializeResponse(const std::string& id, const nlohmann::json& result, const std::string& error = "");

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
    handlers[method] = [handler](const nlohmann::json& params) {
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
extern "C" void sharedLibraryInitialization(RPCPeer *peer);

#endif // __COSMOPOLITAN__

#endif // COSMO_RPC_HPP