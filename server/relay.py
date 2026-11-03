#!/usr/bin/env python3
# =============================================================
#  Lumi - Servidor relay
# -------------------------------------------------------------
#  Hace de puente entre el ESP32-S3 y la API Realtime de OpenAI.
#  La clave de OpenAI vive AQUÍ (en .env), nunca en el firmware.
#
#    ESP32  --WebSocket(PCM16 24kHz binario)-->  relay  -->  OpenAI
#    ESP32  <--WebSocket(audio binario)--------  relay  <--  OpenAI
# =============================================================

import asyncio
import base64
import json
import os

import websockets
from dotenv import load_dotenv

load_dotenv()

OPENAI_API_KEY = os.environ["OPENAI_API_KEY"]
MODEL = os.environ.get("OPENAI_MODEL", "gpt-realtime")
VOICE = os.environ.get("VOICE", "coral")
HOST = os.environ.get("RELAY_HOST", "0.0.0.0")
PORT = int(os.environ.get("RELAY_PORT", "8765"))

OPENAI_URL = f"wss://api.openai.com/v1/realtime?model={MODEL}"

# Personalidad de Lumi (system prompt, en español)
INSTRUCTIONS = (
    "Eres Lumi, un asistente doméstico amable y conversacional. "
    "Hablas siempre en español de forma natural, cálida y breve. "
    "Respondes en 1 o 2 frases salvo que te pidan más detalle. "
    "Si no sabes algo, lo dices con honestidad."
)

# Configuración de la sesión (formato GA de la API Realtime: bloque audio anidado).
SESSION_UPDATE = {
    "type": "session.update",
    "session": {
        "type": "realtime",
        "instructions": INSTRUCTIONS,
        "audio": {
            "input": {
                "format": {"type": "audio/pcm", "rate": 24000},
                "turn_detection": {"type": "server_vad"},
            },
            "output": {
                "format": {"type": "audio/pcm", "rate": 24000},
                "voice": VOICE,
            },
        },
    },
}


async def handle_device(device_ws):
    """Una conexión del ESP32 -> abre una sesión con OpenAI y las conecta."""
    print("[relay] ESP32 conectado")

    headers = {"Authorization": f"Bearer {OPENAI_API_KEY}"}
    # Si usas un modelo *beta* en vez del GA, descomenta la línea siguiente:
    # headers["OpenAI-Beta"] = "realtime=v1"

    try:
        async with websockets.connect(
            OPENAI_URL, additional_headers=headers, max_size=None
        ) as oai:
            await oai.send(json.dumps(SESSION_UPDATE))

            async def device_to_openai():
                # Audio binario del ESP32 -> base64 -> OpenAI
                async for msg in device_ws:
                    if isinstance(msg, (bytes, bytearray)):
                        audio_b64 = base64.b64encode(msg).decode("ascii")
                        await oai.send(json.dumps({
                            "type": "input_audio_buffer.append",
                            "audio": audio_b64,
                        }))

            async def openai_to_device():
                # Eventos de OpenAI -> audio binario al ESP32
                async for raw in oai:
                    event = json.loads(raw)
                    etype = event.get("type", "")

                    # Aceptamos el nombre GA y el beta por compatibilidad.
                    if etype in ("response.output_audio.delta", "response.audio.delta"):
                        pcm = base64.b64decode(event["delta"])
                        await device_ws.send(pcm)

                    elif etype == "error":
                        print("[openai][error]", json.dumps(event, ensure_ascii=False))

                    elif etype in ("session.created", "session.updated"):
                        print("[openai]", etype)

                    elif etype == "input_audio_buffer.speech_started":
                        print("[openai] usuario hablando...")

                    elif etype == "response.done":
                        print("[openai] respuesta completa")

            await asyncio.gather(device_to_openai(), openai_to_device())

    except websockets.ConnectionClosed:
        print("[relay] conexión cerrada")
    except Exception as e:  # noqa: BLE001
        print("[relay][error]", repr(e))
    finally:
        print("[relay] ESP32 desconectado")


async def main():
    print(f"[relay] escuchando en ws://{HOST}:{PORT}  (modelo={MODEL}, voz={VOICE})")
    async with websockets.serve(handle_device, HOST, PORT, max_size=None):
        await asyncio.Future()  # corre para siempre


if __name__ == "__main__":
    asyncio.run(main())
