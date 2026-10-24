#pragma once
#include "private_channel.hpp"

namespace marksix::maintenance {
struct NativeReport {
    std::string kind; // review / outcome
    std::string approvalDigest;
    std::string text;
    bool incomplete = false;
    bool cancelled = false;
};
// Report text is chunked so a full selectable file list is never squeezed into
// the path-field limit. Its byte count, order, nonce and hash are authenticated
// over an already kernel-authenticated private channel.
void sendNativeReport(PrivateChannel& channel, const std::string& nonce, const NativeReport& report);
NativeReport receiveNativeReport(PrivateChannel& channel, const std::string& nonce);
}
