// =============================================================
//  Lumi - Asistente de voz doméstico  (DIRECTO a OpenAI, sin servidor)
//  ESP32-S3 + INMP441 (mic) + MAX98357A (amp) + OLED SSD1306 (ojos)
// -------------------------------------------------------------
//  El ESP32 se conecta por sí mismo a la API Realtime de OpenAI por
//  WebSocket seguro (WSS). NO hace falta la PC ni el relay.
//
//  ⚠ La clave de OpenAI queda escrita en el firmware. No compartas
//    este archivo con tu clave dentro.
//
//  Requisitos (Arduino IDE):
//    - Placas "esp32" de Espressif core 3.x
//    - Librerías: WebSockets (Markus Sattler), Adafruit SSD1306,
//      Adafruit GFX, Adafruit BusIO
//    - Placa: "ESP32S3 Dev Module", PSRAM: "OPI PSRAM",
//      USB CDC On Boot: "Enabled"
// =============================================================

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s_std.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "mbedtls/base64.h"

// ================== CONFIGURACIÓN (RELLENA ESTO) ==================
const char* WIFI_SSID      = "TU_WIFI";          // <-- tu red Wi-Fi
const char* WIFI_PASS      = "TU_PASSWORD";      // <-- tu contraseña
const char* OPENAI_API_KEY = "sk-TU_CLAVE_AQUI"; // <-- tu clave de OpenAI
// =================================================================

const char*    OPENAI_HOST = "api.openai.com";
const uint16_t OPENAI_PORT = 443;
const char*    OPENAI_PATH = "/v1/realtime?model=gpt-realtime";

// ---------- Audio ----------
#define SAMPLE_RATE   24000

// ---------- Pines micrófono INMP441 ----------
#define MIC_SCK  5
#define MIC_WS   4
#define MIC_SD   6

// ---------- Pines amplificador MAX98357A ----------
#define AMP_BCLK 15
#define AMP_LRC  16
#define AMP_DIN  7

// Volumen del micrófono (16 bajo, 14 normal, 12 alto)
#define MIC_SHIFT 14

// ---------- Pantalla OLED SSD1306 (I2C) ----------
#define OLED_SDA   8
#define OLED_SCL   9
#define OLED_ADDR  0x3C
#define OLED_W     128
#define OLED_H     64

// Personalidad de Lumi + configuración de la sesión (JSON, formato GA).
// (sin acentos en el texto para evitar problemas de codificación; igual habla en español)
const char* SESSION_UPDATE =
  "{\"type\":\"session.update\",\"session\":{\"type\":\"realtime\","
  "\"instructions\":\"Eres Lumi, un asistente domestico amable y conversacional. "
  "Hablas siempre en espanol de forma natural, calida y breve. Respondes en 1 o 2 "
  "frases salvo que te pidan mas detalle. Si no sabes algo lo dices con honestidad.\","
  "\"audio\":{"
  "\"input\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},\"turn_detection\":{\"type\":\"server_vad\"}},"
  "\"output\":{\"format\":{\"type\":\"audio/pcm\",\"rate\":24000},\"voice\":\"coral\"}"
  "}}}";

// ---------- Handles I2S ----------
static i2s_chan_handle_t rx_handle = nullptr;
static i2s_chan_handle_t tx_handle = nullptr;

WebSocketsClient webSocket;
String authHeader;                 // se mantiene viva para setExtraHeaders
uint8_t* pcmInBuf = nullptr;       // buffer para decodificar audio entrante (PSRAM)
const size_t PCM_IN_SIZE = 24000;  // bytes

// ---------- Pantalla / ojos ----------
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
bool oledOk = false;
uint32_t lastDisplayMs = 0;
const int EYE_W = 34, EYE_H_MAX = 42, EYE_RADIUS = 10;
const int LEFT_CX = 40, RIGHT_CX = 88, EYE_CY = 32;

// ---------- Half-duplex ----------
volatile uint32_t lastPlaybackMs = 0;
const uint32_t    PLAYBACK_TAIL_MS = 400;

// =============================================================
//  I2S
// =============================================================
void setupMic() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, nullptr, &rx_handle);
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)MIC_SCK, .ws = (gpio_num_t)MIC_WS,
      .dout = I2S_GPIO_UNUSED, .din = (gpio_num_t)MIC_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
  Serial.println("[I2S] microfono listo");
}

void setupAmp() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, &tx_handle, nullptr);
  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)AMP_BCLK, .ws = (gpio_num_t)AMP_LRC,
      .dout = (gpio_num_t)AMP_DIN, .din = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  i2s_channel_init_std_mode(tx_handle, &std_cfg);
  i2s_channel_enable(tx_handle);
  Serial.println("[I2S] amplificador listo");
}

