#include "native_report.hpp"
#include "wire_codec.hpp"
#include <algorithm>

namespace marksix::maintenance {
namespace {
constexpr std::size_t maximum = 16 * 1024 * 1024, chunkSize = 16384;
void validate(const std::string& nonce, const NativeReport& report) {
    if (nonce.size() != 64 || nonce.find_first_not_of("0123456789abcdef") != std::string::npos ||
        (report.kind != "review" && report.kind != "outcome") || report.text.size() > maximum ||
        (report.kind == "review" && (report.approvalDigest.size() != 64 || report.approvalDigest.find_first_not_of("0123456789abcdef") != std::string::npos)))
        throw std::runtime_error("Invalid native maintenance report");
}
}
void sendNativeReport(PrivateChannel& channel, const std::string& nonce, const NativeReport& report) {
    validate(nonce, report);
    Encoder header; header.text("M6REPORT1"); header.text(nonce); header.text(report.kind); header.text(report.approvalDigest);
    header.number(report.incomplete); header.number(report.cancelled); header.number(report.text.size()); header.text(sha256Bytes(report.text));
    channel.send(header.bytes);
    for (std::size_t offset = 0, sequence = 0; offset < report.text.size(); offset += chunkSize, ++sequence) {
        Encoder chunk; chunk.text("M6REPORTPART1"); chunk.text(nonce); chunk.number(sequence);
        chunk.text(std::string_view(report.text).substr(offset, std::min(chunkSize, report.text.size() - offset))); channel.send(chunk.bytes);
    }
}
NativeReport receiveNativeReport(PrivateChannel& channel, const std::string& nonce) {
    const auto bytes = channel.receive(); Decoder header(bytes);
    if (header.text() != "M6REPORT1" || header.text() != nonce) throw std::runtime_error("Native report protocol or nonce mismatch");
    NativeReport report; report.kind = header.text(); report.approvalDigest = header.text();
    const auto incomplete = header.number(), cancelled = header.number(), size = header.number();
    const auto digest = header.text(); header.end();
    if (incomplete > 1 || cancelled > 1 || size > maximum) throw std::runtime_error("Native report exceeds its bounds");
    report.incomplete = incomplete != 0; report.cancelled = cancelled != 0; report.text.reserve(std::size_t(size));
    std::size_t sequence = 0;
    while (report.text.size() < size) {
        const auto chunkBytes = channel.receive(); Decoder chunk(chunkBytes);
        if (chunk.text() != "M6REPORTPART1" || chunk.text() != nonce || chunk.number() != sequence++) throw std::runtime_error("Native report chunk order or nonce mismatch");
        const auto text = chunk.text(); chunk.end();
        if (text.empty() || text.size() > chunkSize || text.size() > size - report.text.size()) throw std::runtime_error("Native report chunk length mismatch");
        report.text += text;
    }
    if (sha256Bytes(report.text) != digest) throw std::runtime_error("Native report integrity mismatch");
    validate(nonce, report); return report;
}
}
