#include "host/updater.h"

#include "bbhost_version.h"
#include "core/config.h"
#include "core/sha256.h"
#include "log.h"
#include "replay/json.h"

#include "monocypher-ed25519.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#if defined(BBHOST_HAVE_CURL)
#include <curl/curl.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
extern char** environ;
#endif

namespace updater {

namespace {

namespace fs = std::filesystem;

// The release signing key's public half. The private half signs each
// release's SHA256SUMS in the release workflow (the RELEASE_SIGNING_KEY
// secret); nothing it did not sign is ever installed.
constexpr std::uint8_t kReleaseKey[32] = {
    0xce, 0x83, 0x68, 0x4e, 0xe9, 0x4e, 0xc4, 0xb9, 0xc9, 0x1c, 0xf6, 0x24, 0x7a, 0x23, 0xd0, 0x8f,
    0x50, 0x1c, 0xdd, 0x7a, 0x19, 0x73, 0x3c, 0x83, 0x48, 0xca, 0x3e, 0xd6, 0xae, 0xa7, 0xad, 0x62,
};
}  // namespace

const std::uint8_t* release_key() { return kReleaseKey; }

namespace {
// The DLAA fork's builds look for updates in the fork's releases: one from
// droogie/bbhost would swap this exe for one without DLAA. The fork's releases
// carry no SHA256SUMS.sig (only the original's release key signs), so a newer
// one is announced with "get it from the release page" and never swapped in.
// The official plugins still come from the original's latest release.
constexpr const char* kDefaultSource = "https://api.github.com/repos/YoucefNabil/bbhost/releases/latest";
constexpr const char* kPluginSource = "https://api.github.com/repos/droogie/bbhost/releases/latest";
constexpr std::size_t kMaxDownload = 256u << 20;

struct Release {
    std::string tag;
    std::string exe_url, sums_url, sig_url;  // the assets' API URLs
    long long exe_size = 0;
};

std::mutex g_mu;
std::string g_status, g_detail;  // under g_mu
Release g_release;               // under g_mu: the newer release, when there is one
std::atomic<bool> g_busy{false}, g_available{false}, g_restart_ready{false}, g_restart_asked{false};
std::vector<std::string> g_argv;

void set_lines(const std::string& status, const std::string& detail) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_status = status;
    g_detail = detail;
}

fs::path exe_path() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return fs::path(buf);
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}

