#!/usr/bin/env python3
"""Contract check: movy_config.json / module.json chain_params / the C param
table / ui_chain.js must all describe the same parameter surface.

Shape rules come from Movy's ModuleConfig (src/types/param.ts) and its
renderer; the 5-char enum rule comes from store.ts formatValue, which does
options[i].substring(0, 5) on the knob readout — options that collide there
are indistinguishable on the device even though the data is correct.

Runs in the build container before the cross-compile; a violation fails it.
"""
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
fails = []


def check(cond, msg):
    if not cond:
        fails.append(msg)


# ---- sources ----------------------------------------------------------
cfg = json.loads((ROOT / "src/movy_config.json").read_text())
mod = json.loads((ROOT / "src/module.json").read_text())
chain = mod["capabilities"]["chain_params"]
params_h = (ROOT / "src/dsp/te2_params.h").read_text()
ui_chain = (ROOT / "src/ui_chain.js").read_text()

chain_by_key = {p["key"]: p for p in chain}

# ---- movy_config.json shape -------------------------------------------
check(cfg.get("id") == mod["id"], f"movy id {cfg.get('id')} != module id {mod['id']}")
check(bool(cfg.get("name")), "movy config has a name")

banks = cfg.get("banks", [])
check(len(banks) >= 1, "at least one bank")

VALID_TYPES = {"float", "int", "enum", "file"}
seen_keys = []
for b in banks:
    check(isinstance(b.get("name"), str) and b["name"], "bank has a name")
    check(len(b["name"]) <= 8, f"bank name '{b['name']}' should be short (<=8)")
    for r in b.get("rows", []):
        check(len(r) == 8,
              f"bank {b['name']}: a row must have exactly 8 slots, got {len(r)}")
        for s in r:
            if s is None:
                continue
            k = s.get("key")
            check(isinstance(k, str) and bool(k), f"slot missing key in bank {b['name']}")
            check(k not in seen_keys, f"duplicate key {k}")
            seen_keys.append(k)
            check(s.get("type") in VALID_TYPES, f"{k}: bad type {s.get('type')}")
            check(isinstance(s.get("short"), str) and 0 < len(s["short"]) <= 5,
                  f"{k}: short label must be 1..5 chars ('{s.get('short')}')")
            check(isinstance(s.get("full"), str) and bool(s["full"]), f"{k}: full label")
            if s["type"] == "enum":
                check(isinstance(s.get("options"), list) and bool(s["options"]),
                      f"{k}: enum needs options")
            else:
                check("min" in s and "max" in s and s["min"] < s["max"],
                      f"{k}: needs min < max")

# ---- movy_config <-> chain_params -------------------------------------
for k in seen_keys:
    check(k in chain_by_key, f"movy_config key {k} missing from chain_params")
for k in chain_by_key:
    check(k in seen_keys, f"chain_params key {k} is not on any movy page")

for b in banks:
    for r in b.get("rows", []):
        for s in r:
            if not s or s["key"] not in chain_by_key:
                continue
            cp = chain_by_key[s["key"]]
            check(s["type"] == cp["type"],
                  f"{s['key']}: movy type {s['type']} != chain type {cp['type']}")
            if s["type"] == "enum":
                check(s["options"] == cp["options"],
                      f"{s['key']}: movy options differ from chain_params")
            else:
                check(s["min"] == cp["min"] and s["max"] == cp["max"],
                      f"{s['key']}: movy range differs from chain_params")

# ---- the 5-char knob readout ------------------------------------------
# Movy renders options[i].substring(0, 5); a collision makes two distinct
# settings look identical on the device.
for p in chain:
    if p["type"] != "enum":
        continue
    trunc = {}
    for opt in p["options"]:
        t = opt[:5]
        check(t not in trunc,
              f"{p['key']}: options '{trunc.get(t)}' and '{opt}' both display "
              f"as '{t}' (movy truncates to 5 chars)")
        trunc[t] = opt

# ---- C param table is the source of truth -----------------------------
c_keys = re.findall(r'\{\s*"(\w+)",\s+"', params_h)
for k in chain_by_key:
    check(k in c_keys, f"chain_params key {k} not in te2_params.h")

for p in chain:
    if p["type"] != "enum":
        continue
    # every option string must appear verbatim in the C options table,
    # otherwise set_param's name lookup silently falls back to atof() -> 0
    for opt in p["options"]:
        check(f'"{opt}"' in params_h,
              f"{p['key']}: option '{opt}' missing from te2_params.h")

# ---- ui_chain.js mirrors the same keys and options --------------------
for k in chain_by_key:
    check(f'"{k}"' in ui_chain, f"ui_chain.js is missing param {k}")
for p in chain:
    if p["type"] != "enum":
        continue
    for opt in p["options"]:
        check(f'"{opt}"' in ui_chain,
              f"ui_chain.js: {p['key']} option '{opt}' missing/stale")

# ---- the .so filename the host will actually dlopen -------------------
# An audio_fx module is loaded from "<audio_fx>/<id>/<id>.so" (chain_host.c
# builds that path literally and NEVER reads the module.json "dsp" field).
# Only sound_generators are loaded as dsp.so. Getting this wrong makes the
# module appear in the FX picker (the list comes from module.json) and then
# silently fail to load, which is exactly what shipping dsp.so did.
if mod.get("component_type") == "audio_fx":
    want = f'{mod["id"]}.so'
    check(mod.get("dsp") == want,
          f'audio_fx dsp must be "{want}", got "{mod.get("dsp")}"')
    cmake = (ROOT / "CMakeLists.txt").read_text()
    check(f'OUTPUT_NAME "{mod["id"]}"' in cmake,
          f'CMakeLists must set OUTPUT_NAME "{mod["id"]}" so the build emits {want}')
    for script, needle in (("scripts/docker-build.sh", f"build/{want}"),
                           ("scripts/deploy.sh", f'SO="{want}"')):
        check(needle in (ROOT / script).read_text(),
              f"{script} must ship {want}")

# ---- module.json under the 8 KB loader cap ----------------------------
sz = (ROOT / "src/module.json").stat().st_size
check(sz < 8192, f"module.json {sz} bytes exceeds the 8 KB loader cap")

if fails:
    print("CONFIG CONTRACT FAILED:")
    for f in fails:
        print("  -", f)
    sys.exit(1)

pages = sum(len(b.get("rows", [])) for b in banks)
print(f"config contract OK: {len(banks)} banks / {pages} pages, "
      f"{len(seen_keys)} knobs, {len(chain)} chain_params, module.json {sz} B")
