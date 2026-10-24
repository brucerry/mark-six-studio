#include "private_channel.hpp"
#include "wire_codec.hpp"
#include "durable_files.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace marksix::maintenance {
namespace {
void authenticate(const ProcessIdentity& peer, const ChannelPolicy& policy, bool elevated) {
    const auto current = processIdentity(peer.pid);
    if (peer.uncertain || !current || current->started != peer.started || current->executable != peer.executable ||
        (!current->account.empty() && current->account != peer.account) ||
        (!policy.administratorOnly && peer.account != (policy.account.empty() ? currentAccount() : policy.account)) ||
        (policy.administratorOnly && !elevated)) throw std::runtime_error("Maintenance peer account or privilege mismatch");
}
#ifdef _WIN32
struct Token { HANDLE value = nullptr; ~Token() { if (value) CloseHandle(value); } };
std::string tokenAccount(HANDLE token) {
    DWORD count = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &count);
    if (!count || count > 65536) throw std::runtime_error("Cannot inspect maintenance peer account");
    std::vector<unsigned char> bytes(count);
    if (!GetTokenInformation(token, TokenUser, bytes.data(), count, &count)) throw std::runtime_error("Cannot inspect maintenance peer account");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) throw std::runtime_error("Cannot resolve maintenance peer account");
    const auto result = pathUtf8(fs::path(sid)); LocalFree(sid); return result;
}
bool tokenElevated(HANDLE token) {
    TOKEN_ELEVATION info{}; DWORD count = sizeof(info);
    if (!GetTokenInformation(token, TokenElevation, &info, sizeof(info), &count)) throw std::runtime_error("Cannot inspect maintenance peer privilege");
    return info.TokenIsElevated != 0;
}
#endif
}
std::uint32_t channelPeer(std::intptr_t handle, bool server) {
#ifdef _WIN32
    ULONG pid = 0;
    const bool ok = server ? GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(handle), &pid) != 0 :
        GetNamedPipeServerProcessId(reinterpret_cast<HANDLE>(handle), &pid) != 0;
    if (!ok) throw std::runtime_error("Cannot authenticate local maintenance peer");
    return pid;
#else
    (void)server; pid_t pid = 0; socklen_t size = sizeof(pid);
    if (getsockopt(int(handle), SOL_LOCAL, LOCAL_PEERPID, &pid, &size) || pid <= 0)
        throw std::runtime_error("Cannot authenticate local maintenance peer");
    return std::uint32_t(pid);
#endif
}
PrivateChannel::PrivateChannel(const std::string& endpoint, const ProcessIdentity& server, ChannelPolicy policy) {
#ifdef _WIN32
    const auto name = pathFromUtf8(endpoint);
    if (!WaitNamedPipeW(name.c_str(), 10000)) throw std::runtime_error("Maintenance connection unavailable");
    const auto handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open maintenance channel");
    DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
    if (!SetNamedPipeHandleState(handle, &mode, nullptr, nullptr)) { CloseHandle(handle); throw std::runtime_error("Cannot bound maintenance channel I/O"); }
    handle_ = reinterpret_cast<std::intptr_t>(handle);
#else
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (endpoint.empty() || endpoint.size() >= sizeof(address.sun_path)) throw std::runtime_error("Invalid maintenance endpoint");
    endpoint.copy(address.sun_path, endpoint.size());
    const auto handle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (handle < 0) throw std::runtime_error("Cannot create maintenance channel");
    if (fcntl(handle, F_SETFD, FD_CLOEXEC)) { close(handle); throw std::runtime_error("Cannot protect maintenance handle inheritance"); }
    timeval timeout{30, 0}; setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    int noSignal = 1; setsockopt(handle, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
    if (connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address))) { close(handle); throw std::runtime_error("Cannot open maintenance channel"); }
    handle_ = handle;
#endif
    try {
        if (channelPeer(handle_, false) != server.pid || !sameProcessAlive(server) || server.uncertain)
            throw std::runtime_error("Maintenance server identity mismatch");
        bool elevated = false;
#ifdef _WIN32
        if (policy.administratorOnly) {
            const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, server.pid);
            if (!process) throw std::runtime_error("Cannot inspect maintenance server");
            Token token; const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token.value) != 0; CloseHandle(process);
            if (!opened) throw std::runtime_error("Cannot inspect maintenance server privilege");
            elevated = tokenElevated(token.value);
        }
