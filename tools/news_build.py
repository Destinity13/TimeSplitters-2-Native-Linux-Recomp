#!/usr/bin/env python3
"""Builds the launcher news feed from the posts in news/.

Each news/YYYY-MM-DD-name.md starts with a header block:
    ---
    title: ...
    date: YYYY-MM-DD
    tag: Coming soon | Patch | Release | Dev log
    summary: one or two sentences
    ---
Writes launcher/news.json (read by the launcher's News tab) and refreshes the post table in news/README.md.
"""
import json
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
NEWS = ROOT / "news"
REPO_URL = "https://github.com/Destinity13/TimeSplitters-2-Native-Recomp-Windows-Linux-SteamDeck/blob/main/news/"


def read_post(path):
    text = path.read_text(encoding="utf-8")
    m = re.match(r"^---\n(.*?)\n---\n", text, re.S)
    if not m:
        raise SystemExit(f"{path.name}: header block (--- ... ---) missing")
    meta = {}
    for line in m.group(1).splitlines():
        if ":" in line:
            k, v = line.split(":", 1)
            meta[k.strip()] = v.strip()
    for k in ("title", "date", "tag", "summary"):
        if not meta.get(k):
            raise SystemExit(f"{path.name}: '{k}' missing in the header")
    meta["file"] = path.name
    return meta


def main():
    posts = sorted((read_post(p) for p in NEWS.glob("[0-9]*.md")), key=lambda p: p["date"], reverse=True)
    feed = [{"date": p["date"] + "T12:00:00Z", "title": p["title"], "tag": p["tag"], "body": p["summary"], "url": REPO_URL + p["file"]} for p in posts]
    out = ROOT / "launcher" / "news.json"
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(feed, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    readme = NEWS / "README.md"
    rows = "\n".join(f"| {p['date']} | [{p['title']}]({p['file']}) | {p['tag']} |" for p in posts)
    table = "| Date | Post | Type |\n|---|---|---|\n" + rows + "\n"
    text = readme.read_text(encoding="utf-8")
    text = re.sub(r"\| Date \| Post \| Type \|\n\|---\|---\|---\|\n(?:\|.*\|\n)*", lambda _: table, text, count=1)
    readme.write_text(text, encoding="utf-8")
    print(f"{len(posts)} post(s) -> {out.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
