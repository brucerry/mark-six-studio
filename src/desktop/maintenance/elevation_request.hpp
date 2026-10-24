#pragma once
#include "native_runtime.hpp"

namespace marksix::maintenance {
struct ElevationRequest {
    std::string endpoint, nonce;
    ProcessIdentity agent;
};
// Fixed bootstrap source. Only bounded, authenticated connection coordinates
// cross this boundary; no cleanup target or client executable path is accepted.
std::string macosBootstrapCommand(const ElevationRequest& request, bool retry = false);
std::string macosAuthorizationScript();
int macosAuthorizationExitCode(int status, const std::string& output);
std::string windowsBootstrapParameters(const ElevationRequest& request);
}
