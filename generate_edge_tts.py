#!/usr/bin/env python3
"""Standalone Edge TTS utility.

Generates an OGG audio file from text using the Burmese neural voice
my-MM-NilarNeural, with pitch shifted by +45Hz and volume at 100%.

Dependencies:
    pip install edge-tts

ffmpeg must be available on PATH (used only to encode the final .ogg).
"""

from __future__ import annotations

import argparse
import asyncio
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

VOICE = "my-MM-NilarNeural"
PITCH = "+45Hz"
VOLUME = "+100%"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate audio with Edge TTS (my-MM-NilarNeural, pitch +45Hz, "
            "volume +100%) and save it as .ogg in the current directory."
        )
    )
    parser.add_argument(
        "text",
        nargs="?",
        help="Text to speak. If omitted, text is read from stdin.",
    )
    parser.add_argument(
        "-o",
        "--output",
        help="Output .ogg filename (saved in the current directory). "
        "If omitted, a timestamped name is used.",
    )
    return parser.parse_args()


def resolve_text(args: argparse.Namespace) -> str:
    if args.text:
        return args.text.strip()
    if not sys.stdin.isatty():
        return sys.stdin.read().strip()
    print("Enter text (end with Ctrl+Z then Enter on Windows, or Ctrl+D on Unix):")
    return sys.stdin.read().strip()


def resolve_output_path(requested: str | None) -> Path:
    cwd = Path.cwd()
    if requested:
        name = Path(requested).name
        if not name.lower().endswith(".ogg"):
            name = f"{Path(name).stem}.ogg"
        return cwd / name
    stamp = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
    return cwd / f"edge_tts_{stamp}.ogg"


def convert_mp3_to_ogg(mp3_path: Path, ogg_path: Path) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        raise RuntimeError(
            "ffmpeg was not found on PATH. Install ffmpeg so the output "
            "can be encoded as .ogg."
        )
    cmd = [
        ffmpeg,
        "-y",
        "-i",
        str(mp3_path),
        "-c:a",
        "libvorbis",
        "-q:a",
        "5",
        str(ogg_path),
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout or "").strip()
        raise RuntimeError(f"ffmpeg failed to write OGG:\n{detail}")


async def synthesize(text: str, mp3_path: Path) -> None:
    import edge_tts

    communicate = edge_tts.Communicate(text, VOICE, pitch=PITCH, volume=VOLUME)
    await communicate.save(str(mp3_path))


def main() -> int:
    args = parse_args()
    text = resolve_text(args)
    if not text:
        print("Error: no text provided.", file=sys.stderr)
        return 1

    output_path = resolve_output_path(args.output)

    try:
        import edge_tts  # noqa: F401
    except ImportError:
        print("Error: edge-tts is not installed. Run: pip install edge-tts", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        mp3_path = Path(tmp) / "speech.mp3"
        try:
            asyncio.run(synthesize(text, mp3_path))
            convert_mp3_to_ogg(mp3_path, output_path)
        except Exception as exc:
            print(f"Error: {exc}", file=sys.stderr)
            return 1

    print(f"Saved: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
