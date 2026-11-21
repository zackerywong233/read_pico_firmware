#include <assert.h>
#include <string.h>
#include "ota_policy.h"

int main(void) {
    assert(ota_update_compare_versions("v0.1.13", "v0.2.0") < 0);
    assert(ota_update_compare_versions("v1.4.0", "1.4.0") == 0);
    assert(ota_update_compare_versions("v1.4.1", "v1.4.0") > 0);
    assert(ota_update_battery_allowed(199, true));
    assert(!ota_update_battery_allowed(199, false));
    assert(ota_update_battery_allowed(200, false));
    assert(!ota_update_battery_allowed(0xffff, true));
    char digest[65];
    assert(ota_update_parse_digest("sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", digest));
    assert(strcmp(digest, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef") == 0);
    assert(!ota_update_parse_digest("sha1:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", digest));
    assert(!ota_update_parse_digest("sha256:xyz", digest));
    assert(ota_update_parse_checksum_file("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  pico-os-app.bin\n", digest));
    assert(!ota_update_parse_checksum_file("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef  other.bin\n", digest));

    char version[32], url[512], checksum_url[512];
    assert(ota_update_parse_release_redirect(
        "https://github.com/zackerywong233/read_pico_firmware/releases/tag/v0.1.24",
        version, url, checksum_url));
    assert(strcmp(version, "v0.1.24") == 0);
    assert(strcmp(url, "https://github.com/zackerywong233/read_pico_firmware/releases/download/v0.1.24/pico-os-app.bin") == 0);
    assert(strcmp(checksum_url, "https://github.com/zackerywong233/read_pico_firmware/releases/download/v0.1.24/pico-os-app.bin.sha256") == 0);
    assert(ota_update_parse_release_redirect(
        "https://github.com/zackerywong233/read_pico_firmware/releases/tag/v1.2.3-rc1?foo=1",
        version, url, checksum_url));
    assert(strcmp(version, "v1.2.3-rc1") == 0);
    assert(!ota_update_parse_release_redirect(
        "https://github.com/other/repo/releases/tag/v0.1.24", version, url, checksum_url));
    assert(!ota_update_parse_release_redirect(
        "https://github.com/zackerywong233/read_pico_firmware/releases/tag/vbad", version, url, checksum_url));
    assert(!ota_update_parse_release_redirect(
        "https://github.com/zackerywong233/read_pico_firmware/releases/tag/0.1.24", version, url, checksum_url));
    return 0;
}