std::string read_token() {
    const std::string& file = config().update_token_file;
    if (file.empty()) return {};
    std::ifstream in(file, std::ios::binary);
    std::string t((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    return t;
}

struct Fetch {
    long status = 0;
    std::string body, error;
    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

#if defined(BBHOST_HAVE_CURL)
std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb, void* user) {
    auto* s = static_cast<std::string*>(user);
    if (s->size() + size * nmemb > kMaxDownload) return 0;  // ends the transfer
    s->append(ptr, size * nmemb);
    return size * nmemb;
}
#endif

// A GET to GitHub. Only the update token goes with it (and only to
// api.github.com: curl leaves a custom Authorization header behind when a
// redirect takes it to another host, which is where asset downloads go).
Fetch fetch(const std::string& url, bool binary, int timeout_ms) {
    Fetch r;
#if defined(BBHOST_HAVE_CURL)
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (!c) {
        r.error = "curl_easy_init failed";
        return r;
    }
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, binary ? "Accept: application/octet-stream" : "Accept: application/vnd.github+json");
    h = curl_slist_append(h, "X-GitHub-Api-Version: 2022-11-28");
    const std::string token = read_token();
    if (!token.empty() && url.rfind("https://api.github.com/", 0) == 0) {
        h = curl_slist_append(h, ("Authorization: Bearer " + token).c_str());
    }
    const std::string agent = std::string("bbhost/") + BBHOST_VERSION;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(c, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
#if defined(_WIN32)
    curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif
    const CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) r.error = curl_easy_strerror(rc);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
#else
    (void)url;
    (void)binary;
    (void)timeout_ms;
    r.error = "this build has no HTTP client";
#endif
    return r;
}

void check_thread() {
    set_lines(std::string("bbhost ") + BBHOST_VERSION + " - checking for updates...", "");
    const std::string source = config().update_source.empty() ? kDefaultSource : config().update_source;
    const Fetch f = fetch(source, false, 20000);
    json::Value v;
    std::string err;
    if (!f.ok() || !json::parse(f.body, v, err) || v.type != json::Value::Type::Object) {
        std::string why = !f.error.empty() ? f.error : "HTTP " + std::to_string(f.status);
        if (f.status == 404) {
            why = read_token().empty() ? "no releases visible (a private repository needs update.token_file)" : "no release found";
        }
        host_log("update: check failed: %s", why.c_str());
        set_lines(std::string("bbhost ") + BBHOST_VERSION + " - could not check for updates", why);
        g_busy.store(false);
        return;
    }
    Release rel;
    if (const json::Value* t = v.find("tag_name"); t && t->type == json::Value::Type::String) rel.tag = t->string;
    if (const json::Value* a = v.find("assets"); a && a->type == json::Value::Type::Array) {
        const std::string want = asset_name(rel.tag);
        for (const json::Value& asset : a->array) {
            const json::Value* n = asset.find("name");
            const json::Value* u = asset.find("url");
            if (!n || !u || n->type != json::Value::Type::String || u->type != json::Value::Type::String) continue;
            if (n->string == want) {
                rel.exe_url = u->string;
                if (const json::Value* s = asset.find("size"); s && s->type == json::Value::Type::Number) {
                    rel.exe_size = static_cast<long long>(s->number);
                }
            } else if (n->string == "SHA256SUMS") {
                rel.sums_url = u->string;
            } else if (n->string == "SHA256SUMS.sig") {
                rel.sig_url = u->string;
            }
        }
    }
    if (rel.tag.empty() || !is_newer(rel.tag, BBHOST_VERSION)) {
        host_log("update: %s is current (latest release %s)", BBHOST_VERSION, rel.tag.empty() ? "?" : rel.tag.c_str());
        set_lines(std::string("bbhost ") + BBHOST_VERSION + " - up to date", rel.tag.empty() ? "" : "Latest release: " + rel.tag);
        g_busy.store(false);
        return;
    }
    if (rel.exe_url.empty() || rel.sums_url.empty() || rel.sig_url.empty()) {
        host_log("update: %s is out, without a signed build for this platform", rel.tag.c_str());
        set_lines("bbhost " + rel.tag + " is out (this is " + BBHOST_VERSION + ")",
                  "It has no signed " + asset_name(rel.tag) + " to install; get it from the release page");
        g_busy.store(false);
        return;
    }
    host_log("update: %s is available (this is %s)", rel.tag.c_str(), BBHOST_VERSION);
    static const bool auto_install = std::getenv("BBHOST_UPDATE_TEST") != nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_release = rel;
        g_status = "bbhost " + rel.tag + " is available (this is " + BBHOST_VERSION + ")";
        g_detail = "Install update downloads it, checks its signature and swaps it in";
    }
    g_available.store(true);
    g_busy.store(false);
    if (auto_install) install();  // BBHOST_UPDATE_TEST: the whole path, with no screen to click
}

bool write_file(const fs::path& p, const std::string& data) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

void install_thread() {
    Release rel;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        rel = g_release;
    }
    auto fail = [&](const std::string& why) {
        host_log("update: %s not installed: %s", rel.tag.c_str(), why.c_str());
        set_lines("bbhost " + rel.tag + " was not installed", why);
        g_busy.store(false);
    };
    set_lines("Installing bbhost " + rel.tag + "...", "Checking the release's signature");
    const Fetch sums = fetch(rel.sums_url, true, 30000);
    const Fetch sig = fetch(rel.sig_url, true, 30000);
    if (!sums.ok() || !sig.ok()) return fail("could not download its checksums (" + (sums.ok() ? sig.error : sums.error) + ")");
    if (!signature_ok(sums.body, sig.body, kReleaseKey)) return fail("its checksums are not signed by the release key");
    const std::string want = manifest_hash(sums.body, asset_name(rel.tag));
    if (want.empty()) return fail("its checksums do not list " + asset_name(rel.tag));
    char size[48] = "";
    if (rel.exe_size > 0) std::snprintf(size, sizeof(size), " (%.1f MB)", static_cast<double>(rel.exe_size) / 1048576.0);
    set_lines("Installing bbhost " + rel.tag + "...", std::string("Downloading") + size);
    const Fetch exe = fetch(rel.exe_url, true, 600000);
    if (!exe.ok()) return fail("the download failed (" + (exe.error.empty() ? "HTTP " + std::to_string(exe.status) : exe.error) + ")");
    const std::string got = sha256_hex(reinterpret_cast<const std::uint8_t*>(exe.body.data()), exe.body.size());
    if (got != want) return fail("the download does not match its signed checksum");

    // In place: the new file beside the running one, the running one renamed
    // aside, the new one renamed to its name. A rename keeps the running
    // process's file open on every platform; Windows only refuses to
    // overwrite or delete it.
    const fs::path cur = exe_path();
    if (cur.empty()) return fail("cannot tell where bbhost is");
    const fs::path next = fs::path(cur).concat(".new"), old = fs::path(cur).concat(".old");
    std::error_code ec;
    if (!write_file(next, exe.body)) return fail("cannot write " + next.string());
