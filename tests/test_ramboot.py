"""RAM-boot host checks. Optional RAMBOOT_CPIO verifies the real build artifact."""
import gzip
import hashlib
import importlib.util
import os
import socket
from pathlib import Path
import stat
import struct
import subprocess
import zlib
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("uboot", ROOT / "tools/uboot_boot_linux.py")
uboot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uboot)


def newc_entries(data):
    """Parse newc without extracting privileged files on the host."""
    at = 0
    result = {}
    while at + 110 <= len(data):
        if data[at:at + 6] != b"070701":
            raise ValueError("invalid newc magic")
        h = [int(data[at + 6 + i * 8:at + 14 + i * 8], 16) for i in range(13)]
        name_start = at + 110
        name = data[name_start:name_start + h[11] - 1].decode()
        at = (name_start + h[11] + 3) & ~3
        content = data[at:at + h[6]]
        if len(content) != h[6]:
            raise ValueError("truncated newc member")
        at = (at + h[6] + 3) & ~3
        if name == "TRAILER!!!":
            return result
        result[name.removeprefix("./")] = (h, content)
    raise ValueError("missing newc trailer")


class BootCommands(unittest.TestCase):
    def test_usb_ramroot_loads_everything_before_boot(self):
        setup, boot = uboot.ramboot_setup()
        loads = [c for c in setup if c.startswith("fatload ")]
        self.assertEqual(len(loads), 3)
        self.assertIn("rootfs.cpio.uboot", loads[-1])
        self.assertIn("${ramdisk_addr_r} ${fdt_addr_r}", boot)
        self.assertIn("rdinit=/init", setup[-1])
        self.assertNotIn("root=", " ".join(setup))
        self.assertNotIn("saveenv", " ".join(setup))
        self.assertIn("4g.dtb", loads[1])

    def test_unit1_sd_ramroot_is_isolated_from_usb_and_unit2_dtb(self):
        setup, boot = uboot.ramboot_setup(unit=1)
        self.assertIn("setenv sddev 0", setup)
        self.assertIn("mmc dev ${sddev}", setup)
        self.assertIn("mmc rescan", setup)
        loads = [c for c in setup if c.startswith("fatload ")]
        self.assertEqual(len(loads), 3)
        self.assertTrue(all(c.startswith("fatload mmc ${sddev}:1 ") for c in loads))
        self.assertTrue(loads[1].endswith("meson-g12a-x96-max-p.dtb"))
        self.assertTrue(loads[2].endswith("rootfs.cpio.uboot"))
        self.assertIn("${ramdisk_addr_r}", boot)
        self.assertNotIn("usb", " ".join(setup))
        self.assertNotIn("4g.dtb", " ".join(setup))
        for unit in (1, 2):
            commands, _ = uboot.ramboot_setup(unit=unit)
            self.assertNotIn("root=", " ".join(commands))
            self.assertNotIn("saveenv", " ".join(commands))

    def test_network_boot_is_unit_specific_and_touches_no_boot_media(self):
        for unit, address, dtb in (
            (1, "192.168.99.109", "meson-g12a-x96-max-p.dtb"),
            (2, "192.168.99.123", "meson-g12a-x96-max-p-ephy-4g.dtb"),
        ):
            with self.subTest(unit=unit):
                setup, boot = uboot.ramboot_setup("192.168.99.80", unit=unit)
                loads = [c for c in setup if c.startswith("tftpboot ")]
                self.assertEqual(len(loads), 3)
                self.assertTrue(loads[1].endswith(dtb))
                self.assertIn(f"setenv ipaddr {address}", setup)
                self.assertIn("setenv serverip 192.168.99.80", setup)
                self.assertNotIn("usb", " ".join(setup))
                self.assertNotIn("mmc", " ".join(setup))
                self.assertIn("ramdisk_addr_r", boot)
                self.assertNotIn(":${", boot)
        setup, _ = uboot.ramboot_setup("192.168.99.80", "192.168.99.110", unit=1)
        self.assertIn("setenv ipaddr 192.168.99.110", setup)

    def test_board_identity_must_match_selected_unit(self):
        for unit, board in ((1, "g12a_u212_2g"), (2, "g12a_u211_4g")):
            self.assertTrue(uboot.ramboot_board_matches(f"aml_dt={board}\r\n", unit))
            self.assertFalse(uboot.ramboot_board_matches(f"aml_dt={board}\r\n", 3 - unit))
            self.assertFalse(uboot.ramboot_board_matches(f"aml_dt={board}_other\r\n", unit))
        self.assertFalse(uboot.ramboot_board_matches("aml_dt is not defined", 1))

    def test_invalid_unit_is_rejected(self):
        for unit in (0, 3, "1", "1; reset"):
            with self.subTest(unit=unit), self.assertRaises(ValueError):
                uboot.ramboot_setup(unit=unit)

    def test_saved_commands_fit_vendor_console(self):
        template = (ROOT / "board/x96max/ramboot-uboot.txt").read_text()
        lines = [line for line in template.splitlines() if line.startswith("setenv ")]
        self.assertEqual(len(lines), 2)
        for line in lines:
            self.assertLess(len(line.encode()), 500)
        self.assertIn("run ram_boot_setup; if usb start; then if fatload", template)
        self.assertIn("fi; fi; fi; fi'", template)

    def test_saved_unit1_commands_are_sd_only_and_fit_vendor_console(self):
        template = (ROOT / "board/x96max/ramboot-unit1-uboot.txt").read_text()
        commands = [line for line in template.splitlines() if line and not line.startswith("#")]
        definitions = [line for line in commands if line.startswith("setenv ")]
        self.assertIn(len(definitions), (2, 3))
        for line in commands:
            self.assertLess(len(line.encode()), 500)
        active = "\n".join(commands)
        self.assertEqual(active.count("fatload mmc ${sddev}:1 "), 3)
        self.assertIn("if mmc dev ${sddev}; then if mmc rescan; then if fatload", active)
        self.assertIn("fi; fi; fi; fi; fi'", active)
        self.assertIn("meson-g12a-x96-max-p.dtb", active)
        self.assertNotIn("usb", active)
        self.assertNotIn("root=", active)
        self.assertNotIn("saveenv", active)

    def test_invalid_addresses_cannot_inject_console_commands(self):
        for unit in (1, 2):
            for address in ("192.168.99.80; reset", "hostname", "::1"):
                with self.subTest(unit=unit, address=address):
                    with self.assertRaises(ValueError):
                        uboot.ramboot_setup(address, unit=unit)
                    with self.assertRaises(ValueError):
                        uboot.ramboot_setup("192.168.99.80", address, unit=unit)

    def test_stale_prompt_is_not_completion(self):
        class FakeConsole:
            fd = -1
            tail = "g12a_u211_v1#"
            def pump(self, _):
                self.tail += "still loading, no prompt"
        console = FakeConsole()
        with patch.object(uboot.os, "write"):
            self.assertFalse(uboot.run(console, "load", timeout=0))
        self.assertNotIn("g12a_u211_v1#", console.tail)

    def test_load_failure_markers(self):
        self.assertIn("No partition table", uboot.FAILURES)
        self.assertIn("Retry count exceeded", uboot.FAILURES)


