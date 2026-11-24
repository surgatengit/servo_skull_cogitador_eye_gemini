# VOX M-01 GEMINI DIRECT (cogitador_gemini)

Servocráneo VOX M-01 para ESP-Mosaico: un ojo mecánico a pantalla completa que
conversa por voz con **Gemini Live directamente por WebSocket seguro y Wi-Fi**,
sin Windows ni servicios intermedios. Aplicación nativa ESP-IDF (ESP32-S31)
creada con la plantilla Hello World de `mosaico.py` y GSP 1.5.1.

**Estado actual y cómo retomar: [docs/HANDOFF.md](docs/HANDOFF.md).**
Diseño, requisitos y decisiones: [docs/DESIGN.md](docs/DESIGN.md).
Instalación, restauración, auditoría de seguridad y pruebas físicas: [docs/INSTALL.md](docs/INSTALL.md).
Google Search nativo (1.2.0: fuentes, cuotas, costes y pensamiento): [docs/GOOGLE_SEARCH.md](docs/GOOGLE_SEARCH.md).
Ampliaciones futuras (cámara, Extended Thinking, base magnética): [docs/ROADMAP.md](docs/ROADMAP.md).
Investigación de proyectos de referencia y propuesta por fases: [docs/RESEARCH.md](docs/RESEARCH.md).

## Componentes

| Componente | Responsabilidad |
|---|---|
| [`app_vox_eye`](components/app_vox_eye/README.md) | Ojo mecánico GSP (iris, parpadeo, colores de estado, toque); también corre en el simulador |
| [`app_vox_session`](components/app_vox_session/README.md) | Máquina de estados de la conversación |
| [`app_gemini_live`](components/app_gemini_live/README.md) | Cliente Gemini Live por WSS (setup, audio, reanudación, Google Search y grounding) |
| [`app_audio`](components/app_audio/README.md) | ES8311: captura 48k→16k y reproducción 24k→48k |
| [`app_wake`](components/app_wake/README.md) | «Hola ESP» local (ESP-SR WakeNet10 `wn10_es_holaesp`), solo en reposo |
| [`app_wifi`](components/app_wifi/README.md) | Estación Wi-Fi con las credenciales de Vibe Mode |
| [`app_secrets`](components/app_secrets/README.md) | Clave de API en NVS propio + RPC privada de provisión |
| [`app_config`](components/app_config/README.md), [`app_display`](components/app_display/README.md) | Plantilla: lectura de Wi-Fi compartido y pantalla CO5300 |

## Requisitos

Este repositorio debe estar en `esp-mosaico-vibe/projects/servocraneo_gemini_direct/`
(usa `../../submodule/esp-mosaico-utils` y `../../submodule/esp-mosaico-bsp`), con
ESP-IDF en el commit `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe` para `esp32s31`.

## Compilar y simular (desde la raíz del espacio de trabajo)

```powershell
$env:IDF_CONFIGDEP_ENABLE = "0"   # la ruta del espacio de trabajo contiene espacios
python submodule/esp-mosaico-utils/mosaico-tools/skills/idf-low-noise-build/scripts/idf_low_noise_build.py --project projects/servocraneo_gemini_direct --idf-path C:\esp\esp-idf-master build
python mosaico.py project sim --project projects/servocraneo_gemini_direct --interactive
```

El bootloader está excluido de la compilación por defecto: las aplicaciones normales
usan el bootloader retenido de Vibe Mode.

## Instalación y clave (solo con autorización del propietario)

1. Instalar con el flujo oficial: `python mosaico.py iris system-update --project projects/servocraneo_gemini_direct`.
2. Configurar el Wi-Fi en Vibe Mode (información compartida).
3. Copiar `.env.example` a `.env` (ignorado por Git) o exportar `GEMINI_API_KEY`, y ejecutar
   `python tools/provision_key.py` por USB. La clave se guarda en el NVS del dispositivo;
   nunca va en el código, los binarios ni GitHub.
4. Decir **«Hola ESP»** o tocar la pantalla: ámbar mientras conecta, rojo escuchando, verde cuando
   VOX responde. Un toque mientras habla lo interrumpe; otro toque cierra. Vuelve al reposo (cian)
   tras 15 s sin actividad. Con Google Search (1.2.0), VOX solo busca si se lo pides («busca…») o si
   desconoce un dato reciente; mientras busca el ojo sigue ámbar y no cita fuentes.

`app-update` no sirve en este dispositivo (`recovery_required`); usa siempre `system-update`.

Nunca subas `.env`, claves, tokens, `sdkconfig` ni binarios (`.gitignore` los excluye).
