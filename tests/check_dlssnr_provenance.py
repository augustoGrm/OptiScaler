"""Verify imported source and shader payloads against the pinned NR manifest."""
import hashlib
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parent.parent
manifest = json.loads((root / "OptiScaler/dlssnr/UPSTREAM.json").read_text())
checked = 0
for entry in manifest["unchanged_files"]:
    actual = hashlib.sha256((root / entry["path"]).read_bytes()).hexdigest()
    assert actual == entry["sha256"], f"Imported file changed: {entry['path']}"
    checked += 1
for name, entry in manifest["adapted_files"].items():
    if isinstance(entry, dict) and "local_sha256" in entry:
        actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
        assert actual == entry["local_sha256"], f"Formatted import changed: {name}"
        checked += 1
header = (root / "OptiScaler/shaders/dlssnr/precompile/DlssNr_Shader.h").read_text()
array = re.search(r"DlssNr_cso\[\]\s*=\s*\{(.*?)\};", header, re.S)
assert array, "Compiled shader array not found"
payload = bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", array[1]))
assert hashlib.sha256(payload).hexdigest() == manifest["shader_bytecode_sha256"], "Shader payload changed"
print(f"PASS: {checked} pinned imports and {len(payload)} shader bytes ({manifest['tag']})")
