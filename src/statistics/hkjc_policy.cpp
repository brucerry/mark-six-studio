#include "hkjc_provider.hpp"

namespace marksix::statistics {
void checkHkjcHttpStatus(unsigned status) {
    if (status == 429) throw ProviderFailure(UpdateFailure::RateLimited);
    if (status == 401 || status == 403 || (status >= 300 && status < 400)) throw ProviderFailure(UpdateFailure::Denied);
    if (status != 200) throw ProviderFailure(UpdateFailure::Connection);
}
void validateHkjcRequest(const HkjcRequest& input) {
    const bool page = input.host == "bet.hkjc.com" && input.body.empty() &&
        (input.path == "/en/marksix/results" || (input.path.starts_with("/static/js/main.") && input.path.ends_with(".js") &&
         input.path.find("..") == std::string::npos &&
         input.path.find_first_not_of("/._abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == std::string::npos));
    const bool query = input.host == "info.cld.hkjc.com" && input.path == "/graphql/base/" && !input.body.empty();
    if ((!page && !query) || !input.maximumBytes || input.maximumBytes > 16 * 1024 * 1024 || input.body.size() > 16384)
        throw ProviderFailure(UpdateFailure::Schema);
}
}
