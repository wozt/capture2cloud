#!/usr/bin/env python3
"""Validate the exact seven WOZT v1.2 themes and nine JSON locale assets."""
from pathlib import Path
import json
import re

root = Path(__file__).resolve().parents[1] / "data"
themes = ["graphite", "light", "ocean", "emerald", "rose", "lime", "steam-legacy"]
languages = ["fr", "de", "it", "es", "pt", "ru", "ja", "ko", "zh-cn"]
colors = {"background", "panel", "entry", "border", "text", "muted",
          "accent", "button", "hover", "active", "negative"}
for folder, names in (("themes", themes), ("languages", languages)):
    paths = sorted((root / folder).glob("*.json"))
    by_id = {}
    for path in paths:
        data = json.loads(path.read_text(encoding="utf-8"))
        name = data.get("id")
        assert re.fullmatch(r"[a-z0-9_-]+", name or ""), path
        assert isinstance(data.get("name"), str) and data["name"], path
        assert name not in by_id, path
        by_id[name] = data
        if folder == "themes":
            palette = data.get("colors", {})
            assert colors <= palette.keys(), path
            assert all(re.fullmatch(r"#[0-9a-fA-F]{6}", palette[c]) for c in colors), path
        else:
            strings = data.get("translations")
            assert isinstance(strings, dict) and strings, path
            assert all(isinstance(k, str) and isinstance(v, str)
                       for k, v in strings.items()), path
    assert set(names) <= by_id.keys(), f"missing {folder}: {set(names) - by_id.keys()}"
    print(f"{folder}: {len(by_id)} JSON resources validated")
