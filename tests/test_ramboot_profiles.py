"""Offline fixtures for unit selection and CPIO-only identity injection.

All build/identity state lives in TemporaryDirectory. Docker, downloads, make,
key generation and ownership operations are mocks, not host/hardware changes.
Real CPIO uid/gid and private-byte checks remain in test_ramboot.Archive.
"""
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
UNIT3_EXTRA_DTBS = ("meson-g12a-x96-max-p-nousb.dtb", "meson-g12a-x96-max-p-usb.dtb")
DTBS = ("meson-g12a-x96-max-p.dtb", "meson-g12a-x96-max-p-ephy-4g.dtb", *UNIT3_EXTRA_DTBS)
RAM_FILES = ("Image", *DTBS, "rootfs.cpio.gz", "rootfs.cpio.uboot", "ramboot.img", "SHA256SUMS")


def executable(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    path.chmod(0o755)


def fixture_env(base, **settings):
    env = os.environ.copy()
    for name in ("X96MAX_PROFILE", "X96MAX_UNIT", "RAMBOOT_CPIO", "RAMBOOT_UNIT"):
        env.pop(name, None)
    env.update(PATH=f"{base / 'bin'}:{env['PATH']}", FIXTURE_LOG=str(base / "calls"), **settings)
    return env


class BuildEntry(unittest.TestCase):
    def run_entry(self, base, **settings):
        script = base / "scripts/build-linux.sh"
        script.parent.mkdir(exist_ok=True)
        shutil.copyfile(ROOT / "scripts/build-linux.sh", script)
        for command in ("docker", "colima"):
            executable(base / "bin" / command,
                       '#!/bin/sh\nprintf "%s %s\\n" "${0##*/}" "$*" >> "$FIXTURE_LOG"\n')
        return subprocess.run(["bash", str(script)], env=fixture_env(base, **settings),
                              capture_output=True, text=True)

    def test_invalid_settings_fail_before_any_docker_or_directory_change(self):
        for settings in ({"X96MAX_UNIT": "4"}, {"X96MAX_UNIT": "1; reset"},
                         {"X96MAX_PROFILE": "unknown"},
                         {"X96MAX_PROFILE": "ramboot", "X96MAX_UNIT": "0"}):
            with self.subTest(settings=settings), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                result = self.run_entry(base, **settings)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unknown X96MAX_", result.stderr)
                for name in ("calls", "work", "output"):
                    self.assertFalse((base / name).exists())

    def test_ramboot_unit_exports_and_identities_are_separate(self):
        for requested, unit, output, identity in (
            (None, "2", "ramboot", "ramboot-identity"),
            ("2", "2", "ramboot", "ramboot-identity"),
            ("1", "1", "ramboot-unit1", "ramboot-identity-unit1"),
            ("3", "3", "ramboot-unit3", "ramboot-identity-unit3"),
        ):
            with self.subTest(unit=requested), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                settings = {"X96MAX_PROFILE": "ramboot"}
                if requested is not None:
                    settings["X96MAX_UNIT"] = requested
                result = self.run_entry(base, **settings)
                self.assertEqual(result.returncode, 0, result.stderr)
                calls = (base / "calls").read_text()
                self.assertIn(f"{base}/output/{output}:/export", calls)
                self.assertIn(f"{base}/work/{identity}:/identity", calls)
                self.assertIn(f"-e X96MAX_UNIT={unit}", calls)
                self.assertEqual(stat.S_IMODE((base / "output" / output).stat().st_mode), 0o700)
                self.assertEqual(stat.S_IMODE((base / "work" / identity).stat().st_mode), 0o700)
                self.assertEqual({p.name for p in (base / "work").iterdir()}, {"build", identity})

    def test_disk_keeps_default_export_and_never_mounts_identity(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            result = self.run_entry(base)
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = (base / "calls").read_text()
            self.assertIn(f"{base}/output:/export", calls)
            self.assertIn("-e X96MAX_PROFILE=disk", calls)
            self.assertNotIn(":/identity", calls)
            self.assertEqual({p.name for p in (base / "work").iterdir()}, {"build"})


class ContainerBuild(unittest.TestCase):
    def prepare(self, base, diagnostic_dtb=False):
        build = base / "build"
        project = base / "project"
        output = build / "output"
        images = output / "images"
        images.mkdir(parents=True)
        (base / "export").mkdir()
        version = "2026.08.1"
        (build / f"buildroot-{version}/dl/linux").mkdir(parents=True)
        (build / f"buildroot-{version}/dl/linux/linux-6.18.62.tar.xz").touch()
        executable(build / f"buildroot-{version}/support/kconfig/merge_config.sh",
                   '#!/bin/sh\nprintf "merge %s\\n" "$*" >> "$FIXTURE_LOG"\n')
        executable(base / "bin/make", '#!/bin/sh\nprintf "make %s\\n" "$*" >> "$FIXTURE_LOG"\n')
        executable(base / "bin/sha256sum", '#!/bin/sh\nexit 0\n')
        for command in ("curl", "docker", "colima"):
            executable(base / "bin" / command, '#!/bin/sh\necho "unexpected external operation" >&2\nexit 99\n')
        for name in (*RAM_FILES, "rootfs.ext4", "sdcard.img"):
            (images / name).write_text(f"fixture {name}\n")
        # Units 1 and 2 do not build the dwc3-disabled diagnostic DTB.
        if not diagnostic_dtb:
            for name in UNIT3_EXTRA_DTBS:
                (images / name).unlink()
        (output / ".config").write_text("fixture buildroot configuration\n")
        (output / "build/linux-6.18.62").mkdir(parents=True)
        (output / "build/linux-6.18.62/.config").write_text("fixture kernel configuration\n")
        executable(output / "host/bin/aarch64-buildroot-linux-gnu-gcc", '#!/bin/sh\necho fixture-gcc\n')
        board = project / "board/x96max"
        board.mkdir(parents=True)
        for name in ("ramboot-unit1-uboot.txt", "ramboot-uboot.txt", "ramboot-unit3-uboot.txt"):
            (board / name).write_text(f"fixture {name}\n")
        text = (ROOT / "scripts/build-in-container.sh").read_text()
        # Relocate only absolute container paths; every executable build action is mocked.
        text = re.sub(r'(?<![\w/])/(build|project|export)(?=/|\s|")',
                      lambda match: str(base / match[1]), text)
        script = base / "container.sh"
        script.write_text(text)
        return script

    def test_matching_unit_fragment_and_boot_template_with_both_dtbs(self):
        for requested, unit in ((None, "2"), ("1", "1"), ("2", "2"), ("3", "3")):
            with self.subTest(unit=requested), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                script = self.prepare(base, diagnostic_dtb=(unit == "3"))
                settings = {"X96MAX_PROFILE": "ramboot"}
                if requested is not None:
                    settings["X96MAX_UNIT"] = requested
                result = subprocess.run(["bash", str(script)], env=fixture_env(base, **settings),
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                calls = (base / "calls").read_text()
                merge = next(line for line in calls.splitlines() if line.startswith("merge "))
                self.assertTrue(merge.endswith(f"ramboot.fragment {base}/project/board/x96max/ramboot-unit{unit}.fragment"))
                for other in {1, 2, 3} - {int(unit)}:
                    self.assertNotIn(f"ramboot-unit{other}.fragment", merge)
                template = {"1": "ramboot-unit1-uboot.txt",
                            "3": "ramboot-unit3-uboot.txt"}.get(unit, "ramboot-uboot.txt")
                expected = {*RAM_FILES, template, "buildroot.config", "linux.config", "toolchain.txt"}
                if unit != "3":
                    # The dwc3-disabled diagnostic DTB exists only for unit 3.
                    expected.difference_update(UNIT3_EXTRA_DTBS)
                self.assertEqual({p.name for p in (base / "export").iterdir()}, expected)

    def test_disk_export_and_no_ramboot_merge(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script = self.prepare(base)
            result = subprocess.run(["bash", str(script)], env=fixture_env(base), capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("merge ", (base / "calls").read_text())
            self.assertEqual({p.name for p in (base / "export").iterdir()},
                             {"Image", DTBS[0], "rootfs.ext4", "sdcard.img", "SHA256SUMS",
                              "buildroot.config", "linux.config", "toolchain.txt"})

    def test_direct_container_validation_precedes_build_access(self):
        for settings in ({"X96MAX_UNIT": "4"}, {"X96MAX_PROFILE": "unknown"}):
            with self.subTest(settings=settings), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                result = subprocess.run(["bash", str(ROOT / "scripts/build-in-container.sh")],
                                        env=fixture_env(base, **settings), capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unknown X96MAX_", result.stderr)
                self.assertNotIn("cd:", result.stderr)


class ProfileContracts(unittest.TestCase):
    def test_common_fragment_has_no_unit_identity_or_overlay(self):
        common = (ROOT / "board/x96max/ramboot.fragment").read_text()
        # INTREE_DTS_NAME belongs to the unit fragment: merge_config.sh applies
        # the unit file last, so a value here would drop unit-specific DTBs.
        for field in ("BR2_TARGET_GENERIC_HOSTNAME", "BR2_TARGET_GENERIC_ISSUE",
                      "BR2_ROOTFS_OVERLAY", "BR2_LINUX_KERNEL_INTREE_DTS_NAME="):
            self.assertNotIn(field, common)
        for unit, overlay in ((1, "ramboot-unit1-overlay"), (2, "ramboot-overlay"),
                              (3, "ramboot-unit3-overlay")):
            text = (ROOT / f"board/x96max/ramboot-unit{unit}.fragment").read_text()
            line = next(line for line in text.splitlines() if line.startswith("BR2_ROOTFS_OVERLAY="))
            self.assertEqual(line.split('="', 1)[1].rstrip('"').split(),
                             ["$(BR2_EXTERNAL_X96MAX_PATH)/board/x96max/overlay",
                              f"$(BR2_EXTERNAL_X96MAX_PATH)/board/x96max/{overlay}"])
            self.assertNotIn("/identity", text)

    def test_unit1_retains_public_stream_and_cm_settings_without_dt295_fields(self):
        defaults = (ROOT / "board/x96max/ramboot-unit1-overlay/etc/default/pix-stream-linux").read_text()
        values = dict(line.split("=", 1) for line in defaults.splitlines() if line.startswith("PIX_"))
        self.assertEqual(values, {
            "PIX_TUNER": "pxs1ud", "PIX_SYSTEM": "terrestrial", "PIX_CHANNEL": "17",
            "PIX_DISPLAY": "X96Max-P", "PIX_TUNER_DEV": "", "PIX_FIRMWARE": "/lib/firmware/isdbt_rio.inp",
            "PIX_CARD_SOCKET": "/dev/shm/torne/card.sock", "PIX_PORT": "40774",
            "PIX_ALLOWED_IP": "192.168.99.80,192.168.99.52", "PIX_SPOOL_MB": "192",
            "PIX_BITRATE": "6000k", "PIX_AUDIO_BITRATE": "192k", "PIX_GOP": "60",
            "PIX_DEINTERLACE": "1", "PIX_START_DELAY": "4", "PIX_FEATURES": "1",
            "PIX_FEATURE_MAX_MB": "384", "PIX_FEATURE_STEP": "3", "PIX_FEATURE_FAILURES": "3",
            "PIX_LOGO_WEEKLY": "1", "PIX_LOGO_WEEKLY_DAY": "1", "PIX_LOGO_WEEKLY_HOUR": "4",
            "PIX_LOGO_WEEKLY_WAIT": "900",
        })

    def test_unit3_uses_dt295_on_the_sbs_physical_channel(self):
        fragment = (ROOT / "board/x96max/ramboot-unit3.fragment").read_text()
        self.assertIn('BR2_TARGET_GENERIC_HOSTNAME="x96max-p3"', fragment)
        self.assertIn("ramboot-unit3-overlay", fragment)
        self.assertNotIn("/identity", fragment)
        text = (ROOT / "board/x96max/ramboot-unit3-overlay/etc/default/pix-stream-linux").read_text()
        values = dict(line.split("=", 1) for line in text.splitlines() if line.startswith("PIX_"))
        self.assertEqual(values["PIX_TUNER"], "dt295")
        self.assertEqual(values["PIX_SYSTEM"], "terrestrial")
        # SBS (静岡放送) is physical 15 on 日本平; 17 would be テレビ静岡, which
        # unit 1 already records. Physical channels 13..62 are what pix-ts
        # accepts, so the remote key (6) is not a substitute.
        self.assertEqual(values["PIX_CHANNEL"], "15")
        self.assertEqual(values["PIX_DISPLAY"], "X96Max-P3")
        # DT295 decrypts on its internal card: no external reader, no torne-b25.
        self.assertEqual(values["PIX_DECRYPT"], "0")
        # Satellite-only options must be absent entirely: the launchers default
        # them to "--slot 0" plus a BS/CS PID list, and pix-ts rejects that for
        # terrestrial capture (exit 2, usage). An empty value is not enough.
        self.assertNotIn("PIX_SAT_SLOT", values)
        self.assertNotIn("PIX_PIDS", values)
        self.assertNotIn("PIX_CARD_SOCKET", values)
        # Unit 2 keeps its satellite slot/PID list from its own defaults file.
        unit2_defaults = (ROOT / "board/x96max/ramboot-overlay/etc/default/pix-stream-linux").read_text()
        self.assertIn("PIX_SAT_SLOT=0", unit2_defaults)
        # The other two units keep their own channel and tuner.
        unit1 = (ROOT / "board/x96max/ramboot-unit1-overlay/etc/default/pix-stream-linux").read_text()
        unit2 = (ROOT / "board/x96max/ramboot-overlay/etc/default/pix-stream-linux").read_text()
        self.assertIn("PIX_CHANNEL=17", unit1)
        self.assertIn("PIX_CHANNEL=9", unit2)

    def test_usb_boot_rail_preserves_firmware_enabled_state(self):
        common = (ROOT / 'board/x96max/dts/amlogic/meson-g12a-x96-max-p.dts').read_text()
        rail = re.search(r'&vcc_5v\s*\{([^}]*)\}', common)
        self.assertIsNotNone(rail)
        self.assertIn('regulator-boot-on;', rail[1])
        self.assertNotIn('regulator-always-on;', rail[1])
        # Both unit variants inherit this rail policy, not just a generated DTB.
        external = (ROOT / 'board/x96max/dts/amlogic/meson-g12a-x96-max-p-ephy.dts').read_text()
        unit2 = (ROOT / 'board/x96max/dts/amlogic/meson-g12a-x96-max-p-ephy-4g.dts').read_text()
        self.assertIn('#include "meson-g12a-x96-max-p.dts"', external)
        self.assertIn('#include "meson-g12a-x96-max-p-ephy.dts"', unit2)

    def test_genimage_template_is_unit_neutral_and_post_image_adds_extras(self):
        image = (ROOT / "board/x96max/ramboot-genimage.cfg").read_text()
        post = (ROOT / "board/x96max/ramboot-post-image.sh").read_text()
        # The shared template carries only the DTBs every unit builds; a unit's
        # extra DTBs are appended by the post-image script when they exist, so
        # units 1 and 2 keep the image they had.
        for dtb in DTBS[:2]:
            self.assertIn(f'"{dtb}"', image)
        for dtb in UNIT3_EXTRA_DTBS:
            self.assertNotIn(f'"{dtb}"', image)
            self.assertIn(dtb, post)
        self.assertNotIn("rootfs.ext4", image)


class Finalize(unittest.TestCase):
    def prepare(self, base, private=True, key=True):
        build = base / "build"
        stage = build / "buildroot-fs/cpio/target"
        common = base / "target"
        identity = base / "identity"
        for directory in ("etc/default", "etc/network", "root/.ssh", "lib", "usr/bin"):
            (stage / directory).mkdir(parents=True, exist_ok=True)
        (common / "etc/dropbear").mkdir(parents=True)
        (common / "etc/dropbear/keep").write_text("shared target must stay unchanged\n")
        (stage / "etc/dropbear").symlink_to(common / "etc/dropbear", target_is_directory=True)
        (stage / "etc/inittab").write_text("::sysinit:/bin/mount -o remount,rw /\n::sysinit:/bin/true\n")
        (stage / "etc/fstab").write_text("/dev/root / ext4 defaults 0 1\nproc /proc proc defaults 0 0\n")
        identity.mkdir()
        if key:
            (identity / "dropbear_ed25519_host_key").write_bytes(b"fixture existing host key\x00")
        if private:
            for path, content in (("etc/default/bcas-linux", b"fixture private card settings\n"),
                                  ("etc/network/interfaces", b"fixture private interfaces\n"),
                                  ("root/.ssh/authorized_keys", b"ssh-ed25519 fixture-public-key\n"),
                                  ("etc/unlisted", b"must not be copied\n")):
                target = identity / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(content)
        text = (ROOT / "board/x96max/ramboot-finalize.sh").read_text().replace("/identity", str(identity))
        script = base / "finalize.sh"
        script.write_text(text)
        executable(base / "bin/chown", '#!/bin/sh\nprintf "chown %s\\n" "$*" >> "$FIXTURE_LOG"\n')
        # macOS sed lacks GNU -i syntax; delegate the same expression without -i.
        executable(base / "bin/sed", f'#!{sys.executable}\nimport pathlib, subprocess, sys\n'
                   'assert sys.argv[1] == "-i"\n'
                   'result = subprocess.run(["/usr/bin/sed", *sys.argv[2:]], check=True, capture_output=True)\n'
                   'pathlib.Path(sys.argv[-1]).write_bytes(result.stdout)\n')
        executable(stage / "lib/ld-linux-aarch64.so.1", '#!/bin/sh\n'
                   'printf "GENERATED\\n" >> "$FIXTURE_LOG"\n'
                   'for arg; do key=$arg; done\nprintf "fixture generated key" > "$key"\n')
        return script, build, stage, common, identity

    def test_obsolete_cached_init_is_not_shipped_again(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script, build, stage, common, identity = self.prepare(base)
            init = stage / 'etc/init.d'
            init.mkdir()
            old = init / 'S95pix-stream'
            old.write_text('obsolete cached launcher\n')
            current = init / 'S97pix-stream'
            current.write_text('current launcher\n')
            unrelated = init / 'S40network'
            unrelated.write_text('package-owned network service\n')
            result = self.run_finalize(base, script, build, stage)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertFalse(old.exists())
            self.assertEqual(current.read_text(), 'current launcher\n')
            self.assertEqual(unrelated.read_text(), 'package-owned network service\n')

    def run_finalize(self, base, script, build, root):
        env = fixture_env(base, BUILD_DIR=str(build))
        return subprocess.run(["sh", str(script), str(root)], env=env, capture_output=True, text=True)

    def test_whitelisted_identity_is_cpio_only_with_secure_modes_and_root_chown(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script, build, stage, common, identity = self.prepare(base)
            before = {str(p.relative_to(common)): p.read_bytes() for p in common.rglob("*") if p.is_file()}
            original_key = (identity / "dropbear_ed25519_host_key").read_bytes()
            result = self.run_finalize(base, script, build, stage)
            self.assertEqual(result.returncode, 0, result.stderr)
            calls = (base / "calls").read_text()
            self.assertNotIn("GENERATED", calls)
            self.assertTrue((identity / "dropbear_ed25519_host_key").read_bytes() == original_key)
            expected = (("etc/dropbear/dropbear_ed25519_host_key", "dropbear_ed25519_host_key", 0o600),
                        ("etc/default/bcas-linux", "etc/default/bcas-linux", 0o600),
                        ("etc/network/interfaces", "etc/network/interfaces", 0o644),
                        ("root/.ssh/authorized_keys", "root/.ssh/authorized_keys", 0o600))
            for dest, source, mode in expected:
                self.assertTrue((stage / dest).read_bytes() == (identity / source).read_bytes(), dest)
                self.assertEqual(stat.S_IMODE((stage / dest).stat().st_mode), mode)
                self.assertIn(str(stage / dest), calls)
            self.assertEqual(stat.S_IMODE((stage / "root/.ssh").stat().st_mode), 0o700)
            self.assertIn(f"chown 0:0 {stage}/root/.ssh\n", calls)
            self.assertTrue(all(line.startswith("chown 0:0 ") for line in calls.splitlines()))
            self.assertFalse((stage / "etc/unlisted").exists())
            self.assertFalse((stage / "etc/dropbear").is_symlink())
            after = {str(p.relative_to(common)): p.read_bytes() for p in common.rglob("*") if p.is_file()}
            self.assertEqual(before, after)
            self.assertNotIn("remount", (stage / "etc/inittab").read_text())
            self.assertNotIn("/dev/root", (stage / "etc/fstab").read_text())

    def test_missing_optional_inputs_leave_unit2_defaults_unchanged(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script, build, stage, _, _ = self.prepare(base, private=False)
            defaults = stage / "etc/default/bcas-linux"
            defaults.write_bytes(b"unit2 existing defaults\n")
            result = self.run_finalize(base, script, build, stage)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(defaults.read_bytes(), b"unit2 existing defaults\n")
            self.assertFalse((stage / "etc/network/interfaces").exists())
            self.assertFalse((stage / "var").exists())

    def test_optional_logo_cache_is_cpio_only_with_private_modes_and_root_owner(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script, build, stage, common, identity = self.prepare(base)
            logos = identity / "var/lib/pix/logos"
            logos.mkdir(parents=True)
            expected = {"index.json": b'{"fixture":true}\n'}
            for service in (101, 102, 103):
                expected[f"{service}.json"] = b'{"fixture":"metadata"}\n'
                expected[f"{service}.png"] = b"\x89PNG\r\nfixture logo bytes"
            for name, content in expected.items():
                (logos / name).write_bytes(content)
            result = self.run_finalize(base, script, build, stage)
            self.assertEqual(result.returncode, 0, result.stderr)
            destination = stage / "var/lib/pix/logos"
            self.assertEqual({path.name for path in destination.iterdir()}, set(expected))
            self.assertEqual(stat.S_IMODE(destination.stat().st_mode), 0o700)
            calls = (base / "calls").read_text()
            self.assertIn(f"chown 0:0 {destination}\n", calls)
            for name, content in expected.items():
                self.assertTrue((destination / name).read_bytes() == content, "logo cache bytes differ")
                self.assertTrue((logos / name).read_bytes() == content, "logo cache source changed")
                self.assertEqual(stat.S_IMODE((destination / name).stat().st_mode), 0o600)
                self.assertIn(f"chown 0:0 {destination / name}\n", calls)
            self.assertFalse((common / "var").exists())

    def test_logo_cache_rejects_symlinks_special_files_and_unexpected_types(self):
        for kind in ("symlink", "dangling", "directory", "fifo", "unexpected", "hidden"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                script, build, stage, _, identity = self.prepare(base)
                logos = identity / "var/lib/pix/logos"
                logos.mkdir(parents=True)
                member = logos / "invalid.json"
                outside = base / "outside"
                outside.write_bytes(b"must not read or overwrite\n")
                if kind == "symlink":
                    member.symlink_to(outside)
                elif kind == "dangling":
                    member.symlink_to(base / "missing")
                elif kind == "directory":
                    member.mkdir()
                elif kind == "fifo":
                    os.mkfifo(member)
                else:
                    member = logos / ("unexpected.txt" if kind == "unexpected" else ".unexpected")
                    member.write_bytes(b"not a cache type\n")
                result = self.run_finalize(base, script, build, stage)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("logo cache", result.stderr)
                self.assertFalse((stage / "var/lib/pix/logos" / member.name).exists())
                self.assertEqual(outside.read_bytes(), b"must not read or overwrite\n")

    def test_logo_cache_rejects_symlink_directory_paths_and_destinations(self):
        for location in ("source", "source-parent", "destination", "destination-file"):
            with self.subTest(location=location), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                script, build, stage, _, identity = self.prepare(base)
                outside = base / "outside"
                outside.mkdir()
                (outside / "index.json").write_bytes(b"outside cache\n")
                logos = identity / "var/lib/pix/logos"
                if location == "source":
                    logos.parent.mkdir(parents=True)
                    logos.symlink_to(outside, target_is_directory=True)
                elif location == "source-parent":
                    (outside / "logos").mkdir()
                    (identity / "var/lib").mkdir(parents=True)
                    (identity / "var/lib/pix").symlink_to(outside, target_is_directory=True)
                else:
                    logos.mkdir(parents=True)
                    (logos / "index.json").write_bytes(b"identity cache\n")
                    destination = stage / "var/lib/pix/logos"
                    if location == "destination":
                        destination.parent.mkdir(parents=True)
                        destination.symlink_to(outside, target_is_directory=True)
                    else:
                        destination.mkdir(parents=True)
                        (destination / "index.json").symlink_to(outside / "index.json")
                result = self.run_finalize(base, script, build, stage)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("logo", result.stderr)
                self.assertEqual((outside / "index.json").read_bytes(), b"outside cache\n")

    def test_only_standard_var_lib_tmp_link_is_materialized_in_cpio(self):
        for target in ("../tmp", "../other", "/tmp"):
            with self.subTest(target=target), tempfile.TemporaryDirectory() as d:
                base = Path(d)
                script, build, stage, common, identity = self.prepare(base)
                logos = identity / "var/lib/pix/logos"
                logos.mkdir(parents=True)
                (logos / "index.json").write_bytes(b"fixture cache index\n")
                (stage / "var").mkdir()
                (stage / "tmp").mkdir()
                (stage / "tmp/keep").write_bytes(b"original tmp contents\n")
                (stage / "var/lib").symlink_to(target, target_is_directory=True)
                result = self.run_finalize(base, script, build, stage)
                self.assertEqual((stage / "tmp/keep").read_bytes(), b"original tmp contents\n")
                self.assertFalse((common / "var").exists())
                if target == "../tmp":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertFalse((stage / "var/lib").is_symlink())
                    self.assertEqual(stat.S_IMODE((stage / "var/lib").stat().st_mode), 0o755)
                    self.assertTrue((stage / "var/lib/pix/logos/index.json").read_bytes() == b"fixture cache index\n")
                    self.assertFalse((stage / "tmp/pix").exists())
                    self.assertIn(f"chown 0:0 {stage}/var/lib\n", (base / "calls").read_text())
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("invalid staging logo directory", result.stderr)
                    self.assertTrue((stage / "var/lib").is_symlink())

    def test_fresh_identity_generates_key_once_then_reuses_it(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script, build, stage, _, identity = self.prepare(base, private=False, key=False)
            for _ in range(2):
                result = self.run_finalize(base, script, build, stage)
                self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual((base / "calls").read_text().count("GENERATED\n"), 1)
            self.assertTrue((identity / "dropbear_ed25519_host_key").is_file())
            self.assertEqual(stat.S_IMODE((identity / "dropbear_ed25519_host_key").stat().st_mode), 0o600)

    def test_shared_target_is_rejected_before_identity_or_root_changes(self):
        with tempfile.TemporaryDirectory() as d:
            base = Path(d)
            script, build, _, common, identity = self.prepare(base, key=False)
            result = self.run_finalize(base, script, build, common)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unexpected staging tree", result.stderr)
            self.assertFalse((identity / "dropbear_ed25519_host_key").exists())
            self.assertFalse((base / "calls").exists())


if __name__ == "__main__":
    unittest.main()
