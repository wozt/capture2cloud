#!/usr/bin/env python3
"""Print GitHub Actions outputs for safe incremental four-component tags."""
import os
import re
import subprocess

head = os.environ.get("GITHUB_SHA", "")
if not re.fullmatch(r"[0-9a-fA-F]{40}", head):
    raise SystemExit("GITHUB_SHA is missing/invalid")
subprocess.run(["git", "fetch", "--tags", "origin"], check=True)
raw = subprocess.check_output(["git", "tag", "--list"], text=True)
all_tags = []
for tag in raw.splitlines():
    match = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)\.(\d+)", tag)
    if match:
        all_tags.append((tuple(map(int, match.groups())), tag))
for _, tag in all_tags:
    sha = subprocess.check_output(
        ["git", "rev-list", "-n", "1", tag], text=True
    ).strip()
    if sha.casefold() == head.casefold():
        print("skip=true")
        print(f"tag={tag}")
        break
else:
    if not all_tags:
        next_parts = (1, 0, 0, 0)
    else:
        parts, _ = max(all_tags)
        next_parts = parts[:3] + (parts[3] + 1,)
    print("skip=false")
    print("tag=v" + ".".join(map(str, next_parts)))