class Startup(unittest.TestCase):
    def run_start(self, tuner, decrypt, card_present=False, expected_status=0):
        with tempfile.TemporaryDirectory() as d, socket.socket(socket.AF_UNIX) as card:
            base = Path(d)
            if card_present:
                # A local fixture socket only; no receiver or network service.
                card.bind(str(base / "card.sock"))
            (base / "defaults").write_text(f"PIX_TUNER={tuner}\nPIX_DECRYPT={decrypt}\n")
            text = (ROOT / "board/x96max/overlay/etc/init.d/S97pix-stream").read_text()
            for old, new in {
                "/etc/default/pix-stream-linux": str(base / "defaults"),
                "/dev/shm/torne/card.sock": str(base / "card.sock"),
                "/dev/shm/pix": str(base / "pix"),
                "/var/run/pix-stream.pid": str(base / "pid"),
            }.items():
                text = text.replace(old, new)
            # Report launcher arguments without executing hardware/service code.
            text = text.replace("start-stop-daemon", "mock_daemon")
            text = text.replace("start() {", 'sleep() { :; }\nmock_daemon() { printf "LAUNCHED %s\\n" "$*"; }\nstart() {', 1)
            result = subprocess.run(["sh", "-c", text, "startup-test", "start"], capture_output=True, text=True)
            self.assertEqual(result.returncode, expected_status, result.stderr)
            return result.stdout

    def test_dt295_internal_card_always_selects_simple(self):
        for present in (False, True):
            with self.subTest(card_present=present):
                out = self.run_start("dt295", 0, present)
                self.assertIn("LAUNCHED", out)
                self.assertIn("-x /usr/bin/pix-stream-simple", out)
                self.assertNotIn("-x /usr/bin/pix-stream-linux", out)

    def test_external_decrypt_still_waits_for_card(self):
        out = self.run_start("dt295", 1)
        self.assertIn("skipped", out)
        self.assertNotIn("LAUNCHED", out)
        self.assertIn("-x /usr/bin/pix-stream-simple", self.run_start("dt295", 1, True))

    def test_other_tuners_preserve_gate_and_select_legacy(self):
        for tuner in ("pxs1ud", "torne"):
            for decrypt in (0, 1):
                with self.subTest(tuner=tuner, decrypt=decrypt):
                    out = self.run_start(tuner, decrypt)
                    self.assertIn("skipped", out)
                    self.assertNotIn("LAUNCHED", out)
                    out = self.run_start(tuner, decrypt, True)
                    self.assertIn("LAUNCHED", out)
                    self.assertIn("-x /usr/bin/pix-stream-linux", out)
                    self.assertNotIn("-x /usr/bin/pix-stream-simple", out)

    def test_unknown_tuner_never_starts_a_daemon(self):
        self.assertNotIn("LAUNCHED", self.run_start("unknown", 0, True, expected_status=1))