#if !defined(_WIN32)
    struct stat st{};
    ::chmod(next.c_str(), ::stat(cur.c_str(), &st) == 0 ? (st.st_mode & 07777) : 0755);
#endif
    fs::remove(old, ec);
    fs::rename(cur, old, ec);
    if (ec) {
        fs::remove(next, ec);
        return fail("cannot move the running bbhost aside: " + ec.message());
    }
    fs::rename(next, cur, ec);
    if (ec) {
        std::error_code back;
        fs::rename(old, cur, back);
        return fail("cannot put the new bbhost in place: " + ec.message());
    }
    host_log("update: %s installed at %s (signature and checksum verified); it runs from the next start", rel.tag.c_str(),
             cur.string().c_str());
    set_lines("bbhost " + rel.tag + " is installed", "Restart now starts it (the running copy stays until you do)");
    g_available.store(false);
    g_restart_ready.store(true);
    g_busy.store(false);
}

}  // namespace

// --- official plugins ----------------------------------------------------

std::mutex g_plugins_mu;
std::string g_plugins_status;  // under g_plugins_mu
std::atomic<bool> g_plugins_busy{false};

void set_plugins_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_plugins_mu);
    g_plugins_status = s;
    host_log("plugins: %s", s.c_str());
}

#if defined(_WIN32)
constexpr const char* kPluginSuffix = "-windows.dll";
constexpr const char* kPluginExt = ".dll";
#else
constexpr const char* kPluginSuffix = "-linux.so";
constexpr const char* kPluginExt = ".so";
#endif

