#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Provision the Gemini API key (or model) into VOX M-01 over ESP-Iris RPC.

The key is read from, in order: the GEMINI_API_KEY environment variable, a local
`.env` file next to this project (ignored by Git), or an interactive prompt.
It is sent once to the device, which stores it in its own NVS. The key is never
printed. The firmware offers no RPC that reads the key back.

Examples (from the project directory, inside esp-mosaico-vibe/projects/):
    python tools/provision_key.py            # store the key
    python tools/provision_key.py --status   # {"api_key":true,"model":""}
    python tools/provision_key.py --model gemini-3.1-flash-live-preview
    python tools/provision_key.py --model "" # back to the firmware default
    python tools/provision_key.py --clear    # erase the key from the device

Use USB (the default ESP-Iris transport) for provisioning.
"""

from __future__ import annotations

import argparse
import getpass
import os
import re
import subprocess
import sys
from pathlib import Path

SERVICE = 0x5658
SET_API_KEY, CLEAR_API_KEY, STATUS, SET_MODEL = 1, 2, 3, 4
MODELS = ("gemini-3.8-live", "gemini-3.1-flash-live-preview")
KEY_PATTERN = re.compile(r"^[A-Za-z0-9._-]{30,128}$")

PROJECT = Path(__file__).resolve().parents[1]
WORKSPACE_CLI = PROJECT.parents[1] / "mosaico.py"


def read_dotenv(name: str) -> str:
    env_file = PROJECT / ".env"
    if not env_file.is_file():
        return ""
    for line in env_file.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key.strip() == name:
            return value.strip().strip('"').strip("'")
    return ""


def resolve_key() -> str:
    key = os.environ.get("GEMINI_API_KEY", "").strip() or read_dotenv("GEMINI_API_KEY")
    if not key:
        key = getpass.getpass("Clave de API de Gemini (no se mostrará): ").strip()
    return key


def rpc(method: int, payload: bytes = b"") -> int:
    if not WORKSPACE_CLI.is_file():
        sys.exit(f"No se encuentra {WORKSPACE_CLI}; el proyecto debe estar en esp-mosaico-vibe/projects/.")
    command = [sys.executable, str(WORKSPACE_CLI), "iris", "rpc", str(SERVICE), str(method),
               "--project", str(PROJECT)]
    if payload:
        # mosaico.py forwards hex payloads to ESP-Iris through stdin, not argv.
        command += ["--payload-hex", payload.hex()]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    output = (result.stdout or "").strip()
    secret_hex = payload.hex() if method == SET_API_KEY and payload else ""
    for stream in (output, (result.stderr or "").strip()):
        if stream:
            print(stream.replace(secret_hex, "<redacted>") if secret_hex else stream)
    return result.returncode


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--status", action="store_true", help="show whether a key is stored")
    group.add_argument("--clear", action="store_true", help="erase the stored key")
    group.add_argument("--model", metavar="NAME", help=f"set model override ({', '.join(MODELS)} or '')")
    args = parser.parse_args()

    if args.status:
        return rpc(STATUS)
    if args.clear:
        return rpc(CLEAR_API_KEY)
    if args.model is not None:
        if args.model and args.model not in MODELS:
            parser.error(f"modelo no permitido: {args.model}")
        return rpc(SET_MODEL, args.model.encode("ascii"))

    key = resolve_key()
    if not KEY_PATTERN.match(key):
        print("La clave no tiene un formato válido (30-128 caracteres A-Z a-z 0-9 . _ -).", file=sys.stderr)
        return 2
    try:
        return rpc(SET_API_KEY, key.encode("ascii"))
    finally:
        key = ""


if __name__ == "__main__":
    sys.exit(main())