@unittest.skipUnless(os.environ.get("RAMBOOT_CPIO"), "set RAMBOOT_CPIO to the built rootfs.cpio.gz")
class Archive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.unit = os.environ.get("RAMBOOT_UNIT", "2")
        if cls.unit not in ("1", "2"):
            raise ValueError("RAMBOOT_UNIT must be 1 or 2")
        cls.identity = ROOT / "work" / ("ramboot-identity-unit1" if cls.unit == "1" else "ramboot-identity")
        cls.entries = newc_entries(gzip.decompress(Path(os.environ["RAMBOOT_CPIO"]).read_bytes()))

    def test_uimage_header_and_payload(self):
        image = Path(os.environ["RAMBOOT_CPIO"])
        wrapped = image.with_name("rootfs.cpio.uboot").read_bytes()
        self.assertGreater(len(wrapped), 64)
        magic, hcrc, timestamp, size, load, entry, dcrc, os_id, arch, image_type, compression, name = struct.unpack(">7I4B32s", wrapped[:64])
        self.assertEqual(magic, 0x27051956)
        self.assertEqual(zlib.crc32(wrapped[:4] + b"\0" * 4 + wrapped[8:64]), hcrc)
        self.assertEqual(size, len(wrapped) - 64)
        self.assertEqual(zlib.crc32(wrapped[64:]), dcrc)
        self.assertEqual((os_id, arch, image_type), (5, 22, 3))  # Linux, ARM64, ramdisk
        self.assertTrue(wrapped[64:] == image.read_bytes(), "uImage payload differs from gzip CPIO")

    def test_init_and_console(self):
        h, init = self.entries["init"]
        self.assertTrue(h[1] & 0o111)
        self.assertIn(b"devtmpfs", init)
        self.assertIn(b"exec /sbin/init", init)
        console, _ = self.entries["dev/console"]
        self.assertTrue(stat.S_ISCHR(console[1]))
        self.assertEqual(console[9:11], [5, 1])

    def test_ssh_keys_and_root_ownership(self):
        h, key = self.entries["root/.ssh/authorized_keys"]
        self.assertEqual(h[2:4], [0, 0])
        self.assertEqual(stat.S_IMODE(h[1]), 0o600)
        self.assertIn(b"ssh-", key)
        self.assertEqual(stat.S_IMODE(self.entries["root/.ssh"][0][1]), 0o700)
        self.assertEqual(self.entries["root/.ssh"][0][2:4], [0, 0])

    def test_stable_private_host_identity(self):
        h, key = self.entries["etc/dropbear/dropbear_ed25519_host_key"]
        self.assertEqual(h[2:4], [0, 0])
        self.assertEqual(stat.S_IMODE(h[1]), 0o600)
        self.assertTrue(key)
        self.assertTrue(key == (self.identity / "dropbear_ed25519_host_key").read_bytes(),
                        "archive host key differs from selected private identity")

    def test_optional_private_inputs_match_without_disclosing_contents(self):
        for name, mode in (("etc/default/bcas-linux", 0o600),
                           ("etc/network/interfaces", 0o644),
                           ("root/.ssh/authorized_keys", 0o600)):
            source = self.identity / name
            with self.subTest(path=name):
                if self.unit == "1":
                    self.assertTrue(source.is_file(), f"unit 1 private input is missing: {name}")
                if source.is_file():
                    h, content = self.entries[name]
                    self.assertTrue(content == source.read_bytes(), f"archive private input differs: {name}")
                    self.assertEqual(h[2:4], [0, 0])
                    self.assertEqual(stat.S_IMODE(h[1]), mode)

    def test_preserved_logo_cache_matches_private_inputs_without_dumping_bytes(self):
        source = self.identity / "var/lib/pix/logos"
        if self.unit != "1" and not source.exists():
            self.skipTest("selected identity has no optional logo cache")
        self.assertFalse(source.is_symlink())
        self.assertTrue(source.is_dir(), "unit 1 logo cache input is missing")
        files = sorted(source.iterdir())
        if self.unit == "1":
            self.assertEqual(len(files), 7, "unit 1 must retain all seven captured cache files")
        prefix = "var/lib/pix/logos"
        expected_names = {f"{prefix}/{path.name}" for path in files}
        self.assertEqual({name for name in self.entries if name.startswith(prefix + "/")}, expected_names)
        for directory in ("var/lib", prefix):
            h, _ = self.entries[directory]
            self.assertTrue(stat.S_ISDIR(h[1]), f"archive directory must not be a symlink: {directory}")
            self.assertEqual(h[2:4], [0, 0])
        self.assertEqual(stat.S_IMODE(self.entries[prefix][0][1]), 0o700)
        for path in files:
            self.assertTrue(stat.S_ISREG(path.lstat().st_mode), "logo input must be regular, not a link")
            self.assertIn(path.suffix, (".json", ".png"))
            h, content = self.entries[f"{prefix}/{path.name}"]
            self.assertTrue(stat.S_ISREG(h[1]), "archive logo member must be regular")
            self.assertEqual(h[2:4], [0, 0])
            self.assertEqual(stat.S_IMODE(h[1]), 0o600)
            self.assertTrue(content == path.read_bytes(), "archive logo cache bytes differ from selected identity")

    def test_exported_artifacts_and_manifest(self):
        directory = Path(os.environ["RAMBOOT_CPIO"]).resolve().parent
        files = {"Image", "meson-g12a-x96-max-p.dtb", "meson-g12a-x96-max-p-ephy-4g.dtb",
                 "rootfs.cpio.gz", "rootfs.cpio.uboot", "ramboot.img"}
        template = "ramboot-unit1-uboot.txt" if self.unit == "1" else "ramboot-uboot.txt"
        for name in files | {template, "buildroot.config", "linux.config", "toolchain.txt"}:
            self.assertTrue((directory / name).is_file(), f"missing exported file: {name}")
        manifest = {}
        for line in (directory / "SHA256SUMS").read_text().splitlines():
            digest, name = line.split(maxsplit=1)
            manifest[name.removeprefix("*")] = digest
        self.assertEqual(set(manifest), files)
        for name, expected in manifest.items():
            self.assertEqual(hashlib.sha256((directory / name).read_bytes()).hexdigest(), expected, name)
        config = (directory / "buildroot.config").read_text()
        hostname = "x96max-linux" if self.unit == "1" else "x96max-p2"
        overlay = "ramboot-unit1-overlay" if self.unit == "1" else "ramboot-overlay"
        self.assertIn(f'BR2_TARGET_GENERIC_HOSTNAME="{hostname}"', config)
        overlay_line = next(line for line in config.splitlines() if line.startswith("BR2_ROOTFS_OVERLAY="))
        self.assertIn(f"/board/x96max/{overlay}", overlay_line)
        self.assertEqual(len(overlay_line.split()), 2)

    def test_installed_launchers_match_current_source(self):
        for name in ("etc/init.d/S96bcas", "etc/init.d/S97pix-stream", "etc/init.d/S98pix-logo",
                     "usr/bin/pix-stream-linux", "usr/bin/pix-stream-simple", "usr/bin/pix-usb-reset-common",
                     "usr/bin/pix-logo-fetch", "usr/bin/pix-logo-weekly"):
            expected = (ROOT / "board/x96max/overlay" / name).read_bytes()
            self.assertTrue(self.entries[name][1] == expected, f"stale launcher in archive: {name}")

    def test_services_and_unit_profile(self):
        for name in ("usr/bin/stb_streamer", "usr/bin/eit_watch", "usr/bin/pix-ts-legacy", "usr/bin/ffmpeg", "usr/bin/pix-usb-reset-common"):
            self.assertIn(name, self.entries)
        defaults = self.entries["etc/default/pix-stream-linux"][1]
        if self.unit == "1":
            for setting in (b"PIX_TUNER=pxs1ud", b"PIX_SYSTEM=terrestrial", b"PIX_CHANNEL=17",
                            b"PIX_FEATURES=1", b"PIX_DISPLAY=X96Max-P", b"PIX_BITRATE=6000k"):
                self.assertIn(setting, defaults)
            self.assertNotIn(b"PIX_TUNER=dt295", defaults)
        else:
            self.assertIn(b"PIX_TUNER=dt295", defaults)
            self.assertIn(b"PIX_DECRYPT=0", defaults)
        fstab = self.entries["etc/fstab"][1]
        self.assertNotIn(b"swap", fstab)
        for line in fstab.splitlines():
            if line.startswith(b"/dev/"):
                self.assertIn(b"noauto", line)


if __name__ == "__main__":
    unittest.main()
