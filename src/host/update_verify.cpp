// The parts of host/updater.cpp that touch nothing but their arguments:
// version order, the release signature, what a release answer says and which
// pages may be opened. Kept apart so tests/updater_test.cpp links them alone.
#include "host/updater.h"

#include "replay/json.h"

#include "monocypher-ed25519.h"

#include <string_view>

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

bool parse_release(const std::string& body, std::string& tag, std::string& page) {
    json::Value v;
    std::string err;
    if (!json::parse(body, v, err) || v.type != json::Value::Type::Object) return false;
    const json::Value* t = v.find("tag_name");
    if (!t || t->type != json::Value::Type::String || t->string.empty()) return false;
    tag = t->string;
    const json::Value* h = v.find("html_url");
    page = h && h->type == json::Value::Type::String ? h->string : "";
    if (!release_page_ok(page)) page = "https://github.com/droogie/bbhost/releases/tag/" + tag;
    if (!release_page_ok(page)) page = "https://github.com/droogie/bbhost/releases/latest";
    return true;
}

bool release_page_ok(const std::string& url) {
    // A link and nothing more: no spaces, quotes, backslashes or control
    // characters, whatever opens it.
    constexpr std::string_view kSite = "https://github.com/";
    if (url.size() > 512 || url.compare(0, kSite.size(), kSite) != 0) return false;
    for (std::size_t i = kSite.size(); i < url.size(); ++i) {
        const char ch = url[i];
        const bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        if (!alnum && std::string_view("-._~/?#&=%+").find(ch) == std::string_view::npos) return false;
    }
    return true;
}

}  // namespace updater
