import hashlib
import json
import unittest


def parse_release(raw):
    data = json.loads(raw)
    tag = data["tag_name"]
    asset = next(item for item in data["assets"] if item["name"] == "pico-os-app.bin")
    digest = asset.get("digest", "").removeprefix("sha256:")
    checksum = next((item["browser_download_url"] for item in data["assets"]
                     if item["name"] == "pico-os-app.bin.sha256"), "")
    if not tag.startswith("v") or not asset["browser_download_url"].startswith("https://"):
        raise ValueError("invalid release metadata")
    if not digest and not checksum:
        raise ValueError("missing checksum")
    if digest and (len(digest) != 64 or any(c not in "0123456789abcdefABCDEF" for c in digest)):
        raise ValueError("invalid digest")
    return tag, asset["browser_download_url"], checksum, digest.lower()


class ReleaseMetadataTests(unittest.TestCase):
    def test_release_asset_and_sha256(self):
        payload = b"sample firmware"
        raw = json.dumps({"tag_name": "v0.2.0", "assets": [
            {"name": "pico-os-merged.bin", "browser_download_url": "https://github.com/zackerywong233/read_pico_firmware/releases/download/v0.2.0/merged", "digest": "sha256:" + "0" * 64},
            {"name": "pico-os-app.bin", "browser_download_url": "https://github.com/zackerywong233/read_pico_firmware/releases/download/v0.2.0/pico-os-app.bin", "digest": "sha256:" + hashlib.sha256(payload).hexdigest()},
        ]})
        tag, url, checksum, digest = parse_release(raw)
        self.assertEqual((tag, url, checksum), ("v0.2.0", "https://github.com/zackerywong233/read_pico_firmware/releases/download/v0.2.0/pico-os-app.bin", ""))
        self.assertEqual(digest, hashlib.sha256(payload).hexdigest())

    def test_rejects_missing_checksum(self):
        with self.assertRaises(ValueError):
            parse_release(json.dumps({"tag_name": "v1.0.0", "assets": [
                {"name": "pico-os-app.bin", "browser_download_url": "https://github.com/zackerywong233/read_pico_firmware/releases/download/v1.0.0/pico-os-app.bin", "digest": "sha256:bad"}
            ]}))

    def test_checksum_asset_fallback(self):
        raw = json.dumps({"tag_name": "v1.0.0", "assets": [
            {"name": "pico-os-app.bin", "browser_download_url": "https://github.com/zackerywong233/read_pico_firmware/releases/download/v1.0.0/pico-os-app.bin"},
            {"name": "pico-os-app.bin.sha256", "browser_download_url": "https://github.com/zackerywong233/read_pico_firmware/releases/download/v1.0.0/pico-os-app.bin.sha256"},
        ]})
        self.assertEqual(parse_release(raw)[2], "https://github.com/zackerywong233/read_pico_firmware/releases/download/v1.0.0/pico-os-app.bin.sha256")


if __name__ == "__main__":
    unittest.main()
