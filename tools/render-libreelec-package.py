#!/usr/bin/env python3

import argparse
from pathlib import Path


PLACEHOLDERS = {
    "@PKG_VERSION@": "version",
    "@PKG_SHA256@": "sha256",
    "@PKG_SITE@": "site",
    "@PKG_URL@": "url",
}


def main() -> None:
    parser = argparse.ArgumentParser(description="Render the LibreELEC package recipe")
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--site", required=True)
    parser.add_argument("--url", required=True)
    args = parser.parse_args()

    text = args.template.read_text(encoding="utf-8")
    for placeholder, attribute in PLACEHOLDERS.items():
        if text.count(placeholder) != 1:
            raise SystemExit(f"expected exactly one {placeholder} placeholder")
        text = text.replace(placeholder, getattr(args, attribute))

    if "@PKG_" in text:
        raise SystemExit("unresolved package placeholder")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
