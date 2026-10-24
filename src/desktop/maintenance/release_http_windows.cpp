#include "download_inventory.hpp"
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <array>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
struct HttpHandle { HINTERNET handle; ~HttpHandle() { if (handle) WinHttpCloseHandle(handle); } };
}
std::string fetchReleasePage(unsigned page) {
    if (!page || page > 10) throw std::runtime_error("Invalid release catalogue page");
    HttpHandle session{WinHttpOpen(L"MarkSixStudio-maintenance/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session.handle || !WinHttpSetTimeouts(session.handle, 15000, 15000, 15000, 15000)) throw std::runtime_error("Release service unavailable");
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(session.handle, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) throw std::runtime_error("Verified HTTPS unavailable");
    HttpHandle connection{WinHttpConnect(session.handle, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0)};
    if (!connection.handle) throw std::runtime_error("Cannot connect to official release service");
    const auto path = L"/repos/brucerry/mark-six-studio/releases?per_page=100&page=" + std::to_wstring(page);
    HttpHandle request{WinHttpOpenRequest(connection.handle, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    if (!request.handle) throw std::runtime_error("Cannot request release identity metadata");
    DWORD disabled = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request.handle, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) throw std::runtime_error("Cannot constrain release request");
    const wchar_t headers[] = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(request.handle, headers, DWORD(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.handle, nullptr))
        throw std::runtime_error("Official release metadata could not be verified; downloads are preserved");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) || status != 200)
        throw std::runtime_error("Release service returned an unavailable or rate-limited response");
    std::string body; std::array<char, 16384> buffer{}; const auto deadline = GetTickCount64() + 30000;
    for (;;) {
        DWORD count = 0;
        if (GetTickCount64() > deadline || !WinHttpReadData(request.handle, buffer.data(), DWORD(buffer.size()), &count)) throw std::runtime_error("Release identity request timed out or was interrupted");
        if (!count) break;
        if (body.size() + count > 4 * 1024 * 1024) throw std::runtime_error("Release metadata exceeds inspection limit");
        body.append(buffer.data(), count);
    }
    return body;
}
}
