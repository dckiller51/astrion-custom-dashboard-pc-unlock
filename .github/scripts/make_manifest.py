"""Writes the ESP Web Tools manifest for one firmware: python make_manifest.py <dir> <version> <factory.bin>."""

import json
import pathlib
import sys

folder, version, factory = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
if not (folder / factory).exists():
    sys.exit(f"missing {factory}")
manifest = {
    "name": "Astrion PC Unlock",
    "version": version,
    "new_install_prompt_erase": True,
    "builds": [{"chipFamily": "ESP32-S3", "parts": [{"path": factory, "offset": 0}]}],
}
(folder / pathlib.Path(factory).name.replace(".factory.bin", ".manifest.json")).write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
