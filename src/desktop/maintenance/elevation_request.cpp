#include "elevation_request.hpp"
#include <regex>
#include <stdexcept>

namespace marksix::maintenance {
namespace {
void validate(const ElevationRequest& request) {
    if (request.nonce.size() != 64 || request.nonce.find_first_not_of("0123456789abcdef") != std::string::npos ||
        !request.agent.pid || !request.agent.started || request.agent.uncertain || request.agent.account.empty())
        throw std::runtime_error("Invalid administrator handoff identity");
}
std::string shellField(const std::string& value) {
    std::string result = "'";
    for (const auto byte : value) result += byte == '\'' ? "'\\''" : std::string(1, byte);
    return result + '\'';
}
}
std::string macosAuthorizationScript() {
    // argv is data. AppleScript never parses connection fields as source.
    return "on run argv\nreturn do shell script (item 1 of argv) with administrator privileges\nend run\n";
}
int macosAuthorizationExitCode(int status, const std::string& output) {
    return status && output.find("(-128)") != std::string::npos ? 1223 : status;
}
std::string macosBootstrapCommand(const ElevationRequest& request, bool retry) {
    validate(request);
    static const std::regex endpoint("/private/tmp/marksix-ipc-[0-9a-f]{64}/channel");
    static const std::regex uid("(0|[1-9][0-9]{0,9})");
    if (!std::regex_match(request.endpoint, endpoint) || !std::regex_match(request.agent.account, uid))
        throw std::runtime_error("Invalid native macOS handoff coordinates");
    const auto stage = "/private/var/tmp/marksix-uninstall-" + request.nonce;
    // The only elevated executable source is the fixed installed helper, never
    // the caller's temporary image. cp -p preserves source ownership; a forged
    // user-owned replacement cannot acquire root ownership through copying.
    std::string command = R"SH(set -eu
umask 077
source='/Applications/Mark Six Studio.app/Contents/Helpers/mark-six-maintenance'
)SH";
    command += "stage=" + shellField(stage) + '\n';
    if (retry) command += R"SH([ -d "$stage" ]
[ ! -L "$stage" ]
[ "$(/usr/bin/stat -f %u "$stage")" = 0 ]
)SH";
    else command += R"SH(/bin/mkdir -m 0711 "$stage"
trap 'if [ -d "$stage" ] && [ ! -e "$stage/ledger.bin" ] && [ ! -e "$stage/.operation.lock" ]; then /bin/rm -f "$stage/mark-six-maintenance"; /bin/rmdir "$stage"; fi' EXIT
/bin/cp -p "$source" "$stage/mark-six-maintenance"
)SH";
    command += R"SH(
[ "$(/usr/bin/stat -f %u "$stage/mark-six-maintenance")" = 0 ]
mode=$(/usr/bin/stat -f %Lp "$stage/mark-six-maintenance")
[ "$((0$mode & 022))" = 0 ]
/usr/bin/codesign --verify --strict --all-architectures -R '=identifier "com.brucerry.marksixstudio.maintenance"' "$stage/mark-six-maintenance"
/bin/chmod 0755 "$stage/mark-six-maintenance"
/usr/bin/env -i PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME=/var/root USER=root LOGNAME=root LANG=en_US.UTF-8 "$stage/mark-six-maintenance" --uninstall-broker )SH";
    command += shellField(request.endpoint) + ' ' + shellField(request.nonce) + ' ' + std::to_string(request.agent.pid) +
        ' ' + std::to_string(request.agent.started) + ' ' + shellField(request.agent.account) + '\n';
    return command;
}
std::string windowsBootstrapParameters(const ElevationRequest& request) {
    validate(request);
    static const std::regex endpoint(R"(\\\\\.\\pipe\\marksix-maintenance-[0-9a-f]{64})");
    static const std::regex sid("S-1-[0-9]+(-[0-9]+){1,15}");
    if (!std::regex_match(request.endpoint, endpoint) || !std::regex_match(request.agent.account, sid) || request.agent.account.size() > 256)
        throw std::runtime_error("Invalid native Windows handoff coordinates");
    return "/M6FULLCLEANUP=1 /M6PIPE=" + request.endpoint + " /M6NONCE=" + request.nonce +
        " /M6PID=" + std::to_string(request.agent.pid) + " /M6BIRTH=" + std::to_string(request.agent.started) + " /M6ACCOUNT=" + request.agent.account;
}
}
