#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prueba desde el PC qué variantes del mensaje `setup` de Gemini Live acepta el servidor.

Abre una conexión por variante (mismo endpoint que el dispositivo), envía el `setup` y
muestra si llega `setupComplete` o el código y el motivo del cierre. No envía audio.

La clave se lee como en provision_key.py: variable GEMINI_API_KEY, `.env` local del
proyecto o entrada oculta. Nunca se imprime ni se guarda.

Uso (desde la raíz del proyecto):  python tools/test_live_setup.py
Requiere: pip install websockets
"""

import json
import sys

from provision_key import resolve_key

try:
    from websockets.exceptions import ConnectionClosed, InvalidStatus
    from websockets.sync.client import connect
except ImportError:
    sys.exit("Falta el paquete websockets: python -m pip install websockets")

ENDPOINT = ("wss://generativelanguage.googleapis.com/ws/"
            "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=")
MODELS = ("gemini-3.1-flash-live-preview", "gemini-3.8-live")


def base_setup(model: str) -> dict:
    """Same fields VOX sends (app_gemini_live.c, build_setup)."""
    return {
        "model": f"models/{model}",
        "generationConfig": {
            "responseModalities": ["AUDIO"],
            "speechConfig": {"voiceConfig": {"prebuiltVoiceConfig": {"voiceName": "Charon"}},
                             "languageCode": "es-ES"},
        },
        "systemInstruction": {"parts": [{"text": "Eres VOX M-01. Responde en español."}]},
        "inputAudioTranscription": {},
        "outputAudioTranscription": {},
        "realtimeInputConfig": {"automaticActivityDetection": {"disabled": False}},
        "contextWindowCompression": {"slidingWindow": {}},
        "sessionResumption": {},
    }


def variants(model: str):
    yield "sin herramientas (control, como 1.1.0)", base_setup(model)
    s = base_setup(model)
    s["tools"] = [{"googleSearch": {}}]
    yield "tools googleSearch (como 1.2.0)", s
    s = base_setup(model)
    s["tools"] = [{"google_search": {}}]
    yield "tools google_search", s
    s = base_setup(model)
    s["tools"] = [{"googleSearch": {}}]
    del s["generationConfig"]["speechConfig"]["languageCode"]
    yield "googleSearch sin languageCode", s
    s = base_setup(model)
    s["tools"] = [{"googleSearch": {}}]
    for field in ("inputAudioTranscription", "outputAudioTranscription"):
        del s[field]
    yield "googleSearch sin transcripciones", s
    s = base_setup(model)
    s["tools"] = [{"googleSearch": {}}]
    for field in ("contextWindowCompression", "sessionResumption"):
        del s[field]
    yield "googleSearch sin compresión ni reanudación", s


def try_setup(key: str, setup: dict) -> str:
    try:
        with connect(ENDPOINT + key, open_timeout=10, close_timeout=2) as ws:
            ws.send(json.dumps({"setup": setup}))
            reply = ws.recv(timeout=10)
            message = json.loads(reply)
            if "setupComplete" in message:
                return "ACEPTADO (setupComplete)"
            return "respuesta inesperada: " + ", ".join(message)[:120]
    except ConnectionClosed as exc:
        rcvd = exc.rcvd
        if rcvd is None:
            return "cerrado sin trama de cierre"
        return f"RECHAZADO: código {rcvd.code}, motivo: {rcvd.reason or '(sin motivo)'}"
    except InvalidStatus as exc:
        return f"RECHAZADO en el saludo HTTP: {exc.response.status_code}"
    except TimeoutError:
        return "sin respuesta en 10 s"
    except OSError as exc:  # red; el mensaje no contiene la URI
        return f"error de red: {type(exc).__name__}"


def main() -> int:
    key, source = resolve_key()
    if not key:
        print("No hay clave de API.")
        return 1
    print(f"Clave leída de: {source} (no se muestra)\n")
    for model in MODELS:
        print(f"== {model}")
        for name, setup in variants(model):
            print(f"  {name:45s} -> {try_setup(key, setup)}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
