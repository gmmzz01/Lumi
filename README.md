# Lumi 🤖

Asistente de voz doméstico en **español** sobre **ESP32-S3**, con la IA en la
nube (**OpenAI Realtime API**).

El ESP32 solo captura y reproduce audio. El trabajo de IA (voz→texto, LLM y
texto→voz) lo hace la API Realtime, a través de un pequeño **servidor relay**
propio que guarda la clave de OpenAI de forma segura.

```
INMP441 ──I2S──► ESP32-S3 ──WebSocket(PCM16 24kHz)──► relay ──► OpenAI Realtime
MAX98357A ◄─I2S── ESP32-S3 ◄──WebSocket(audio)─────── relay ◄── OpenAI Realtime
```

- **Voz:** `coral`
- **Idioma:** español
- **Turnos:** VAD del servidor (hablas y detecta solo cuándo empiezas/terminas)
- **Nombre/personalidad:** "Lumi" (editable en `server/relay.py`)

---

## 1. Hardware y conexiones

| INMP441 (micrófono) | ESP32-S3 |
|---|---|
| SD  | GPIO 6 |
| WS (Vs) | GPIO 4 |
| SCK | GPIO 5 |
| VDD | 3V3 |
| GND | GND |
| L/R | GND (canal izquierdo) |

| MAX98357A (amplificador) | ESP32-S3 |
|---|---|
| DIN  | GPIO 7 |
| LRC  | GPIO 16 |
| BCLK | GPIO 15 |
| Vin  | 5V |
| GND  | GND |
| +/−  | altavoz (4–8 Ω) |

| OLED SSD1306 128x64 (I2C) | ESP32-S3 |
|---|---|
| SDA | GPIO 8 |
| SCL | GPIO 9 |
| VCC | 3V3 |
| GND | GND |

> El micrófono y el amplificador van en buses I2S separados (el S3 tiene dos),
> así pueden funcionar a la vez. La pantalla es **opcional**: si no está
> conectada, el firmware lo detecta y sigue funcionando sin ella.
> Dirección I2C por defecto `0x3C` (si la tuya es `0x3D`, cámbiala en
> `OLED_ADDR` dentro de `main.cpp`).

### Ojos animados

En la pantalla OLED aparecen dos ojos que **parpadean** en reposo y **vibran al
ritmo de la voz** mientras Lumi habla. No hay que configurar nada: se activan
solos si la pantalla está conectada.

---

## 2. Servidor relay (tu PC)

Necesitas **Python 3.10+**.

```bash
cd server
python -m venv .venv
source .venv/bin/activate        # Windows: .venv\Scripts\activate
pip install -r requirements.txt

cp .env.example .env             # Windows: copy .env.example .env
# edita .env y pon tu OPENAI_API_KEY

python relay.py
```

Deberías ver:

```
[relay] escuchando en ws://0.0.0.0:8765  (modelo=gpt-realtime, voz=coral)
```

Anota la **IP local de tu PC** (ej. `192.168.1.100`): la necesita el ESP32.
- Windows: `ipconfig`
- macOS/Linux: `ip addr` o `ifconfig`

---

## 3. Firmware (ESP32-S3)

Con **PlatformIO** (VS Code o CLI):

1. Abre la carpeta `firmware/`.
2. En `src/main.cpp`, rellena:
   - `WIFI_SSID` y `WIFI_PASS`
   - `RELAY_HOST` = la IP local de tu PC
3. Compila y sube:
   ```bash
   cd firmware
   pio run -t upload
   pio device monitor
   ```

El monitor serie debe mostrar `Wi-Fi OK`, la IP del ESP32 y
`[WS] conectado al relay`.

> El firmware usa Arduino core 3.x (ESP-IDF 5.x) mediante la plataforma
> **pioarduino**, necesaria para el driver I2S nuevo (`i2s_std.h`). Eso ya está
> configurado en `platformio.ini`.

---

## 4. Uso

Con el relay corriendo y el ESP32 encendido: **habla**. El VAD del servidor
detecta tu voz, OpenAI responde y Lumi contesta por el altavoz.

---

## 5. Ajustes útiles

| Qué | Dónde |
|---|---|
| Personalidad / nombre | `INSTRUCTIONS` en `server/relay.py` |
| Voz | `VOICE` en `server/.env` |
| Volumen del micrófono | `MIC_SHIFT` en `firmware/src/main.cpp` (14 = normal; 12 = más alto) |
| Cola de silencio tras hablar | `PLAYBACK_TAIL_MS` en `main.cpp` |

---

## 6. Limitaciones conocidas (base v1)

- **Medio dúplex:** mientras Lumi habla, el micrófono se silencia (evita que se
  oiga a sí mismo). Por eso aún **no puedes interrumpirlo** por voz. Para dúplex
  completo haría falta cancelación de eco (AEC).
- **Sin palabra de activación:** escucha de forma continua. Se puede añadir
  después con ESP-SR (WakeNet) en el ESP32.
- **Depende de la nube:** si no hay internet o se cae el relay, no funciona.
- **Costo:** la API Realtime de OpenAI se cobra por uso.

---

## 7. Estructura

```
lumi/
├── firmware/              # ESP32-S3 (PlatformIO)
│   ├── platformio.ini
│   └── src/main.cpp
└── server/                # relay en Python
    ├── relay.py
    ├── requirements.txt
    └── .env.example
```

> Nota: la API Realtime de OpenAI evoluciona rápido. Si un evento o campo
> cambia, revisa la referencia oficial. El relay ya acepta los nombres de
> evento GA y beta por compatibilidad.
