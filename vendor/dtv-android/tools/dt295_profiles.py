#!/usr/bin/env python3
"""Compile, check and explain PIX-DT295 transcode register profiles.

pix-ts-legacy takes the on-board encoder setup as two environment strings,
DT295_XREG and DT295_XREG2, each a comma separated list of
"addr:shift:width:value" (addr and value hex, shift and width decimal). That is
enough to make the MB86M01 transcode, but every mistake is silent: a field that
overlaps another one, a value wider than its field, or a mode switch that runs
before the size is set all end as "no output" or as a broken stream, and the
host only sees a timeout.

This tool keeps the register map and the measured profiles in one place and
turns them into that string, so the setup is reviewable:

    python3 tools/dt295_profiles.py export avc736p8m     # paste into pix-dt295.conf
    python3 tools/dt295_profiles.py explain "152c:0:16:500,152e:0:16:2e0"
    python3 tools/dt295_profiles.py validate "$DT295_XREG" "$DT295_XREG2"
    python3 tools/dt295_profiles.py audit /data/local/tmp/pix-dt295.conf

The register names and the measured values come from docs/dt295-integration.md
section 5 and docs/dt295-interlace-ip-plan.md section 4. The MB86M01 field
layout is documented in the Elgato Game Capture HD driver (tolga9009/elgato-gchd,
MIT); the DT295 defaults and the working recipes were measured on the hardware.

Nothing here talks to the device: it checks what a profile *means*.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

# --------------------------------------------------------------------------
# Register map

ENCODE, PASSTHROUGH = 1, 2


@dataclass(frozen=True)
class Field:
    """One bit field of one 16-bit register."""

    address: int
    shift: int
    width: Optional[int]
    name: str
    kind: str = "raw"  # raw | size | bitrate | level | profile | enum | mode
    values: Optional[Dict[int, str]] = None
    unit: str = ""
    note: str = ""
    source: str = "measured"

    def label(self) -> str:
        return f"0x{self.address:04x}[{self.shift}:{self.width}] {self.name}"


FIELDS: List[Field] = [
    Field(0x100A, 4, 4, "v_ts_out_mode", "mode",
          {PASSTHROUGH: "passthrough", ENCODE: "encode"},
          note="DT295 default is 2 (passthrough); 1 switches the encoder on"),
    Field(0x101C, 8, 8, "program_video_type", "raw",
          {0x02: "MPEG-2", 0x1B: "H.264"},
          note="written with the route metadata; the emitted PMT stays 0x02"),
    Field(0x101C, 0, 8, "program_audio_type", "raw",
          {0x0F: "AAC"},
          note="written with the route metadata"),
    Field(0x1104, 0, 16, "system_rate", "raw",
          note="TS output rate; 0xffff on the DT295"),
    Field(0x1502, 0, 8, "v_format", "enum",
          {0: "1080i60", 2: "720p60", 32: "1080p60"},
          note="input format"),
    Field(0x1504, 0, 2, "tfrff", "raw",
          note="input timing; gchd always writes 2"),
    Field(0x1510, 0, 3, "v_rate_mode", "raw", note="rate control"),
    Field(0x1512, 8, 7, "v_ip_format", "enum",
          {0: "interlaced", 2: "progressive"},
          note="input scan type; measured NOT to change the output scan type"),
    Field(0x1518, 0, None, "v_gop_struct", "raw", note="width unconfirmed"),
    Field(0x1518, 8, None, "v_gop_size", "raw", note="width unconfirmed"),
    Field(0x1526, 0, 8, "v_h264_level", "level", note="H.264 level_idc, 31 = 3.1"),
    Field(0x1526, 8, 8, "v_h264_profile", "profile",
          {66: "Baseline", 77: "Main", 100: "High"},
          note="DT295 default is Main (77); the stream reports High"),
    Field(0x152C, 0, 16, "v_hsize_out", "size", unit="px"),
    Field(0x152E, 0, 16, "v_vsize_out", "size", unit="px"),
    Field(0x1532, 0, 16, "v_bitrate_a", "bitrate", unit="kbps"),
    Field(0x1534, 0, 16, "v_bitrate_b", "bitrate", unit="kbps"),
    Field(0x1536, 0, 16, "v_bitrate_c", "bitrate", unit="kbps"),
    Field(0x1538, 0, 16, "v_bitrate_d", "bitrate", unit="kbps",
          note="the recipes set this to 0"),
    Field(0x1570, 14, 1, "insert_pic_struct", "raw",
          note="insert the H.264 pic_struct SEI"),
    Field(0x1710, 12, 2, "v_v420", "raw", note="4:2:0 sampling"),
]

# H.264 level limits (only what the checker needs).
LEVELS: Dict[int, Tuple[int, int, int]] = {
    # level_idc: (max frame size in macroblocks, max MB/s, max bitrate kbps)
    30: (1620, 40500, 10000),
    31: (3600, 108000, 14000),
    32: (5120, 216000, 20000),
    40: (8192, 245760, 25000),
    41: (8192, 245760, 50000),
    42: (8704, 522240, 50000),
}

# --------------------------------------------------------------------------
# Profiles

# phase 1 (DT295_XREG) is applied right after the receive profile, phase 2
# (DT295_XREG2) right before the acquisition command. The mode switch must stay
# in phase 2 so the encoder sees a fully configured size/rate when it starts.
PROFILES: Dict[str, dict] = {
    "passthrough": {
        "note": "DT295 native MPEG-2 output; the STB encodes. Method C baseline.",
        "measured": True,
        "phase1": {},
        "phase2": {"mode": PASSTHROUGH},
    },
    "avc720p8m": {
        "note": "Measured 2026-11-03: High@3.1 AVC at 6.6 Mbps, 30 s with "
                "TEI/CC/CRC clean. A 720 request still emits 1280x736; the SPS "
                "carries no cropping.",
        "measured": True,
        "phase1": {"hsize": 1280, "vsize": 720, "bitrate_kbps": 8000, "level": 31},
        "phase2": {"video_type": 0x1B, "mode": ENCODE},
    },
    "avc720p6m": {
        "note": "Same as avc720p8m with a 6 Mbps target; derived, not measured.",
        "measured": False,
        "phase1": {"hsize": 1280, "vsize": 720, "bitrate_kbps": 6000, "level": 31},
        "phase2": {"video_type": 0x1B, "mode": ENCODE},
    },
    "avc736p8m": {
        "note": "The production profile: ask for 1280x736 so the SPS carries the "
                "macroblock-aligned height the encoder emits anyway.",
        "measured": True,
        "phase1": {"hsize": 1280, "vsize": 736, "bitrate_kbps": 8000, "level": 31},
        "phase2": {"video_type": 0x1B, "mode": ENCODE},
    },
    "progressive": {
        "note": "avc736p8m plus v_ip_format=2. Measured 2026-11-03: no effect on "
                "the output scan type (still PAFF), kept for reference only.",
        "measured": False,
        "phase1": {"hsize": 1280, "vsize": 736, "bitrate_kbps": 8000, "level": 31,
                   "ip_format": 2},
        "phase2": {"video_type": 0x1B, "mode": ENCODE},
    },
    "progressive-sei": {
        "note": "progressive plus insert_pic_struct, the untried second option from "
                "docs/dt295-interlace-ip-plan.md section 4.4: signal the field order "
                "in the SEI instead of changing the encoder's scan type. Not measured; "
                "check frame_mbs_only_flag and idet before keeping it.",
        "measured": False,
        "phase1": {"hsize": 1280, "vsize": 736, "bitrate_kbps": 8000, "level": 31,
                   "ip_format": 2, "pic_struct": 1},
        "phase2": {"video_type": 0x1B, "mode": ENCODE},
    },
}

# Semantic name -> (address, shift, value, width hint)
SEMANTIC = {
    "hsize": (0x152C, 0, 16),
    "vsize": (0x152E, 0, 16),
    "level": (0x1526, 0, 8),
    "profile": (0x1526, 8, 8),
    "video_type": (0x101C, 8, 8),
    "mode": (0x100A, 4, 4),
    "ip_format": (0x1512, 8, 7),
    "tfrff": (0x1504, 0, 2),
    "gop_size": (0x1518, 8, None),
    "rate_mode": (0x1510, 0, 3),
    "pic_struct": (0x1570, 14, 1),
}

PHASE1_ORDER = ["hsize", "vsize", "bitrate_kbps", "level", "profile", "gop_size",
                "rate_mode", "ip_format", "pic_struct", "tfrff"]
PHASE2_ORDER = ["video_type", "mode"]  # mode last: it starts the encoder


def find_field(address: int, shift: int, width: int) -> Optional[Field]:
    for candidate in FIELDS:
        if candidate.address != address:
            continue
        if candidate.width is None:
            if candidate.shift == shift:
                return candidate
        elif candidate.shift == shift and candidate.width == width:
            return candidate
    return None


def fields_at(address: int) -> List[Field]:
    return [candidate for candidate in FIELDS if candidate.address == address]


# --------------------------------------------------------------------------
# Spec parsing


class SpecError(Exception):
    pass


@dataclass
class Write:
    address: int
    shift: int
    width: int
    value: int
    order: int = 0

    def text(self) -> str:
        return f"{self.address:x}:{self.shift}:{self.width}:{self.value:x}"


SPEC_RE = re.compile(r"^([0-9a-fA-F]+):(\d+):(\d+):([0-9a-fA-F]+)$")


def parse_spec(spec: str) -> List[Write]:
    writes: List[Write] = []
    if not spec or not spec.strip():
        return writes
    for index, item in enumerate(spec.split(",")):
        item = item.strip()
        if not item:
            raise SpecError(f"empty entry at position {index + 1}")
        match = SPEC_RE.match(item)
        if not match:
            raise SpecError(
                f"'{item}': expected addr:shift:width:value "
                "(addr and value hex, shift and width decimal)")
        address, shift, width, value = match.groups()
        writes.append(Write(int(address, 16), int(shift), int(width), int(value, 16),
                            index))
    return writes


def decode_field(write: Write) -> str:
    field = find_field(write.address, write.shift, write.width)
    if field is None:
        other = fields_at(write.address)
        if other:
            return "unknown field of a known register (" + ", ".join(
                f"{c.name}[{c.shift}:{c.width}]" for c in other) + ")"
        return "unknown register"
    text = field.name
    if field.values and write.value in field.values:
        text += f" = {field.values[write.value]}"
    else:
        text += f" = {write.value}"
        if field.unit:
            text += f" {field.unit}"
    return text


# --------------------------------------------------------------------------
# Profiles -> specs


def _semantic_writes(key: str, value: int, order: int) -> List[Write]:
    if key == "bitrate_kbps":
        # The recipes set the three rate words to the same target and zero the
        # fourth; which word is max/ave/min is not confirmed.
        return [Write(0x1532, 0, 16, value, order),
                Write(0x1534, 0, 16, value, order + 1),
                Write(0x1536, 0, 16, value, order + 2),
                Write(0x1538, 0, 16, 0, order + 3)]
    if key not in SEMANTIC:
        raise SpecError(f"unknown setting: {key}")
    address, shift, width = SEMANTIC[key]
    if width is None:
        raise SpecError(f"{key} has no confirmed width")
    return [Write(address, shift, width, value, order)]


def compile_profile(profile: dict) -> Tuple[List[Write], List[Write]]:
    phase1: List[Write] = []
    for key in PHASE1_ORDER:
        if key not in profile.get("phase1", {}):
            continue
        phase1.extend(_semantic_writes(key, profile["phase1"][key], len(phase1)))
    phase2: List[Write] = []
    for key in PHASE2_ORDER:
        if key not in profile.get("phase2", {}):
            continue
        phase2.extend(_semantic_writes(key, profile["phase2"][key], len(phase2)))
    return phase1, phase2


def spec_of(writes: List[Write]) -> str:
    return ",".join(write.text() for write in writes)


# --------------------------------------------------------------------------
# Checks


def suggest_level(width: int, height: int, fps: float, kbps: int) -> Optional[int]:
    macroblocks = ((width + 15) // 16) * ((height + 15) // 16)
    macroblocks_per_second = macroblocks * fps
    for level in sorted(LEVELS):
        max_frame, max_rate, max_bitrate = LEVELS[level]
        if macroblocks <= max_frame and macroblocks_per_second <= max_rate \
                and kbps <= max_bitrate:
            return level
    return None


@dataclass
class Finding:
    severity: str  # error | warning | info
    message: str


ERROR, WARNING, INFO = "error", "warning", "info"


def check(writes: List[Write], experimental: bool = False) -> List[Finding]:
    findings: List[Finding] = []
    seen: Dict[Tuple[int, int, int], Write] = {}
    mode = None
    hsize = vsize = None
    bitrates: List[int] = []

    for write in writes:
        where = f"'{write.text()}'"

        # 1. the value must fit the field, and the field must fit the register
        if write.width < 1 or write.shift > 15 or write.shift + write.width > 16:
            findings.append(Finding(ERROR, f"{where}: shift {write.shift} + width "
                                           f"{write.width} does not fit a 16-bit register"))
            continue
        if write.value >= (1 << write.width):
            findings.append(Finding(
                ERROR, f"{where}: value 0x{write.value:x} does not fit {write.width} bits"))
            continue

        # 2. a register write must be understood, or it is a guess
        field = find_field(write.address, write.shift, write.width)
        if field is None:
            known = fields_at(write.address)
            if known:
                findings.append(Finding(
                    WARNING, f"{where}: 0x{write.address:04x} is known but "
                             f"[{write.shift}:{write.width}] is not; the map has "
                             + ", ".join(f"{c.name}[{c.shift}:{c.width}]" for c in known)))
            elif write.address >= 0x1700 and write.address < 0x1800 and not experimental:
                findings.append(Finding(
                    ERROR, f"{where}: 0x{write.address:04x} is in the undocumented "
                           "0x1700 block; pass --experimental to allow it"))
            else:
                findings.append(Finding(
                    WARNING, f"{where}: 0x{write.address:04x} is not in the map"))
            continue

        key = (write.address, write.shift, write.width)
        if key in seen and seen[key].value != write.value:
            findings.append(Finding(
                ERROR, f"{where}: {field.name} is written twice with different values "
                       f"(0x{seen[key].value:x} then 0x{write.value:x}; the last one wins)"))
        seen[key] = write

        # 3. field semantics
        if field.name == "v_ts_out_mode":
            mode = write.value
            if write.value not in (ENCODE, PASSTHROUGH):
                findings.append(Finding(
                    WARNING, f"{where}: encoder mode {write.value} is neither 1 (encode) "
                             "nor 2 (passthrough)"))
        elif field.name == "v_hsize_out":
            hsize = write.value
        elif field.name == "v_vsize_out":
            vsize = write.value
        elif field.kind == "bitrate" and field.name != "v_bitrate_d":
            bitrates.append(write.value)

    # 4. mode 1 needs a usable geometry and rate, otherwise the encoder has
    #    nothing to emit and the host only sees a timeout.
    if mode == ENCODE:
        if hsize is None or vsize is None:
            findings.append(Finding(
                ERROR, "mode=1 (encode) without v_hsize_out/v_vsize_out: the encoder "
                       "would run with the default zero size"))
        if not bitrates or not any(bitrates):
            findings.append(Finding(
                ERROR, "mode=1 (encode) without a non-zero bitrate"))
        if hsize and hsize % 16:
            findings.append(Finding(
                INFO, f"v_hsize_out {hsize} is not a multiple of 16; the encoder "
                      f"rounds the width up to {(hsize + 15) // 16 * 16}"))
        if vsize and vsize % 16:
            findings.append(Finding(
                INFO, f"v_vsize_out {vsize} is not a multiple of 16; the encoder "
                      f"rounds the height up to {(vsize + 15) // 16 * 16}"))
    elif mode == PASSTHROUGH:
        if bitrates or hsize or vsize:
            findings.append(Finding(
                WARNING, "mode=2 (passthrough) with size/bitrate writes: they are "
                         "ignored until the mode switches to 1"))

    # 5. the level must be plausible for the requested geometry
    level_write = next((write for write in writes
                        if find_field(write.address, write.shift, write.width)
                        and find_field(write.address, write.shift,
                                       write.width).name == "v_h264_level"), None)
    if mode == ENCODE and level_write and hsize and vsize:
        level_idc = level_write.value
        kbps = max(bitrates) if bitrates else 0
        needed = suggest_level(hsize, vsize, 30.0, kbps)
        limits = LEVELS.get(level_idc)
        if limits:
            macroblocks = ((hsize + 15) // 16) * ((vsize + 15) // 16)
            if macroblocks > limits[0] or macroblocks * 30 > limits[1] \
                    or kbps > limits[2]:
                findings.append(Finding(
                    INFO, f"level {level_idc} allows {limits[0]} macroblocks per frame "
                          f"and {limits[2]} kbps; {hsize}x{vsize} is {macroblocks} "
                          f"macroblocks at {kbps} kbps"
                          + (f" (level {needed} would be formally correct)" if needed
                             else "")))
        elif level_idc not in LEVELS:
            findings.append(Finding(
                WARNING, f"level {level_idc} is not a level this checker knows"))

    # 6. the mode switch has to come last: it starts the encoder.
    mode_writes = [write for write in writes
                   if find_field(write.address, write.shift, write.width)
                   and find_field(write.address, write.shift, write.width).name
                   == "v_ts_out_mode"]
    if mode_writes and mode_writes[-1] is not writes[-1]:
        findings.append(Finding(
            WARNING, "v_ts_out_mode is not the last write; the encoder should be "
                     "started after every other setting is in place"))
    return findings


def check_pair(phase1: List[Write], phase2: List[Write],
               experimental: bool = False) -> List[Finding]:
    findings = check(phase1 + phase2, experimental)
    overlap: Dict[Tuple[int, int, int], Write] = {}
    for write in phase1:
        overlap[(write.address, write.shift, write.width)] = write
    for write in phase2:
        key = (write.address, write.shift, write.width)
        if key in overlap and overlap[key].value != write.value:
            field = find_field(write.address, write.shift, write.width)
            name = field.name if field else f"0x{write.address:04x}"
            findings.append(Finding(
                WARNING, f"{name} is written in both DT295_XREG and DT295_XREG2 "
                         f"(0x{overlap[key].value:x} vs 0x{write.value:x}); the "
                         "second phase wins"))
    if not phase1 and not phase2:
        findings.append(Finding(WARNING, "empty profile: no register is set"))
    return findings


def has_errors(findings: List[Finding]) -> bool:
    return any(finding.severity == ERROR for finding in findings)


def print_findings(findings: List[Finding], stream=None) -> None:
    stream = stream or sys.stdout
    for finding in findings:
        print(f"  {finding.severity}: {finding.message}", file=stream)


# --------------------------------------------------------------------------
# Commands


def explain(writes: List[Write], label: str) -> None:
    print(f"{label}:")
    if not writes:
        print("  (no writes)")
        return
    for write in writes:
        width = f"{write.width:2d}"
        print(f"  0x{write.address:04x} [{write.shift:2d}:{width}] "
              f"= 0x{write.value:04x}  {decode_field(write)}")


def export(profile: dict, name: str, form: str) -> None:
    phase1, phase2 = compile_profile(profile)
    findings = check_pair(phase1, phase2)
    text1, text2 = spec_of(phase1), spec_of(phase2)
    if form == "json":
        print(json.dumps({
            "name": name,
            "measured": profile.get("measured", False),
            "note": profile.get("note", ""),
            "DT295_XREG": text1,
            "DT295_XREG2": text2,
            "DT295_TRANSCODE": "1" if any(
                w.address == 0x100A and w.value == ENCODE for w in phase2) else "0",
            "findings": [{"severity": f.severity, "message": f.message}
                         for f in findings],
        }, indent=2))
        return
    if form == "kv":
        print(f"DT295_XREG={text1}")
        print(f"DT295_XREG2={text2}")
        return
    print(f"# dt295_profiles.py: {name}")
    print(f"# {profile.get('note', '')}")
    print(f"# measured: {'yes' if profile.get('measured') else 'no (derived)'}")
    print(f"DT295_XREG={text1}")
    print(f"DT295_XREG2={text2}")
    print(f"DT295_TRANSCODE="
          f"{1 if any(w.address == 0x100A and w.value == ENCODE for w in phase2) else 0}")
    if findings:
        print("# checks:")
        for finding in findings:
            print(f"#   {finding.severity}: {finding.message}")


def command_list() -> None:
    print("profiles:")
    for name, profile in PROFILES.items():
        phase1, phase2 = compile_profile(profile)
        flag = "" if profile.get("measured") else "  (derived)"
        print(f"  {name:14s} {spec_of(phase1) or '-'}  |  {spec_of(phase2) or '-'}"
              f"{flag}")
    print()
    print("registers:")
    for field in FIELDS:
        width = "?" if field.width is None else str(field.width)
        values = ""
        if field.values:
            values = " " + "/".join(f"{k}={v}" for k, v in sorted(field.values.items()))
        print(f"  0x{field.address:04x} [{field.shift}:{width}] {field.name:20s}"
              f"{values}{('  ' + field.note) if field.note else ''}")


def command_explain(spec: str) -> int:
    try:
        writes = parse_spec(spec)
    except SpecError as error:
        print(f"dt295_profiles: {error}", file=sys.stderr)
        return 2
    explain(writes, "spec")
    print()
    print("DT295_XREG-style:", spec_of(writes))
    return 0


def command_validate(specs: List[str], experimental: bool = False) -> int:
    try:
        phase1 = parse_spec(specs[0]) if len(specs) > 0 else []
        phase2 = parse_spec(specs[1]) if len(specs) > 1 else []
    except SpecError as error:
        print(f"dt295_profiles: {error}", file=sys.stderr)
        return 2
    findings = check_pair(phase1, phase2, experimental)
    explain(phase1, "DT295_XREG")
    explain(phase2, "DT295_XREG2")
    print()
    if findings:
        print_findings(findings)
    if has_errors(findings):
        print("result: rejected")
        return 1
    print("result: accepted" + (" (with warnings)" if findings else ""))
    return 0


def command_audit(path: str, experimental: bool = False) -> int:
    try:
        with open(path, "r", encoding="utf-8") as handle:
            text = handle.read()
    except OSError as error:
        print(f"dt295_profiles: {error}", file=sys.stderr)
        return 2

    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        values[key.strip()] = value.strip().strip('"').strip("'")

    if "DT295_XREG" not in values and "DT295_XREG2" not in values:
        print(f"dt295_profiles: no DT295_XREG/DT295_XREG2 in {path}", file=sys.stderr)
        return 2

    print(f"{path}:")
    for key in ("PIX_TUNER", "DT295_TRANSCODE", "DT295_SYSTEM", "DT295_ASSETS"):
        if key in values:
            print(f"  {key}={values[key]}")
    specs = [values.get("DT295_XREG", ""), values.get("DT295_XREG2", "")]
    result = command_validate(specs, experimental)
    transcode = values.get("DT295_TRANSCODE", "0") in ("1", "yes", "true")
    phase2 = parse_spec(specs[1]) if specs[1] else []
    encodes = any(write.address == 0x100A and write.value == ENCODE for write in phase2)
    if transcode != encodes:
        print(f"  warning: DT295_TRANSCODE={values.get('DT295_TRANSCODE', '?')} but the "
              f"mode register says {'encode' if encodes else 'passthrough'}")
        return 1
    return result


def command_diff(left: str, right: str) -> int:
    def resolve(name: str) -> Tuple[List[Write], List[Write]]:
        if name in PROFILES:
            return compile_profile(PROFILES[name])
        return parse_spec(name), []

    try:
        a1, a2 = resolve(left)
        b1, b2 = resolve(right)
    except SpecError as error:
        print(f"dt295_profiles: {error}", file=sys.stderr)
        return 2
    def keyed(writes):
        return {(write.address, write.shift, write.width): write.value
                for write in writes}

    a = keyed(a1 + a2)
    b = keyed(b1 + b2)
    changed = False
    for key in sorted(set(a) | set(b)):
        if a.get(key) != b.get(key):
            changed = True
            field = find_field(*key)
            label = field.name if field else f"0x{key[0]:04x}"
            print(f"  {label}: {a.get(key)} -> {b.get(key)}")
    if not changed:
        print("  (identical)")
    return 0


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Compile and check PIX-DT295 transcode register profiles.")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="show the profiles and the register map")

    show = sub.add_parser("show", help="explain one profile")
    show.add_argument("name")

    exp = sub.add_parser("export", help="emit DT295_XREG/DT295_XREG2 for a profile")
    exp.add_argument("name")
    exp.add_argument("--format", default="shell", choices=["shell", "kv", "json"])

    expl = sub.add_parser("explain", help="decode an addr:shift:width:value list")
    expl.add_argument("spec")

    val = sub.add_parser("validate", help="check DT295_XREG [DT295_XREG2]")
    val.add_argument("spec", nargs="+")
    val.add_argument("--experimental", action="store_true",
                     help="allow writes to the undocumented 0x1700 block")

    aud = sub.add_parser("audit", help="check the DT295_XREG lines of a conf file")
    aud.add_argument("path")
    aud.add_argument("--experimental", action="store_true")

    dif = sub.add_parser("diff", help="compare two profiles or specs")
    dif.add_argument("left")
    dif.add_argument("right")

    args = parser.parse_args(argv)

    if args.command == "list":
        command_list()
        return 0
    if args.command == "show":
        profile = PROFILES.get(args.name)
        if profile is None:
            print(f"dt295_profiles: unknown profile: {args.name}", file=sys.stderr)
            return 2
        phase1, phase2 = compile_profile(profile)
        print(f"{args.name}: {profile['note']}")
        print(f"measured: {'yes' if profile['measured'] else 'no (derived)'}")
        explain(phase1, "DT295_XREG")
        explain(phase2, "DT295_XREG2")
        print()
        print_findings(check_pair(phase1, phase2))
        return 0
    if args.command == "export":
        profile = PROFILES.get(args.name)
        if profile is None:
            print(f"dt295_profiles: unknown profile: {args.name}", file=sys.stderr)
            return 2
        export(profile, args.name, args.format)
        return 0
    if args.command == "explain":
        return command_explain(args.spec)
    if args.command == "validate":
        return command_validate(args.spec, args.experimental)
    if args.command == "audit":
        return command_audit(args.path, args.experimental)
    if args.command == "diff":
        return command_diff(args.left, args.right)
    return 2


if __name__ == "__main__":
    sys.exit(main())