// =============================================================
//  Micrófono -> OpenAI  (PCM16 -> base64 -> JSON append)
// =============================================================
void loopMic() {
  if (millis() - lastPlaybackMs < PLAYBACK_TAIL_MS) return;   // half-duplex
  if (!webSocket.isConnected()) return;

  static int32_t raw[256];
  static int16_t pcm[256];
  size_t bytesRead = 0;
  if (i2s_channel_read(rx_handle, raw, sizeof(raw), &bytesRead, 20 / portTICK_PERIOD_MS) != ESP_OK || !bytesRead)
    return;

  int samples = bytesRead / sizeof(int32_t);
  for (int i = 0; i < samples; i++) {
    int32_t s = raw[i] >> MIC_SHIFT;
    if (s >  32767) s =  32767;
    if (s < -32768) s = -32768;
    pcm[i] = (int16_t)s;
  }

  static unsigned char b64[1024];
  size_t b64len = 0;
  if (mbedtls_base64_encode(b64, sizeof(b64), &b64len,
                            (unsigned char*)pcm, samples * 2) != 0) return;

  String out = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
  out += (const char*)b64;
  out += "\"}";
  webSocket.sendTXT(out);
}

// =============================================================
//  OpenAI -> altavoz  (extrae "delta" base64 -> PCM -> I2S)
// =============================================================
void handleEvent(char* msg) {
  if (strstr(msg, "output_audio.delta") || strstr(msg, "\"response.audio.delta\"")) {
    char* d = strstr(msg, "\"delta\":\"");
    if (!d) return;
    d += 9;
    char* end = strchr(d, '"');
    if (!end) return;
    size_t b64len = end - d;

    size_t outlen = 0;
    if (mbedtls_base64_decode(pcmInBuf, PCM_IN_SIZE, &outlen,
                              (const unsigned char*)d, b64len) == 0 && outlen > 0) {
      size_t written = 0;
      i2s_channel_write(tx_handle, pcmInBuf, outlen, &written, 300 / portTICK_PERIOD_MS);
      lastPlaybackMs = millis();
    }
  } else if (strstr(msg, "\"type\":\"error\"")) {
    Serial.printf("[OpenAI error] %s\n", msg);
  } else if (strstr(msg, "session.updated")) {
    Serial.println("[OpenAI] sesion lista");
  } else if (strstr(msg, "input_audio_buffer.speech_started")) {
    Serial.println("[OpenAI] te escucho...");
  }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] conectado a OpenAI");
      webSocket.sendTXT(SESSION_UPDATE);   // configura voz/idioma/VAD
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] desconectado");
      break;
    case WStype_TEXT:
      payload[length] = 0;                 // asegurar fin de cadena
      handleEvent((char*)payload);
      break;
    case WStype_ERROR:
      Serial.println("[WS] error");
      break;
    default:
      break;
  }
}

// =============================================================
//  Pantalla OLED: ojos animados
// =============================================================
void setupDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[OLED] no encontrada. Continuo sin pantalla.");
    oledOk = false;
    return;
  }
  oledOk = true;
  randomSeed(micros());
  display.clearDisplay();
  display.display();
  Serial.println("[OLED] lista");
}

void drawEye(int cx, int cy, int h) {
  if (h < 4) h = 4;
  int r = min(EYE_RADIUS, h / 2);
  display.fillRoundRect(cx - EYE_W / 2, cy - h / 2, EYE_W, h, r, SSD1306_WHITE);
}

void loopDisplay() {
  if (!oledOk) return;
  uint32_t now = millis();
  if (now - lastDisplayMs < 40) return;
  lastDisplayMs = now;

  bool talking = (now - lastPlaybackMs) < PLAYBACK_TAIL_MS;

  static uint32_t nextBlinkAt = 3000, blinkStart = 0;
  static bool blinking = false;
  if (!blinking && now > nextBlinkAt) { blinking = true; blinkStart = now; }
  if (blinking && now - blinkStart > 140) { blinking = false; nextBlinkAt = now + random(2500, 5000); }

  int base = talking ? (EYE_H_MAX - 12) : EYE_H_MAX;
  int hL = base, hR = base;

  if (talking) {
    float t = (now % 360) / 360.0f * TWO_PI;
    int pulse = (int)(sin(t) * 9);
    hL = base + pulse; hR = base + pulse;
  }
  if (blinking) {
    float p = (now - blinkStart) / 140.0f;
    float f = fabs(2.0f * p - 1.0f);
    hL = max(4, (int)(base * f)); hR = hL;
  }

  display.clearDisplay();
  drawEye(LEFT_CX,  EYE_CY, hL);
  drawEye(RIGHT_CX, EYE_CY, hR);
  display.display();
}

// =============================================================
//  setup / loop
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Lumi - ESP32-S3 (directo a OpenAI) ===");

  // Buffer de audio entrante en PSRAM si existe
  pcmInBuf = (uint8_t*)ps_malloc(PCM_IN_SIZE);
  if (!pcmInBuf) pcmInBuf = (uint8_t*)malloc(PCM_IN_SIZE);

  setupDisplay();
  setupMic();
  setupAmp();

  Serial.printf("Conectando a Wi-Fi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.print("\nWi-Fi OK. IP: ");
  Serial.println(WiFi.localIP());

  authHeader = String("Authorization: Bearer ") + OPENAI_API_KEY;
  webSocket.beginSSL(OPENAI_HOST, OPENAI_PORT, OPENAI_PATH);
  webSocket.setExtraHeaders(authHeader.c_str());
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(3000);
  webSocket.enableHeartbeat(15000, 3000, 2);   // mantiene viva la conexión
  Serial.println("Conectando a OpenAI (WSS)...");
}

void loop() {
  webSocket.loop();
  loopMic();
  loopDisplay();
}
