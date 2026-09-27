#include "hkjc_provider.hpp"
#include <Windows.h>
#include <winhttp.h>
#include <array>
#include <chrono>
#include <condition_variable>

namespace marksix::statistics {
namespace {
[[noreturn]] void transportError(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_SECURE_FAILURE: case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID: case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED: case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        throw ProviderFailure(UpdateFailure::Tls);
    default: throw ProviderFailure(UpdateFailure::Connection);
    }
}
void checked(BOOL result) { if (!result) transportError(GetLastError()); }
struct Handle {
    HINTERNET value;
    explicit Handle(HINTERNET handle) : value(handle) { if (!value) transportError(GetLastError()); }
    ~Handle() { if (value) WinHttpCloseHandle(value); }
    Handle(const Handle&) = delete;
};
struct AsyncRequest {
    std::mutex mutex;
    std::condition_variable condition;
    HINTERNET handle;
    bool installed = false, closed = false;
    DWORD status = 0, error = 0, count = 0;
    // Buffers live until HANDLE_CLOSING, including when cancellation interrupts a read/send.
    std::array<char, 32768> buffer{};
    std::string body;
    explicit AsyncRequest(HINTERNET request, std::string requestBody) : handle(request), body(std::move(requestBody)) {
        if (!handle) transportError(GetLastError());
    }
    ~AsyncRequest() {
        WinHttpCloseHandle(handle);
        if (installed) { std::unique_lock lock(mutex); condition.wait(lock, [&] { return closed; }); }
    }
    static void CALLBACK callback(HINTERNET, DWORD_PTR context, DWORD code, void* info, DWORD length) {
        if (!context) return;
        auto& r = *reinterpret_cast<AsyncRequest*>(context);
        std::lock_guard lock(r.mutex);
        if (code == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) r.closed = true;
        else if (code == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) r.error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
        else if (code == WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE || code == WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE || code == WINHTTP_CALLBACK_STATUS_READ_COMPLETE) {
            r.status = code; r.count = length;
        }
        r.condition.notify_all();
    }
    void install() {
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        checked(WinHttpSetOption(handle, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)));
        if (WinHttpSetStatusCallback(handle, callback, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
            transportError(GetLastError());
        installed = true;
    }
    DWORD wait(DWORD expected, std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
        std::unique_lock lock(mutex);
        if (std::chrono::steady_clock::now() >= deadline) throw ProviderFailure(UpdateFailure::Connection);
        while (!status && !error) {
            if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled);
            if (std::chrono::steady_clock::now() >= deadline) throw ProviderFailure(UpdateFailure::Connection);
            condition.wait_for(lock, std::chrono::milliseconds(50));
        }
        if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled);
        if (error) transportError(error);
        if (status != expected) throw ProviderFailure(UpdateFailure::Connection);
        status = 0; return count;
    }
};
}
void checkHkjcHttpStatus(unsigned status) {
    if (status == 429) throw ProviderFailure(UpdateFailure::RateLimited);
    if (status == 401 || status == 403 || (status >= 300 && status < 400)) throw ProviderFailure(UpdateFailure::Denied);
    if (status != 200) throw ProviderFailure(UpdateFailure::Connection);
}
std::string hkjcHttps(const HkjcRequest& input, std::stop_token stop) {
    if (stop.stop_requested()) throw ProviderFailure(UpdateFailure::Cancelled);
    const bool page = input.host == "bet.hkjc.com" && input.body.empty() &&
        (input.path == "/en/marksix/results" || (input.path.starts_with("/static/js/main.") && input.path.ends_with(".js") &&
         input.path.find_first_not_of("/._abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == std::string::npos));
    const bool query = input.host == "info.cld.hkjc.com" && input.path == "/graphql/base/" && !input.body.empty();
    if ((!page && !query) || !input.maximumBytes || input.maximumBytes > 16 * 1024 * 1024 || input.body.size() > 16384)
        throw ProviderFailure(UpdateFailure::Schema);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    Handle session(WinHttpOpen(L"MarkSixEmulator/1.0 local-history", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC));
    checked(WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000));
    const std::wstring host(input.host.begin(), input.host.end()), path(input.path.begin(), input.path.end());
    Handle connection(WinHttpConnect(session.value, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    AsyncRequest request(WinHttpOpenRequest(connection.value, input.body.empty() ? L"GET" : L"POST", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), input.body);
    // Never disable certificate/name/date/chain validation. Use OS TLS defaults;
    // additionally check revocation. No cookies, login, implicit credentials or redirects.
    DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    checked(WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled)));
    DWORD revocation = WINHTTP_ENABLE_SSL_REVOCATION;
    checked(WinHttpSetOption(request.handle, WINHTTP_OPTION_ENABLE_FEATURE, &revocation, sizeof(revocation)));
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    checked(WinHttpSetOption(request.handle, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression)));
    // The streaming cap below applies to decoded bytes, including compressed responses.
    request.install();
    checked(WinHttpSendRequest(request.handle, input.body.empty() ? L"Accept: text/html, application/javascript" : L"Content-Type: application/json\r\nAccept: application/json",
        DWORD(-1), request.body.empty() ? WINHTTP_NO_REQUEST_DATA : request.body.data(), DWORD(request.body.size()), DWORD(request.body.size()), reinterpret_cast<DWORD_PTR>(&request)));
    request.wait(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, stop, deadline);
    checked(WinHttpReceiveResponse(request.handle, nullptr));
    request.wait(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, stop, deadline);
    DWORD status = 0, size = sizeof(status);
    checked(WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX));
    checkHkjcHttpStatus(status);
    std::string output;
    for (;;) {
        checked(WinHttpReadData(request.handle, request.buffer.data(), DWORD(request.buffer.size()), nullptr));
        const auto count = request.wait(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, stop, deadline);
        if (!count) break;
        if (count > request.buffer.size() || count > input.maximumBytes - output.size()) throw ProviderFailure(UpdateFailure::Schema);
        output.append(request.buffer.data(), count);
    }
    return output;
}
}