#else
        elevated = server.account == "0";
        uid_t uid = 0; gid_t gid = 0;
        if (getpeereid(int(handle_), &uid, &gid) || std::to_string(uid) != server.account)
            throw std::runtime_error("Maintenance server credential mismatch");
#endif
        authenticate(server, policy, elevated); peer_ = server;
    } catch (...) {
#ifdef _WIN32
        CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
        close(int(handle_));
#endif
        handle_ = -1; throw;
    }
}
PrivateChannel::~PrivateChannel() {
#ifdef _WIN32
    if (handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    if (handle_ != -1) close(int(handle_));
#endif
}
void PrivateChannel::setTimeout(std::chrono::milliseconds timeout) {
    if (timeout.count() <= 0 || timeout > std::chrono::minutes(30)) throw std::runtime_error("Invalid maintenance I/O timeout");
#ifndef _WIN32
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    timeval value{long(seconds.count()), int((timeout - seconds).count() * 1000)};
    if (setsockopt(int(handle_), SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) || setsockopt(int(handle_), SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value)))
        throw std::runtime_error("Cannot bound native maintenance I/O");
#endif
    timeout_ = timeout;
}
void PrivateChannel::transfer(char* bytes, std::size_t count, bool sending) {
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    while (count) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Maintenance connection timed out");
#ifdef _WIN32
        const auto handle = reinterpret_cast<HANDLE>(handle_);
        if (!sending) {
            DWORD available = 0;
            if (!PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr)) throw std::runtime_error("Maintenance peer disconnected");
            if (!available) {
                if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Maintenance connection timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue;
            }
        }
        DWORD transferred = 0;
        const auto chunk = DWORD(std::min<std::size_t>(count, 65536));
        const bool ok = sending ? WriteFile(handle, bytes, chunk, &transferred, nullptr) != 0 : ReadFile(handle, bytes, chunk, &transferred, nullptr) != 0;
        if ((!ok && GetLastError() == ERROR_NO_DATA) || (ok && !transferred)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue;
        }
        if (!ok) throw std::runtime_error("Maintenance channel interrupted");
#else
        (void)deadline;
        const auto transferred = sending ? write(int(handle_), bytes, count) : read(int(handle_), bytes, count);
        if (transferred <= 0) throw std::runtime_error("Maintenance channel interrupted");
