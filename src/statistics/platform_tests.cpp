#include "platform.hpp"
#include "domain.hpp"
#include "hkjc_provider.hpp"
#include <array>
#include <fstream>
#include <stdexcept>

size_t testPlatform() {
    using namespace marksix::statistics;
    size_t checks = 0;
    const auto check = [&](bool okay) { ++checks; if (!okay) throw std::runtime_error("Portable platform fixture failed"); };
    check(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    check(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    for (const std::string valid : {"", "ASCII", "\xc2\xa2", "\xe4\xb8\xad", "\xf0\x9f\x8e\xb2", "\xf4\x8f\xbf\xbf"})
        check(platform::validUtf8(valid));
    for (const std::string invalid : {"\x80", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xe4\xb8", "\xc2x"})
        check(!platform::validUtf8(invalid));
    const auto now = platform::utcNow();
    check(now.size() == 20 && validDate(now.substr(0, 10)) && now.back() == 'Z');
    std::array<unsigned char, 16> random{};
    platform::randomBytes(random.data(), random.size());
    const auto directory = std::filesystem::temp_directory_path() / ("marksix-platform-" + sha256(std::string_view(reinterpret_cast<char*>(random.data()), random.size())));
    std::filesystem::create_directory(directory);
    const auto file = directory / std::filesystem::path(u8"backup-\u4e2d.sqlite3");
    platform::reserveFile(file);
    { std::ofstream out(file); out << "preserve"; }
    bool rejected = false;
    try { platform::reserveFile(file); } catch (const std::exception&) { rejected = true; }
    std::string saved; { std::ifstream in(file); in >> saved; }
    check(rejected && saved == "preserve");
    std::filesystem::remove_all(directory);
    validateHkjcRequest({"bet.hkjc.com", "/en/marksix/results", {}, 1024});
    validateHkjcRequest({"info.cld.hkjc.com", "/graphql/base/", "{}", 1024});
    for (const HkjcRequest& bad : {
             HkjcRequest{"evil.invalid", "/", {}, 1024},
             HkjcRequest{"bet.hkjc.com", "/static/js/main.../bad.js", {}, 1024},
             HkjcRequest{"bet.hkjc.com", "/en/marksix/results?redirect=1", {}, 1024},
             HkjcRequest{"bet.hkjc.com", "/en/marksix/results", {}, 0},
             HkjcRequest{"info.cld.hkjc.com", "/graphql/base/", "{}", 16 * 1024 * 1024 + 1}}) {
        rejected = false;
        try { validateHkjcRequest(bad); } catch (const ProviderFailure& failure) { rejected = failure.category == UpdateFailure::Schema; }
        check(rejected);
    }
    return checks;
}
