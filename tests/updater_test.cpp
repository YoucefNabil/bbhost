// The updater's checks (host/update_verify.cpp): which release is newer, the
// Ed25519 signature over SHA256SUMS (OpenSSL-made, as the release workflow
// signs), and finding a file's hash in the list. The key and signature here
// are a throwaway pair made for this test, not the release key.
#include "host/updater.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const std::uint8_t kTestKey[32] = {
    0x49, 0x68, 0x75, 0xe6, 0x47, 0x44, 0x16, 0x8f, 0xa3, 0xf9, 0xd3, 0x6e, 0x17, 0x17, 0x87, 0x4c,
    0x37, 0x00, 0xc7, 0x6c, 0x6b, 0x6b, 0x9c, 0x2f, 0x81, 0x21, 0xae, 0x0a, 0xda, 0x4e, 0x67, 0x34,
};
const unsigned char kTestSig[64] = {
    0x5e, 0xf8, 0xeb, 0x98, 0x2e, 0xf0, 0xcc, 0x5e, 0xc9, 0x3c, 0x23, 0xb1, 0xf7, 0xd9, 0x73, 0x28,
    0x6a, 0xc1, 0x39, 0x77, 0x77, 0xa1, 0xc7, 0xae, 0x10, 0xe4, 0xc4, 0x33, 0x5e, 0x17, 0xbf, 0x11,
    0x49, 0x28, 0xee, 0x69, 0x5d, 0x5c, 0x16, 0x88, 0x17, 0x37, 0x17, 0x4c, 0x82, 0xe2, 0x06, 0xda,
    0xee, 0x9b, 0x9d, 0x95, 0x23, 0x72, 0xc1, 0xb2, 0xe2, 0x72, 0x80, 0xea, 0x28, 0x24, 0x6d, 0x0a,
};
const char* const kManifest =
    "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d0a2e5b1e4c3f1b0c2d3e4f50  bbhost-v0.2.0-linux\n"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  bbhost-v0.2.0-windows.exe\n";

}  // namespace

int main() {
    using namespace updater;
    int v[3] = {};
    check(parse_version("v1.12.3", v) && v[0] == 1 && v[1] == 12 && v[2] == 3, "parse v1.12.3");
    check(parse_version("0.2.0-3-gabc1234+", v) && v[0] == 0 && v[1] == 2 && v[2] == 0, "parse past a tag");
    check(!parse_version("v1.2", v) && !parse_version("dev", v), "reject partial versions");

    check(is_newer("v0.2.0", "v0.1.0"), "minor bump is newer");
    check(is_newer("v1.0.0", "v0.9.9"), "major bump is newer");
    check(is_newer("v0.1.10", "v0.1.9"), "numeric, not lexical");
    check(!is_newer("v0.1.0", "v0.1.0"), "same is not newer");
    check(!is_newer("v0.1.0", "v0.1.0-3-gabc1234"), "a build past the tag keeps the tag");
    check(is_newer("v0.1.1", "v0.1.0-3-gabc1234"), "the next release is newer than a build past the last");
    check(!is_newer("v0.1.0", "v0.2.0"), "older is not newer");
    check(fork_release("v0.2.16-dlaa.12-3-gabc1234") == 12 && fork_release("v0.2.16-3-gabc1234") == 0, "fork release number");
    check(is_newer("v0.2.16-dlaa.2", "v0.2.16-dlaa.1"), "the next fork release is newer");
    check(is_newer("v0.2.16-dlaa.1", "v0.2.16"), "a fork release is newer than its base");
    check(!is_newer("v0.2.16-dlaa.1", "v0.2.16-dlaa.1-3-gabc1234"), "a build past a fork tag keeps the tag");
    check(!is_newer("v0.2.16-dlaa.1", "v0.2.16-dlaa.2"), "an older fork release is not newer");
    check(is_newer("v0.2.17-dlaa.1", "v0.2.16-dlaa.5"), "a newer base wins over the fork number");
    check(is_newer("v0.1.0", "v0.0.0-1cf3e08"), "an untagged build takes any release");
    check(!is_newer("nightly", "v0.1.0"), "a tag that is no version is never newer");

    const std::string manifest = kManifest;
    const std::string sig(reinterpret_cast<const char*>(kTestSig), sizeof(kTestSig));
    check(signature_ok(manifest, sig, kTestKey), "a good signature verifies");
    std::string tampered = manifest;
    tampered[0] = 'b';
    check(!signature_ok(tampered, sig, kTestKey), "a changed manifest fails");
    std::string bad_sig = sig;
    bad_sig[10] ^= 1;
    check(!signature_ok(manifest, bad_sig, kTestKey), "a changed signature fails");
    check(!signature_ok(manifest, sig.substr(0, 63), kTestKey), "a short signature fails");
    std::uint8_t other[32] = {};
    check(!signature_ok(manifest, sig, other), "another key fails");

    check(manifest_hash(manifest, "bbhost-v0.2.0-linux") == "aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d0a2e5b1e4c3f1b0c2d3e4f50",
          "finds a hash");
    check(manifest_hash(manifest, "bbhost-v0.2.0-windows.exe").size() == 64, "finds the other");
    check(manifest_hash(manifest, "bbhost-v0.2.0").empty(), "no partial name match");
    check(manifest_hash("abc  x\n", "x").empty(), "a malformed line is skipped");

    if (failures) return 1;
    std::printf("updater_test: all checks passed\n");
    return 0;
}