// Every bbhost-plugin-<name><suffix> of the latest release, with its .sig:
// verified against the release key, then written to <dir>/<name><ext>
// (+ .sig) - or <name><ext>.new when that file is in use (a loaded plugin on
// Windows), which plugins_load puts in place at the next start.
void fetch_plugins_thread(std::string dir) {
    set_plugins_status("looking for the latest official plugins...");
    const std::string source = config().update_source.empty() ? kPluginSource : config().update_source;
    const Fetch f = fetch(source, false, 20000);
    json::Value v;
    std::string err;
    if (!f.ok() || !json::parse(f.body, v, err) || v.type != json::Value::Type::Object) {
        set_plugins_status("could not read the latest release: " + (!f.error.empty() ? f.error : "HTTP " + std::to_string(f.status)) +
                           (f.status == 404 && read_token().empty() ? " (a private repository needs update.token_file)" : ""));
        g_plugins_busy.store(false);
        return;
    }
    std::string tag;
    if (const json::Value* t = v.find("tag_name"); t && t->type == json::Value::Type::String) tag = t->string;
    std::map<std::string, std::string> files;  // asset name -> API url
    if (const json::Value* a = v.find("assets"); a && a->type == json::Value::Type::Array) {
        for (const json::Value& asset : a->array) {
            const json::Value* n = asset.find("name");
            const json::Value* u = asset.find("url");
            if (n && u && n->type == json::Value::Type::String && u->type == json::Value::Type::String) files[n->string] = u->string;
        }
    }
    std::error_code ec;
    fs::create_directories(dir, ec);
    int installed = 0, failed = 0;
    std::string names;
    for (const auto& [asset, url] : files) {
        const std::string prefix = "bbhost-plugin-", suffix = kPluginSuffix;
        if (asset.rfind(prefix, 0) != 0 || asset.size() <= prefix.size() + suffix.size() ||
            asset.compare(asset.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        const std::string name = asset.substr(prefix.size(), asset.size() - prefix.size() - suffix.size());
        auto sig_it = files.find(asset + ".sig");
        if (name.find_first_of("/\\.:") != std::string::npos || sig_it == files.end()) {
            ++failed;
            continue;
        }
        set_plugins_status("downloading " + name + "...");
        const Fetch bin = fetch(url, true, 300000);
        const Fetch sig = fetch(sig_it->second, true, 30000);
        if (!bin.ok() || !sig.ok() || !signature_ok(bin.body, sig.body, kReleaseKey)) {
            host_log("plugins: %s: %s", name.c_str(), !bin.ok() || !sig.ok() ? "download failed" : "the signature does not match: not installed");
            ++failed;
            continue;
        }
        // Written beside it and renamed over it: a running plugin keeps the
        // file it was loaded from (Linux), or the rename is refused because
        // it is loaded (Windows) and the next start puts the .new in place.
        const fs::path target = fs::path(dir) / (name + kPluginExt);
        const fs::path tmp = fs::path(target.string() + ".part");
        fs::path out = target;
        std::error_code rec;
        if (!write_file(tmp, bin.body)) {
            ++failed;
            continue;
        }
        fs::rename(tmp, target, rec);
        if (rec) {
            out = fs::path(target.string() + ".new");
            fs::rename(tmp, out, rec);
            if (rec) {
                fs::remove(tmp, rec);
                ++failed;
                continue;
            }
        }
        write_file(fs::path(out.string() + ".sig"), sig.body);
        ++installed;
        names += (names.empty() ? "" : ", ") + name;
    }
    if (!installed && !failed) {
        set_plugins_status("release " + tag + " has no official plugins for this platform");
    } else {
        set_plugins_status("release " + tag + ": " + std::to_string(installed) + " installed" + (names.empty() ? "" : " (" + names + ")") +
                           (failed ? ", " + std::to_string(failed) + " failed (see the log)" : "") + " - active from the next start");
    }
    g_plugins_busy.store(false);
}

const char* current_version() { return BBHOST_VERSION; }

void fetch_plugins(const std::string& dir) {
    if (g_plugins_busy.exchange(true)) return;
    std::thread(fetch_plugins_thread, dir).detach();
}

std::string plugins_status() {
    std::lock_guard<std::mutex> lk(g_plugins_mu);
    return g_plugins_status;
}

bool plugins_busy() { return g_plugins_busy.load(); }

void start(int argc, char** argv) {
    g_argv.assign(argv, argv + argc);
    {
        // The copy the last update set aside: gone once nothing runs it.
        std::error_code ec;
        const fs::path cur = exe_path();
        if (!cur.empty()) fs::remove(fs::path(cur).concat(".old"), ec);
    }
#if defined(BBHOST_RELEASE_BUILD)
    const bool release = true;
#else
    const bool release = false;
#endif
    // BBHOST_UPDATE_TEST=1: check whatever the build and settings say, and
    // install what is found - a test of the whole path (no restart).
    const bool test = std::getenv("BBHOST_UPDATE_TEST") != nullptr;
    if (config().headless && !test) return;  // nobody to offer it to
    const int want = test ? 1 : config().update_check;
    set_lines(std::string("bbhost ") + BBHOST_VERSION, "");
    if (want == 0 || (want < 0 && !release)) {
        if (want < 0) set_lines(std::string("bbhost ") + BBHOST_VERSION, "Built from source: checks only when asked (update.check)");
        return;
    }
    g_busy.store(true);
    std::thread([] {
        // Not during the boot's busiest seconds.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        check_thread();
    }).detach();
}

std::string status() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_status;
}

std::string detail() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_detail;
}

bool busy() { return g_busy.load(); }
bool update_available() { return g_available.load(); }
bool restart_ready() { return g_restart_ready.load(); }

void check_now() {
    if (g_restart_ready.load() || g_busy.exchange(true)) return;
    std::thread(check_thread).detach();
}

void install() {
    if (!g_available.load() || g_busy.exchange(true)) return;
    std::thread(install_thread).detach();
}

void request_restart() {
    if (g_restart_ready.load()) g_restart_asked.store(true);
}

bool take_restart_request() { return g_restart_asked.exchange(false); }

bool relaunch() {
    const fs::path cur = exe_path();
#if defined(_WIN32)
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(cur.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        host_log("update: restart failed (CreateProcess error %lu)", static_cast<unsigned long>(GetLastError()));
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::vector<char*> args;
    for (std::string& a : g_argv) args.push_back(a.data());
    args.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawn(&pid, cur.c_str(), nullptr, nullptr, args.data(), environ) != 0) {
        host_log("update: restart failed (posix_spawn)");
        return false;
    }
    return true;
#endif
}

}  // namespace updater
