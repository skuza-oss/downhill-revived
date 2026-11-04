#!/usr/bin/env python3
"""Post a short, player-facing summary of a GitHub release to a Discord webhook.

Reads the release as JSON on stdin (`gh release view TAG --json name,tagName,body,url`).
The summary is the bold lead of each bullet (or the bullet itself when it has none),
skipping sections such as "Known issues" and install instructions. To write the Discord text
yourself, put it in the release notes inside a hidden block:

    <!-- discord
    Your text here.
    -->

Posts to the webhook in DISCORD_WEBHOOK; with --dry-run (or no webhook) it only prints.
"""
import json
import os
import re
import sys
import urllib.request

MAX_ITEMS = 6
MAX_ITEM_LEN = 140
SKIP_SECTIONS = re.compile(r"known issue|install|check|download|checksum|build|verify", re.I)


def first_sentence(text):
    text = re.sub(r"\s*\([^)]*\)", "", text)  # drop parentheticals
    return re.split(r"(?<=[.!?])\s", text.strip())[0]


def highlights(body):
    """Bold-led bullets first (the headlines), then plain bullets, up to MAX_ITEMS."""
    headlines, plain, skipping = [], [], False
    for line in body.splitlines():
        heading = re.match(r"#{1,6}\s+(.*)", line)
        if heading:
            skipping = bool(SKIP_SECTIONS.search(heading.group(1)))
            continue
        bullet = re.match(r"\s{0,3}[-*]\s+(.*)", line)
        if skipping or not bullet:
            continue
        text = bullet.group(1).strip()
        bold = re.match(r"\*\*(.+?)\*\*(.*)", text)
        if bold:
            lead, rest = bold.groups()
            # A lead that is already a sentence, or is followed by ":", stands alone.
            text = lead if re.search(r"[.!?]$", lead) or rest.startswith(":") else first_sentence(lead + rest)
            target = headlines
        else:
            text, target = first_sentence(text), plain
        text = re.sub(r"[*_`]", "", text).strip().rstrip(":")
        if len(text) > MAX_ITEM_LEN:
            text = text[: MAX_ITEM_LEN - 1].rstrip() + "…"
        if text and text not in headlines + plain:
            target.append(text)
    return (headlines + plain)[:MAX_ITEMS]


def message(release):
    body = release.get("body") or ""
    title = release.get("name") or release["tagName"]
    url = release["url"]
    custom = re.search(r"<!--\s*discord\s*\n(.*?)-->", body, re.S)
    if custom:
        text = custom.group(1).strip()
    else:
        items = highlights(body)
        text = "\n".join(f"• {item}" for item in items) or "Fixes and improvements."
    msg = (
        f"🚵 **{title} is out!**\n\n{text}\n\n"
        f"The game offers the update when you start it, or download it here: <{url}>"
    )
    return msg[:2000]


def main():
    release = json.load(sys.stdin)
    msg = message(release)
    webhook = os.environ.get("DISCORD_WEBHOOK")
    if "--dry-run" in sys.argv or not webhook:
        print(msg)
        return
    data = json.dumps({"content": msg, "allowed_mentions": {"parse": []}}).encode()
    req = urllib.request.Request(
        webhook,
        data=data,
        headers={
            "Content-Type": "application/json",
            # Discord rejects urllib's default User-Agent.
            "User-Agent": "DownhillRevived-ReleaseBot (https://github.com/skuza-oss/downhill-revived, 1.0)",
        },
    )
    with urllib.request.urlopen(req) as resp:
        print(f"Posted to Discord (HTTP {resp.status}):\n{msg}")


if __name__ == "__main__":
    main()
