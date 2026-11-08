#!/usr/bin/env python3
"""Install the optional, SHA-256-verified OpenVINO stems provider (no pip needed)."""

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import sys
import tempfile
import urllib.request
import zipfile


ASSETS = {
    "linux": (
        "https://files.pythonhosted.org/packages/88/01/4e72a8a65b584f01d6e59eb453bae397b82b2e51df1b5c94efa05e17800b/onnxruntime_ep_openvino-1.8.1-py3-none-manylinux_2_28_x86_64.whl",
        "3832643158c8c187fcab6db661cde2a8c06a2b3559d95d0f2411ac0e51c5e924",
        "libonnxruntime_providers_openvino_plugin.so",
    ),
    "win32": (
        "https://files.pythonhosted.org/packages/75/98/224ead07344390258ae0515ed285ba2951e9c1405f4507443a03bb7dd5c7/onnxruntime_ep_openvino-1.8.1-py3-none-win_amd64.whl",
        "1acd878be173b02bb8b953460f2f59b121e5f6e47ebc6aad490c0590cf6718fb",
        "onnxruntime_providers_openvino_plugin.dll",
    ),
}


def default_destination():
    if sys.platform == "win32":
        base = Path(os.environ["LOCALAPPDATA"])
    else:
        base = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    return base / "BrockDJ/stems-providers/openvino"


def install(destination):
    if platform.machine().lower() not in ("x86_64", "amd64") or sys.platform not in ASSETS:
        raise RuntimeError("OpenVINO stems require Linux or Windows x64; macOS uses CoreML.")
    url, expected_hash, library = ASSETS[sys.platform]
    if destination.exists() or destination.is_symlink():
        raise RuntimeError(f"Refusing to overwrite existing provider directory: {destination}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="brockdj-openvino-", dir=destination.parent) as temporary:
        temporary = Path(temporary)
        archive = temporary / "provider.whl"
        digest = hashlib.sha256()
        with urllib.request.urlopen(url, timeout=60) as response, archive.open("wb") as output:
            while block := response.read(1024 * 1024):
                digest.update(block)
                output.write(block)
        if digest.hexdigest() != expected_hash:
            raise RuntimeError("OpenVINO download SHA-256 mismatch; nothing was installed.")
        payload = temporary / "payload"
        payload.mkdir()
        with zipfile.ZipFile(archive) as wheel:
            for entry in wheel.infolist():
                if not entry.filename.startswith("onnxruntime_ep_openvino/") or entry.is_dir():
                    continue
                relative = entry.filename.removeprefix("onnxruntime_ep_openvino/")
                path = PurePosixPath(relative)
                if path.is_absolute() or ".." in path.parts or "\\" in relative or ":" in relative:
                    raise RuntimeError(f"Invalid provider archive path: {relative}")
                target = payload.joinpath(*path.parts)
                target.parent.mkdir(parents=True, exist_ok=True)
                with wheel.open(entry) as source, target.open("wb") as output:
                    shutil.copyfileobj(source, output)
        if not (payload / library).is_file() or not (payload / "third-party-programs.txt").is_file():
            raise RuntimeError("Provider archive is missing its library or license notices.")
        payload.rename(destination)
    print(f"OpenVINO stems provider 1.8.1 installed to {destination}")
    print("Requires ONNX Runtime 1.23+ and an Intel GPU compute driver.")
    print("Enable experimental GPU acceleration in Stems settings and restart BrockDJ.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, help="Override the per-user provider directory")
    arguments = parser.parse_args()
    try:
        install((arguments.destination or default_destination()).absolute())
    except (OSError, RuntimeError, zipfile.BadZipFile, KeyError) as error:
        parser.exit(1, f"OpenVINO installation failed: {error}\n")


if __name__ == "__main__":
    main()
