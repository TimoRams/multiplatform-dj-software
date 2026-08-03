#!/usr/bin/env python3
"""Generate a deterministic source inventory without modifying production code."""

from __future__ import annotations

import argparse
import csv
import re
from collections import Counter
from pathlib import Path


SOURCE_SUFFIXES = {".cpp", ".c", ".h", ".hpp", ".qml"}


def domain_for(path: str) -> str:
    if path.startswith("src/qml/"):
        return "QML UI"
    if path.startswith("src/app/"):
        return "Application" if "Settings" not in path else "Settings"
    if path.startswith("src/audio/device/"):
        return "Audio Device"
    if path.startswith("src/audio/cache/"):
        return "Audio Cache / Streaming"
    if path.startswith("src/engine/deck/"):
        return "Deck Playback"
    if path.startswith("src/engine/scratch/"):
        return "Scratch"
    if path.startswith("src/engine/sync/") or path.endswith("SyncMaintenancePolicy.h"):
        return "Sync"
    if path == "src/engine/DjEngine.h" or path.startswith("src/engine/facade/"):
        return "Application"
    if path.startswith("src/engine/dsp/"):
        if "TimeStretch" in path:
            return "Time-Stretch"
        if "Scratch" in path:
            return "Scratch"
        if "Mixer" in path:
            return "Deck DSP / Mixer"
        return "Deck Playback"
    if path.startswith("src/engine/"):
        return "Master Mixing" if "Master" in path else "Deck Playback"
    if path.startswith("src/analysis/"):
        return "Analysis"
    if path.startswith("src/rendering/") or path.startswith("src/waveform/"):
        return "Waveform"
    if path.startswith("src/library/"):
        return "Library"
    if path.startswith("src/database/"):
        return "Database"
    if path.startswith("src/io/"):
        return "Media I/O"
    if path.startswith("src/controllers/") or path.startswith("src/midi/"):
        return "Controller / MIDI / HID"
    if path.startswith("src/fx/"):
        return "Effects"
    if path.startswith("src/link/"):
        return "Sync"
    if path.startswith("src/platform/"):
        return "Platform"
    if path.startswith("src/domain/"):
        return "Unknown / Mixed Responsibility"
    return "Application"


def classification_for(path: str) -> tuple[str, str, str, str]:
    name = Path(path).name
    if name == "DjEngine_TransportLatency.cpp":
        return ("INVESTIGATE", "", "Mixes global device and per-deck latency concerns", "medium")
    if path.startswith("src/engine/DjEngine_"):
        return ("INVESTIGATE", "", "Public facade split needs a responsibility-based consolidation", "medium")
    if name in {
        "DeckControl.qml", "EnlargedWaveform.qml", "BeatgridEditorPanel.qml", "OverallWaveform.qml",
        "FxUnit.qml", "FxBar.qml",
    }:
        return ("INVESTIGATE", "", "Potentially overlapping product UI; retain until instantiation audit", "medium")
    if name == "DevelopmentControlsWindow.qml":
        return ("INVESTIGATE", "", "Development surface; verify production reachability", "low")
    return ("KEEP", "", "Clear current boundary or insufficient evidence for a change", "low")


def line_count(path: Path) -> int:
    return len(path.read_text(encoding="utf-8", errors="replace").splitlines())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path,
                        default=Path("docs/architecture/file-classification.csv"))
    args = parser.parse_args()

    root = args.root.resolve()
    files = sorted(
        path for path in (root / "src").rglob("*")
        if path.is_file() and path.suffix in SOURCE_SUFFIXES
    )
    contents = {
        path: path.read_text(encoding="utf-8", errors="replace")
        for path in files
    }
    rows: list[dict[str, str | int]] = []
    domains: Counter[str] = Counter()
    lines_by_domain: Counter[str] = Counter()

    for path in files:
        relative = path.relative_to(root).as_posix()
        stem = path.stem
        references = [
            other.relative_to(root).as_posix()
            for other, content in contents.items()
            if other != path and re.search(rf"\b{re.escape(stem)}\b", content)
        ]
        domain = domain_for(relative)
        classification, merge_target, reason, risk = classification_for(relative)
        count = line_count(path)
        domains[domain] += 1
        lines_by_domain[domain] += count
        rows.append({
            "path": relative,
            "domain": domain,
            "line_count": count,
            "classification": classification,
            "merge_target": merge_target,
            "reason": reason,
            "risk": risk,
            "referenced_by": ";".join(references),
        })

    output = args.output if args.output.is_absolute() else root / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=[
            "path", "domain", "line_count", "classification", "merge_target",
            "reason", "risk", "referenced_by",
        ])
        writer.writeheader()
        writer.writerows(rows)

    print(f"files: {len(rows)}")
    for domain in sorted(domains):
        print(f"{domain}: {domains[domain]} files, {lines_by_domain[domain]} lines")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
