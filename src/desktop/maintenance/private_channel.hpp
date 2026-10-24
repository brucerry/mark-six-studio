#pragma once
#include "native_runtime.hpp"
#include <chrono>

namespace marksix::maintenance {
std::uint32_t channelPeer(std::intptr_t handle, bool server);
struct ChannelPolicy {
    std::string account; // Empty means the current native account.
    bool administratorOnly = false;
};
class PrivateServer;
class PrivateChannel {
public:
    PrivateChannel(const std::string& endpoint, const ProcessIdentity& server,
        ChannelPolicy policy = {});
    ~PrivateChannel();
    PrivateChannel(const PrivateChannel&) = delete;
    const ProcessIdentity& peer() const { return peer_; }
    void setTimeout(std::chrono::milliseconds timeout);
    std::string receive();
    void send(std::string_view bytes);
private:
    friend class PrivateServer;
    explicit PrivateChannel(std::intptr_t handle) : handle_(handle) {}
    void transfer(char* bytes, std::size_t count, bool sending);
    std::intptr_t handle_ = -1;
    std::chrono::milliseconds timeout_ = std::chrono::seconds(30);
    ProcessIdentity peer_;
    std::optional<std::string> first_;
};
// Native endpoint owned by this process. It accepts a local peer only after
// kernel process/account authentication; no message is returned before that.
class PrivateServer {
public:
    explicit PrivateServer(ChannelPolicy policy = {});
    ~PrivateServer();
    PrivateServer(const PrivateServer&) = delete;
    const std::string& endpoint() const { return endpoint_; }
    std::unique_ptr<PrivateChannel> accept(std::chrono::milliseconds timeout = std::chrono::seconds(30));
private:
    ChannelPolicy policy_;
    std::string endpoint_;
    std::intptr_t handle_ = -1;
    fs::path directory_;
};
}
