#!/usr/bin/env python3
"""Give the recordings that lost their programme name a real file name.

Recordings made while the EPG did not resolve the programme were published as
``<HHMMSS>_<channel>__<uuid>`` because the recorder had nothing better. The
recorder now takes the title from the broadcast EIT, so this tool only exists
for the files recorded before that change.

The title comes from a Gガイド listing dump (``--guide``), which is a one-off
fallback: it is not a service the recording pipeline depends on. A recording
is matched by its channel and by the wall clock of the segment, and renamed to

    <HHMMSS>_<title>__<uuid>/<YYYYMMDD>_<HHMMSS>_<title>__p01.mp4

The uuid is kept: it is the key the archive database, the CM sidecar and the
CM cut view use.

Usage:
    python3 tools/rename-recordings.py --guide guide.json            # plan
    python3 tools/rename-recordings.py --guide guide.json --apply    # rename
"""
import argparse
import datetime
import glob
import json
import os
import re
import sqlite3
import sys

# Gガイド's 静岡 area lists the terrestrial stations in this order.
TERRESTRIAL = {
    "ＮＨＫ総合１・静岡": "1",
    "ＮＨＫＥテレ１静岡": "2",
    "Ｄａｉｉｃｈｉ－ＴＶ１": "3",
    "静岡朝日テレビ": "4",
    "ＳＢＳ１": "5",
    "テレビ静岡": "6",
}
# Same for the BS page.
BS = {
    "ＮＨＫ　ＢＳ": "1",
    "ＢＳ朝日１": "3",
    "ＢＳ－ＴＢＳ": "4",
    "ＢＳ１１イレブン": "12",
    "放送大学テレビ": "14",
    "Ｊ：ＣＯＭ　ＢＳ": "26",
    "ＢＳよしもと": "27",
}

NAME_MAX_BYTES = 210


def sanitize(text):
    text = text.replace("/", "_").replace("\\", "_")
    # Gガイド renders ARIB's programme icons as emoji; they are markers, not
    # part of the title, and the EIT path drops them too.
    text = "".join(c if ord(c) >= 0x20 and not (0x1F000 <= ord(c) <= 0x1FAFF) else
                   ("_" if ord(c) < 0x20 else "") for c in text)
    return text.strip()


def truncate_utf8(text, limit):
    raw = text.encode("utf-8")
    if len(raw) <= limit:
        return text
    return raw[:limit].decode("utf-8", "ignore").rstrip()


def load_guide(path):
    """{channel_line: [(start, end, title), ...]} for every day in the dump."""
    data = json.load(open(path, encoding="utf-8"))
    guide = {}
    for kind in ("terrestrial", "bs"):
        for day in data.get(kind, {}).values():
            for line, items in day.get("programs", {}).items():
                key = (kind, line)
                bucket = guide.setdefault(key, [])
                for it in items:
                    if it.get("title") and it.get("s") and it.get("e"):
                        bucket.append((it["s"], it["e"], it["title"]))
    for key in guide:
        guide[key].sort()
    return guide


def channel_key(service_name):
    if service_name in TERRESTRIAL:
        return ("terrestrial", TERRESTRIAL[service_name])
    if service_name in BS:
        return ("bs", BS[service_name])
    return None


def lookup(guide, key, wall_ms):
    items = guide.get(key)
    if not items or not wall_ms:
        return None
    stamp = datetime.datetime.fromtimestamp(wall_ms / 1000).strftime("%Y%m%d%H%M")
    for start, end, title in items:
        if start <= stamp < end:
            return title
    return None


def sidecar_time(doc):
    for path in (("program", "startAt"), ("onAir", "startAt"), ("capture", "startAt")):
        node = doc
        for name in path:
            node = node.get(name) if isinstance(node, dict) else None
            if node is None:
                break
        if isinstance(node, int) and node > 0:
            return node
    return 0


def archive_title(db_path, service_id, event_id, wall_ms):
    """Second fallback: the EPG the archive stored before it went stale."""
    if not db_path or not os.path.exists(db_path):
        return None
    try:
        con = sqlite3.connect("file:%s?mode=ro" % db_path, uri=True, timeout=5)
    except sqlite3.Error:
        return None
    try:
        if event_id:
            row = con.execute(
                "SELECT json_extract(json,'$.name') FROM airings "
                "WHERE json_extract(json,'$.serviceId')=? AND json_extract(json,'$.eventId')=? "
                "AND json_extract(json,'$.name') IS NOT NULL LIMIT 1",
                (service_id, event_id)).fetchone()
            if row and row[0]:
                return row[0]
        if wall_ms:
            row = con.execute(
                "SELECT json_extract(json,'$.name') FROM airings "
                "WHERE json_extract(json,'$.serviceId')=? AND start_at<=? AND start_at+duration>? "
                "AND json_extract(json,'$.name') IS NOT NULL LIMIT 1",
                (service_id, wall_ms, wall_ms)).fetchone()
            if row and row[0]:
                return row[0]
    except sqlite3.Error:
        return None
    finally:
        con.close()
    return None


