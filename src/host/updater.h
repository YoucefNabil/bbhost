// Updates from GitHub releases (CONTRIBUTING.md, "Releases"). A release build
// looks for a newer release at start; the F10 screen's UPDATES rows say what
// it found and install it: the release's SHA256SUMS must carry the release
// signing key's signature (Ed25519, the public half below), the executable's
// hash must be in it, and only then is bbhost swapped in place - the running
// one renamed aside (Windows cannot overwrite a running exe), the new one
// put where it was - for the next start, which "Restart now" begins.
//
// Each release carries bbhost-<tag>-windows.exe and bbhost-<tag>-linux (the
// bare executables), SHA256SUMS and SHA256SUMS.sig, besides the packages.
// update.source is the "releases/latest" API URL; a private repository's
// needs update.token_file. Nothing is sent but that token and a user agent:
// never the account's.
#pragma once

#include <cstdint>
#include <string>

namespace updater {

// This build's version (BBHOST_VERSION: "v0.1.0", "v0.1.0-3-gabc1234",
// "v0.0.0-<rev>", "+" for a changed tree).
const char* current_version();

// At start: the leftover of the last swap removed (every start), and the
// check begun on a thread of its own when update.check (or a release build)
// says so and there is a window to offer it in.
// BBHOST_UPDATE_TEST=1 checks regardless and installs what it finds, with no
// screen to click (a test of the whole path).
void start(int argc, char** argv);

// The F10 rows.
std::string status();   // "bbhost v0.1.0 - up to date", "v0.2.0 is available", ...
std::string detail();   // what is happening, or what the player can do next
bool busy();            // a check or an install is running
void check_now();       // look again (the "Check for updates" row)
void install();         // download, verify, swap (the "Install update" row)
void request_restart(); // the "Restart now" row: main's loop starts the new one and ends this one
bool update_available();
bool restart_ready();   // installed, waiting for a restart

// main's loop: true once when a restart was asked for. relaunch() then starts
// bbhost again with this run's arguments, in this run's directory.
bool take_restart_request();
bool relaunch();

// The release signing key's public half (32 bytes): what signs SHA256SUMS,
// and the official plugins (host/plugins.cpp).
const std::uint8_t* release_key();

// The official plugins of the latest release, each verified against the
// release key, into `dir` (the plugin manager's "Get official plugins"). On
// a thread of its own; plugins_status() says how it went.
void fetch_plugins(const std::string& dir);
std::string plugins_status();
bool plugins_busy();

// The pieces, for tests (tests/updater_test.cpp).
// "v1.2.3..." -> 1, 2, 3; false when it does not start that way.
bool parse_version(const std::string& v, int out[3]);
// The release tag is newer than this build: a higher major.minor.patch. A
// build past a tag ("v0.1.0-3-g...") is that tag's version, so the next
// release is newer and the same tag is not. On the same major.minor.patch, the
// DLAA fork's release number decides: "v0.2.16-dlaa.2" is newer than
// "v0.2.16-dlaa.1", which is newer than "v0.2.16".
bool is_newer(const std::string& tag, const std::string& current);
// "v0.2.16-dlaa.2..." -> 2; 0 without a "-dlaa.N".
int fork_release(const std::string& v);
// The manifest's Ed25519 signature (64 raw bytes) against `public_key` (32).
bool signature_ok(const std::string& manifest, const std::string& signature, const std::uint8_t public_key[32]);
// The lowercase hex SHA-256 the manifest (sha256sum's format) gives `name`, "" if none.
std::string manifest_hash(const std::string& manifest, const std::string& name);
// The release asset this platform installs, for a tag.
std::string asset_name(const std::string& tag);

}  // namespace updater
