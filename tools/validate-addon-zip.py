#!/usr/bin/env python3

import argparse
import tempfile
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path, PurePosixPath


REJECTED_PARTS = {".git", "__pycache__", ".pytest_cache"}
REJECTED_SUFFIXES = {".bak", ".orig", ".rej", ".tmp"}


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate a Kodi install ZIP")
    parser.add_argument("zip_path", type=Path)
    parser.add_argument("--addon-id", required=True)
    parser.add_argument("--github-output", type=Path)
    parser.add_argument("--print-version", action="store_true")
    args = parser.parse_args()

    with zipfile.ZipFile(args.zip_path) as archive:
        files = [entry for entry in archive.infolist() if not entry.is_dir()]
        if not files:
            raise SystemExit("archive contains no files")

        paths = [PurePosixPath(entry.filename) for entry in files]
        for path in paths:
            if path.is_absolute() or ".." in path.parts:
                raise SystemExit(f"unsafe archive path: {path}")
            if REJECTED_PARTS.intersection(path.parts):
                raise SystemExit(f"unwanted archive path: {path}")
            if path.suffix in REJECTED_SUFFIXES:
                raise SystemExit(f"unwanted archive file: {path}")

        roots = {path.parts[0] for path in paths}
        if roots != {args.addon_id}:
            raise SystemExit(f"expected one top-level {args.addon_id} directory, found {sorted(roots)}")

        manifest_path = f"{args.addon_id}/addon.xml"
        if manifest_path not in {entry.filename for entry in files}:
            raise SystemExit(f"missing {manifest_path}")

        manifest = ET.fromstring(archive.read(manifest_path))
        addon_id = manifest.attrib.get("id")
        version = manifest.attrib.get("version")
        if addon_id != args.addon_id or not version:
            raise SystemExit("manifest id or version is invalid")

        with tempfile.TemporaryDirectory(prefix="kodi-addon-zip-") as directory:
            archive.extractall(directory)
            extracted_manifest = Path(directory, args.addon_id, "addon.xml")
            if not extracted_manifest.is_file():
                raise SystemExit("clean extraction did not produce addon.xml")

    if args.print_version:
        print(version)
    else:
        print(f"validated {args.zip_path}: {addon_id} {version}")
    if args.github_output:
        with args.github_output.open("a", encoding="utf-8") as output:
            output.write(f"addon_id={addon_id}\n")
            output.write(f"addon_version={version}\n")


if __name__ == "__main__":
    main()