def plan_airtime(root, guide, date_prefix):
    """Plan renames by the wall clock the segment started, not by its name.

    Used to repair recordings made while eit_watch picked the following
    programme: the name in the sidecar belongs to the next programme, the
    content is the one on air when the capture started.
    """
    plan = []
    skipped = []
    for path in sorted(glob.glob(os.path.join(root, "20*", "*", "*", "*", "*", "*.json"))):
        if path.endswith(".cm.json"):
            continue
        record_dir = os.path.dirname(path)
        if date_prefix and date_prefix not in record_dir:
            continue
        try:
            doc = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            skipped.append((path, "unreadable sidecar"))
            continue
        service = doc.get("service", {})
        service_name = service.get("name", "")
        key = channel_key(service_name)
        if key is None:
            skipped.append((path, "no guide channel for %r" % service_name))
            continue
        wall_ms = (doc.get("capture", {}) or {}).get("startAt") or 0
        if not wall_ms:
            skipped.append((path, "no capture time"))
            continue
        title = lookup(guide, key, wall_ms)
        if not title:
            skipped.append((path, "no guide entry for %s" % (
                datetime.datetime.fromtimestamp(wall_ms / 1000).strftime("%Y-%m-%d %H:%M"))))
            continue
        title = truncate_utf8(sanitize(title), NAME_MAX_BYTES)
        stem = os.path.basename(path)[:-5]
        parts = stem.split("_", 2)
        if len(parts) != 3 or not parts[2].endswith("__p01"):
            skipped.append((path, "unexpected file name"))
            continue
        datestamp, clock, rest = parts
        old_name = rest[:-len("__p01")]
        if old_name == title:
            continue
        parent = os.path.dirname(record_dir)
        uuid = os.path.basename(record_dir).rsplit("__", 1)[-1]
        plan.append({
            "sidecar": path,
            "record_dir": record_dir,
            "parent": parent,
            "uuid": uuid,
            "datestamp": datestamp,
            "clock": clock,
            "old_name": old_name,
            "new_name": title,
            "service": service_name,
            "wall_ms": wall_ms,
            "guide_title": title,
        })
    return plan, skipped


def plan(root, guide):
    plan = []
    skipped = []
    for path in sorted(glob.glob(os.path.join(root, "20*", "*", "*", "*", "*", "*.json"))):
        if path.endswith(".cm.json"):
            continue
        try:
            doc = json.load(open(path, encoding="utf-8"))
        except (OSError, ValueError):
            skipped.append((path, "unreadable sidecar"))
            continue
        program = doc.get("program", {})
        service = doc.get("service", {})
        service_name = service.get("name", "")
        # "GR" is what the recorder writes when it has no programme; older or
        # partial files can carry null, and a few carry the channel name.
        name = program.get("name")
        if name not in (None, "", "GR") and name != service_name:
            continue
        key = channel_key(service_name)
        if key is None:
            skipped.append((path, "no guide channel for %r" % service_name))
            continue
        wall_ms = sidecar_time(doc)
        title = lookup(guide, key, wall_ms)
        if not title:
            title = archive_title(os.path.join(root, "_catalog", "archive.sqlite"),
                                  service.get("serviceId") if isinstance(service, dict) else None,
                                  program.get("eventId"), wall_ms)
        if not title:
            skipped.append((path, "no guide entry for %s" % (
                datetime.datetime.fromtimestamp(wall_ms / 1000).strftime("%Y-%m-%d %H:%M")
                if wall_ms else "unknown time")))
            continue
        title = truncate_utf8(sanitize(title), NAME_MAX_BYTES)
        if not title:
            skipped.append((path, "empty title"))
            continue
        record_dir = os.path.dirname(path)
        stem = os.path.basename(path)[:-5]           # drop .json
        parts = stem.split("_", 2)                   # YYYYMMDD_HHMMSS_<name>__p01
        if len(parts) != 3 or not parts[2].endswith("__p01"):
            skipped.append((path, "unexpected file name"))
            continue
        datestamp, clock, rest = parts
        old_name = rest[:-len("__p01")]
        parent = os.path.dirname(record_dir)
        uuid = os.path.basename(record_dir).rsplit("__", 1)[-1]
        plan.append({
            "sidecar": path,
            "record_dir": record_dir,
            "parent": parent,
            "uuid": uuid,
            "datestamp": datestamp,
            "clock": clock,
            "old_name": old_name,
            "new_name": title,
            "service": service_name,
            "wall_ms": wall_ms,
        })
    return plan, skipped


