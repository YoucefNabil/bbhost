// The parts of host/updater.cpp that touch nothing but their arguments:
// version order, the release signature, the checksum list. Kept apart so
// tests/updater_test.cpp links them alone.
#include "host/updater.h"

#include "monocypher-ed25519.h"

#include <cctype>
#include <sstream>

namespace updater {

bool parse_version(const std::string& v, int out[3]) {
    std::size_t i = 0;
    if (i < v.size() && (v[i] == 'v' || v[i] == 'V')) ++i;
    for (int k = 0; k < 3; ++k) {
        if (i >= v.size() || v[i] < '0' || v[i] > '9') return false;
        int n = 0;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') n = n * 10 + (v[i++] - '0');
        out[k] = n;
        if (k < 2) {
            if (i >= v.size() || v[i] != '.') return false;
            ++i;
        }
    }
    return true;
}

int fork_release(const std::string& v) {
    static const std::string kMark = "-dlaa.";
    const std::size_t at = v.find(kMark);
    if (at == std::string::npos) return 0;
    std::size_t i = at + kMark.size();
    if (i >= v.size() || v[i] < '0' || v[i] > '9') return 0;
    int n = 0;
    while (i < v.size() && v[i] >= '0' && v[i] <= '9') n = n * 10 + (v[i++] - '0');
    return n;
}

bool is_newer(const std::string& tag, const std::string& current) {
    int t[3], c[3];
    if (!parse_version(tag, t)) return false;
    if (!parse_version(current, c)) return true;  // a build that cannot say what it is takes any release
    for (int k = 0; k < 3; ++k) {
        if (t[k] != c[k]) return t[k] > c[k];
    }
    return fork_release(tag) > fork_release(current);
}

bool signature_ok(const std::string& manifest, const std::string& signature, const std::uint8_t public_key[32]) {
    if (signature.size() != 64) return false;
    return crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(signature.data()), public_key,
                                reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()) == 0;
}

std::string manifest_hash(const std::string& manifest, const std::string& name) {
    std::istringstream in(manifest);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // "<64 hex>  <name>" (sha256sum; "*<name>" in its binary mode)
        if (line.size() < 66) continue;
        std::string file = line.substr(64);
        while (!file.empty() && (file[0] == ' ' || file[0] == '*')) file.erase(0, 1);
        if (file == name) {
            std::string hex = line.substr(0, 64);
            for (char& ch : hex) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            return hex;
        }
    }
    return {};
}

std::string asset_name(const std::string& tag) {
#if defined(_WIN32)
    return "bbhost-" + tag + "-windows.exe";
#else
    return "bbhost-" + tag + "-linux";
#endif
}

}  // namespace updater
