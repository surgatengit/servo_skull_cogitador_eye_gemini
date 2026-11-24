#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Provision the Gemini API key (or model) into VOX M-01 over ESP-Iris RPC.

The key is read from, in order: the GEMINI_API_KEY environment variable, a local
`.env` file next to this project (ignored by Git), or a hidden interactive prompt.
It is sent once to the device, which stores it in its own NVS.

Exposure analysis (see docs/INSTALL.md):
- The workspace CLI is invoked *in this process*, so the key never appears in any
  process command line.
- mosaico.py forwards the payload to the ESP-Iris Gateway through stdin; its run
  log records the child command without the payload.
- The Gateway operation record stores only the payload size and SHA-256.
- Output is captured and any occurrence of the key is redacted before printing.
- The firmware offers no RPC that reads the key back.

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
import contextlib
import getpass
import io
import os
import re
import sys
from pathlib import Path

SERVICE = 0x5658
SET_API_KEY, CLEAR_API_KEY, STATUS, SET_MODEL = 1, 2, 3, 4
MODELS = ("gemini-3.8-live", "gemini-3.1-flash-live-preview")
KEY_PATTERN = re.compile(r"^[A-Za-z0-9._-]{30,128}$")

PROJECT = Path(__file__).resolve().parents[1]
WORKSPACE = PROJECT.parents[1]
TOOLS_ROOT = WORKSPACE / "submodule" / "esp-mosaico-utils" / "mosaico-tools"


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


def resolve_key() -> tuple[str, str]:
    key = os.environ.get("GEMINI_API_KEY", "").strip()
    if key:
        return key, "variable de entorno GEMINI_API_KEY"
    key = read_dotenv("GEMINI_API_KEY")
    if key:
        return key, ".env local"
    return getpass.getpass("Clave de API de Gemini (no se mostrará): ").strip(), "entrada oculta"


def load_cli():
    if not (TOOLS_ROOT / "tools" / "mosaico_cli" / "cli.py").is_file():
        sys.exit(f"No se encuentra mosaico-tools en {TOOLS_ROOT}; "
                 "el proyecto debe estar en esp-mosaico-vibe/projects/.")
    sys.path.insert(0, str(TOOLS_ROOT / "tools"))
    from mosaico_cli.cli import main as cli_main  # noqa: PLC0415 - workspace-pinned CLI
    return cli_main


def rpc(method: int, payload: bytes = b"", secret: str = "") -> int:
    cli_main = load_cli()
    argv = ["--workspace", str(WORKSPACE), "iris", "rpc", str(SERVICE), str(method),
            "--project", str(PROJECT)]
    if payload:
        argv += ["--payload-hex", payload.hex()]
    captured = io.StringIO()
    with contextlib.redirect_stdout(captured), contextlib.redirect_stderr(captured):
        try:
            code = cli_main(argv, tool_root=TOOLS_ROOT)
        except SystemExit as stop:  # argparse or CLI fatal paths
            code = stop.code if isinstance(stop.code, int) else 1
    text = captured.getvalue()
    if secret:
        for needle in (secret, secret.encode("ascii").hex(), payload.hex()):
            if needle:
                text = text.replace(needle, "<redacted>")
    print(text.rstrip())
    return int(code or 0)


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

    key, source = resolve_key()
    if not KEY_PATTERN.match(key):
        print("La clave no tiene un formato válido (30-128 caracteres A-Z a-z 0-9 . _ -).", file=sys.stderr)
        return 2
    print(f"Enviando la clave desde: {source} (no se muestra).")
    try:
        code = rpc(SET_API_KEY, key.encode("ascii"), secret=key)
    finally:
        key = ""
    if source == ".env local":
        print("Recomendado: borra o vacía GEMINI_API_KEY en .env ahora que el dispositivo la guarda.")
    return code


if __name__ == "__main__":
    sys.exit(main())
