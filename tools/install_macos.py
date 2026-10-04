#!/usr/bin/env python3
"""Stage a signed CLAP bundle, then rename it into place without rewriting a loaded binary."""
import argparse
from datetime import datetime
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build", type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin":
        parser.error("This installer is for macOS.")
    source = args.build.resolve() / "GranularDelay.clap"
    if not (source / "Contents/MacOS/GranularDelay").is_file():
        parser.error(f"Build the plugin first: {source}")
    subprocess.run(["codesign", "--verify", "--deep", "--strict", str(source)], check=True)
    folder = Path.home() / "Library/Audio/Plug-Ins/CLAP"
    folder.mkdir(parents=True, exist_ok=True)
    stage = Path(tempfile.mkdtemp(prefix=".GranularDelay-stage-", dir=folder))
    target = folder / "GranularDelay.clap"
    retired = folder / (".GranularDelay-previous-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    try:
        shutil.copytree(source, stage, dirs_exist_ok=True)
        subprocess.run(["codesign", "--verify", "--deep", "--strict", str(stage)], check=True)
        if target.exists():
            os.rename(target, retired)
        try:
            os.rename(stage, target)
        except OSError:
            if retired.exists():
                os.rename(retired, target)
            raise
    finally:
        if stage.exists():
            shutil.rmtree(stage)
    print(f"Installed: {target}")
    if retired.exists():
        print(f"Previous bundle preserved: {retired}")
    print("Reload the plugin in Bitwig to use the new version. Existing instances keep their loaded binary.")


if __name__ == "__main__":
    main()