def rename_paths(entry):
    """Rename the directory and every file whose name carries the old title."""
    old_dir = entry["record_dir"]
    new_dir = os.path.join(entry["parent"],
                           "%s_%s__%s" % (entry["clock"], entry["new_name"], entry["uuid"]))
    if old_dir == new_dir:
        return []
    os.rename(old_dir, new_dir)
    moved = [(old_dir, new_dir)]

    old_prefix = "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["old_name"])
    new_prefix = "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["new_name"])
    for name in os.listdir(new_dir):
        if not name.startswith(old_prefix):
            continue
        target = new_prefix + name[len(old_prefix):]
        os.rename(os.path.join(new_dir, name), os.path.join(new_dir, target))
        moved.append((os.path.join(new_dir, name), os.path.join(new_dir, target)))
    entry["new_dir"] = new_dir
    return moved


def rewrite_sidecar(entry):
    """Point the sidecar at the new file and record the title it now has."""
    path = os.path.join(entry["new_dir"], os.path.basename(entry["sidecar"]).replace(
        "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["old_name"]),
        "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["new_name"])))
    doc = json.load(open(path, encoding="utf-8"))
    old_rel = doc.get("media", {}).get("relativePath", "")
    new_rel = old_rel.replace(
        "%s_%s__%s" % (entry["clock"], entry["old_name"], entry["uuid"]),
        "%s_%s__%s" % (entry["clock"], entry["new_name"], entry["uuid"]))
    new_rel = new_rel.replace(
        "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["old_name"]),
        "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["new_name"]))
    doc.setdefault("media", {})["relativePath"] = new_rel
    program = doc.setdefault("program", {})
    program["name"] = entry["new_name"]
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(doc, f, ensure_ascii=False, indent=2)
    os.replace(tmp, path)
    entry["new_sidecar"] = path
    entry["relative_path"] = new_rel
    entry["document"] = doc
    return path


def rewrite_cm(entry):
    """The CM sidecar stores absolute paths to the media it was cut from."""
    new_dir = entry["new_dir"]
    old_prefix = "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["old_name"])
    new_prefix = "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["new_name"])
    old_dir_name = "%s_%s__%s" % (entry["clock"], entry["old_name"], entry["uuid"])
    new_dir_name = "%s_%s__%s" % (entry["clock"], entry["new_name"], entry["uuid"])
    for name in os.listdir(new_dir):
        if not (name.startswith(new_prefix) and name.endswith(".cm.json")):
            continue
        path = os.path.join(new_dir, name)
        doc = json.load(open(path, encoding="utf-8"))
        for key in ("original", "output"):
            if isinstance(doc.get(key), str):
                doc[key] = doc[key].replace(old_dir_name, new_dir_name).replace(
                    old_prefix, new_prefix)
        meta = doc.get("programme_metadata")
        if isinstance(meta, dict):
            media = meta.get("media")
            if isinstance(media, dict) and isinstance(media.get("relativePath"), str):
                media["relativePath"] = media["relativePath"].replace(
                    "%s_%s__%s" % (entry["clock"], entry["old_name"], entry["uuid"]),
                    "%s_%s__%s" % (entry["clock"], entry["new_name"], entry["uuid"]))
                media["relativePath"] = media["relativePath"].replace(old_prefix, new_prefix)
        tmp = path + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(doc, f, ensure_ascii=False, indent=2)
        os.replace(tmp, path)


def rename_view(root, entry):
    """The CM cut view mirrors the name and links to the sidecar by path."""
    uuid = entry["uuid"]
    pattern = os.path.join(root, "CMカット", "20*", "*", "*", "*", "*_*__%s" % uuid)
    renames = []
    for view_dir in glob.glob(pattern):
        old_view = view_dir
        new_view = os.path.join(os.path.dirname(view_dir),
                                "%s_%s__%s" % (entry["clock"], entry["new_name"], uuid))
        os.rename(old_view, new_view)
        renames.append((old_view, new_view))
        old_prefix = "%s_%s_" % (entry["datestamp"], entry["clock"])
        for name in os.listdir(new_view):
            if not name.startswith(old_prefix):
                continue
            new_name = name.replace(entry["old_name"], entry["new_name"], 1)
            src = os.path.join(new_view, name)
            if os.path.islink(src):
                target = os.readlink(src)
                target = target.replace(
                    "%s_%s__%s" % (entry["clock"], entry["old_name"], uuid),
                    "%s_%s__%s" % (entry["clock"], entry["new_name"], uuid))
                target = target.replace(
                    "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["old_name"]),
                    "%s_%s_%s" % (entry["datestamp"], entry["clock"], entry["new_name"]))
                os.remove(src)
                os.symlink(target, os.path.join(new_view, new_name))
            else:
                os.rename(src, os.path.join(new_view, new_name))
            renames.append((src, os.path.join(new_view, new_name)))
    return renames


def rewrite_db(db_path, entry):
    """Keep /media/<part> playback working: the archive serves by stored path."""
    doc = entry.get("document") or {}
    part_id = doc.get("partId")
    if not part_id:
        return 0
    rel = "recordings/" + entry["relative_path"]
    payload = json.dumps(doc, ensure_ascii=False)
    for attempt in range(5):
        try:
            con = sqlite3.connect(db_path, timeout=10)
            try:
                cur = con.execute("UPDATE recordings SET path=?, json=? WHERE part_id=?",
                                  (rel, payload, part_id))
                con.commit()
                return cur.rowcount
            finally:
                con.close()
        except sqlite3.OperationalError as exc:
            if "locked" not in str(exc) or attempt == 4:
                raise
            import time
            time.sleep(1)
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="/mnt/xblk/video/zenroku/recordings")
    ap.add_argument("--guide", required=True)
    ap.add_argument("--db", default=None, help="archive.sqlite (default: <root>/_catalog)")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--journal", default=None)
    ap.add_argument("--by-airtime", action="store_true",
                    help="rename everything in --date by the capture start time")
    ap.add_argument("--service", default="",
                    help="limit to one service name (e.g. テレビ静岡)")
    ap.add_argument("--date", default="",
                    help="YYYY/MM/DD fragment to limit --by-airtime")
    args = ap.parse_args()

    db_path = args.db or os.path.join(args.root, "_catalog", "archive.sqlite")
    guide = load_guide(args.guide)
    if args.by_airtime:
        entries, skipped = plan_airtime(args.root, guide, args.date)
        if args.service:
            entries = [e for e in entries if e["service"] == args.service]
    else:
        entries, skipped = plan(args.root, guide)

    print("planned %d recording(s), skipped %d" % (len(entries), len(skipped)))
    by_service = {}
    for e in entries:
        by_service[e["service"]] = by_service.get(e["service"], 0) + 1
    for service, count in sorted(by_service.items(), key=lambda kv: -kv[1]):
        print("  %6d  %s" % (count, service))
    if skipped:
        print("skipped examples:")
        for path, why in skipped[:10]:
            print("  %s  (%s)" % (path, why))
        if len(skipped) > 10:
            print("  ... %d more" % (len(skipped) - 10))

    if not args.apply:
        print("dry run: rerun with --apply to rename")
        return 0

    if args.limit:
        entries = entries[:args.limit]

    journal = []
    failures = 0
    for i, entry in enumerate(entries, 1):
        try:
            moved = rename_paths(entry)
            rewrite_sidecar(entry)
            rewrite_cm(entry)
            moved += rename_view(args.root, entry)
            rows = rewrite_db(db_path, entry)
            journal.append({"old_dir": entry["record_dir"], "new_dir": entry["new_dir"],
                            "uuid": entry["uuid"], "old_name": entry["old_name"],
                            "new_name": entry["new_name"], "moved": moved, "db_rows": rows})
            print("[%d/%d] %s -> %s" % (i, len(entries), entry["old_name"], entry["new_name"]))
        except Exception as exc:                     # noqa: BLE001 - keep going, report
            failures += 1
            print("[%d/%d] FAILED %s: %s" % (i, len(entries), entry["record_dir"], exc),
                  file=sys.stderr)
    if args.journal:
        with open(args.journal, "w", encoding="utf-8") as f:
            json.dump(journal, f, ensure_ascii=False, indent=1)
        print("journal: %s (%d entries)" % (args.journal, len(journal)))
    print("done: %d renamed, %d failed" % (len(journal), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