#endif
        bytes += transferred; count -= std::size_t(transferred);
    }
}
void PrivateChannel::send(std::string_view bytes) {
    if (bytes.size() > 16 * 1024 * 1024) throw std::runtime_error("Maintenance message exceeds limit");
    Encoder header; header.number(bytes.size()); transfer(header.bytes.data(), header.bytes.size(), true);
    transfer(const_cast<char*>(bytes.data()), bytes.size(), true);
}
std::string PrivateChannel::receive() {
    if (first_) { auto bytes = std::move(*first_); first_.reset(); return bytes; }
    std::string header(8, '\0'); transfer(header.data(), 8, false); Decoder in(header);
    const auto size = in.number(); if (size > 16 * 1024 * 1024) throw std::runtime_error("Maintenance message exceeds limit");
    std::string bytes(std::size_t(size), '\0'); transfer(bytes.data(), bytes.size(), false); return bytes;
}
PrivateServer::PrivateServer(ChannelPolicy policy) : policy_(std::move(policy)) {
#ifdef _WIN32
    endpoint_ = "\\\\.\\pipe\\marksix-maintenance-" + randomToken();
    const auto sid = pathFromUtf8(currentAccount()).native();
    const auto acl = L"D:P(A;;GA;;;" + sid + L")(A;;GA;;;BA)(A;;GA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        throw std::runtime_error("Cannot secure maintenance endpoint");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    const auto pipe = CreateNamedPipeW(pathFromUtf8(endpoint_).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security);
    LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot reserve maintenance endpoint");
    handle_ = reinterpret_cast<std::intptr_t>(pipe);
#else
    // The fixed short system-temp path avoids sockaddr_un truncation even when
    // the user's normal temporary folder or home has a very long name.
    directory_ = normalizePlatformPath(fs::path("/tmp")) / ("marksix-ipc-" + randomToken());
    makePrivateDirectory(directory_); endpoint_ = pathUtf8(directory_ / "channel");
    sockaddr_un address{}; address.sun_family = AF_UNIX;
    if (endpoint_.size() >= sizeof(address.sun_path)) { durableRemove(directory_); throw std::runtime_error("Maintenance endpoint is too long"); }
    endpoint_.copy(address.sun_path, endpoint_.size());
    const auto socketHandle = socket(AF_UNIX, SOCK_STREAM, 0);
    if (socketHandle < 0) { durableRemove(directory_); throw std::runtime_error("Cannot create maintenance endpoint"); }
    if (fcntl(socketHandle, F_SETFD, FD_CLOEXEC) || bind(socketHandle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || chmod(endpoint_.c_str(), 0600) || listen(socketHandle, 1)) {
        close(socketHandle); std::error_code error; fs::remove(endpoint_, error); fs::remove(directory_, error);
        throw std::runtime_error("Cannot reserve maintenance endpoint");
    }
    handle_ = socketHandle;
#endif
}
PrivateServer::~PrivateServer() {
#ifdef _WIN32
    if (handle_ != -1) CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    if (handle_ != -1) close(int(handle_));
    std::error_code error;
    if (!endpoint_.empty()) fs::remove(endpoint_, error);
    if (!directory_.empty()) fs::remove(directory_, error);
#endif
}
std::unique_ptr<PrivateChannel> PrivateServer::accept(std::chrono::milliseconds timeout) {
    if (handle_ == -1 || timeout.count() <= 0 || timeout > std::chrono::minutes(5)) throw std::runtime_error("Invalid maintenance accept state");
    std::intptr_t accepted = -1;
#ifdef _WIN32
    const auto pipe = reinterpret_cast<HANDLE>(handle_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) break;
        if (GetLastError() != ERROR_PIPE_LISTENING) throw std::runtime_error("Maintenance connection failed");
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Maintenance connection timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    accepted = handle_; handle_ = -1;
#else
    pollfd wait{int(handle_), POLLIN, 0};
    if (poll(&wait, 1, int(timeout.count())) <= 0 || !(wait.revents & POLLIN)) throw std::runtime_error("Maintenance connection timed out");
    accepted = ::accept(int(handle_), nullptr, nullptr);
    if (accepted < 0) throw std::runtime_error("Cannot accept maintenance peer");
    timeval ioTimeout{30, 0}; setsockopt(int(accepted), SOL_SOCKET, SO_RCVTIMEO, &ioTimeout, sizeof(ioTimeout));
    setsockopt(int(accepted), SOL_SOCKET, SO_SNDTIMEO, &ioTimeout, sizeof(ioTimeout));
    int noSignal = 1; setsockopt(int(accepted), SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
    auto channel = std::unique_ptr<PrivateChannel>(new PrivateChannel(accepted));
#ifndef _WIN32
    if (fcntl(int(accepted), F_SETFD, FD_CLOEXEC)) throw std::runtime_error("Cannot protect maintenance handle inheritance");
#endif
    const auto pid = channelPeer(accepted, true);
    auto peer = processIdentity(pid);
    if (!peer) throw std::runtime_error("Maintenance peer is no longer running");
    bool elevated = false;
#ifdef _WIN32
    // Identification-only SQOS on native clients prevents the UI server from
    // using an administrator's identity for filesystem operations. Read the
    // first frame before querying the pipe's last-message security context.
    channel->first_ = channel->receive();
    if (!ImpersonateNamedPipeClient(reinterpret_cast<HANDLE>(accepted))) throw std::runtime_error("Cannot identify maintenance client");
    Token token; const bool opened = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token.value) != 0;
    const bool reverted = RevertToSelf() != 0;
    if (!reverted) std::terminate(); // Never continue under an unexpected thread token.
    if (!opened) throw std::runtime_error("Cannot inspect maintenance client credentials");
    const auto account = tokenAccount(token.value);
    if (!peer->account.empty() && account != peer->account) throw std::runtime_error("Maintenance client credential mismatch");
    peer->account = account; peer->uncertain = peer->executable.empty();
    elevated = tokenElevated(token.value);
#else
    uid_t uid = 0; gid_t gid = 0;
    if (getpeereid(int(accepted), &uid, &gid) || peer->account != std::to_string(uid)) throw std::runtime_error("Maintenance client credential mismatch");
    elevated = uid == 0;
#endif
    authenticate(*peer, policy_, elevated); channel->peer_ = std::move(*peer); return channel;
}

}
