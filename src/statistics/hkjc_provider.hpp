#pragma once
#include "update_worker.hpp"

namespace marksix::statistics {
inline constexpr const char* HkjcSource = "hkjc-results";
inline constexpr const char* HkjcPolicy = "hkjc-public-local-v1";
inline constexpr const char* HkjcNotice =
    "Update history: fetch HKJC into your local cache. First update backfills history; later updates check missing ranges and 14 recent days. No automatic downloads.";
// Internal transport seam for deterministic tests; no user-configurable URL/plugin.
struct HkjcRequest { std::string host, path, body; size_t maximumBytes = 0; };
using HkjcTransport = std::function<std::string(const HkjcRequest&, std::stop_token)>;
std::shared_ptr<UpdateProvider> makeHkjcProvider(); // Construction performs no I/O.
std::shared_ptr<UpdateProvider> makeHkjcProviderForTest(HkjcTransport transport, std::string today);
std::string hkjcAssetPath(std::string_view page);
std::string hkjcQuery(std::string_view script);
std::vector<Result> parseHkjcResults(std::string_view bytes, const DateRange& range,
                                     std::string_view retrievedUtc, std::stop_token stop = {});
void checkHkjcHttpStatus(unsigned status);
std::string hkjcHttps(const HkjcRequest& request, std::stop_token stop);
}
