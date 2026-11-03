// =============================================================
//  Lumi - Asistente de voz doméstico
//  Firmware ESP32-S3  +  INMP441 (mic)  +  MAX98357A (amplificador)
// -------------------------------------------------------------
//  El ESP32 solo captura y reproduce audio. Toda la IA (voz->texto,
//  LLM y texto->voz) vive en el servidor relay (ver carpeta /server),
//  que a su vez habla con la API Realtime de OpenAI.
//
//  Flujo:
//    INMP441 --I2S--> ESP32 --WebSocket(PCM16 24kHz)--> relay --> OpenAI
//    MAX98357A <-I2S-- ESP32 <--WebSocket(audio)------- relay <-- OpenAI
// =============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <driver/i2s_std.h>

// ================== CONFIGURACIÓN (RELLENA ESTO) ==================
const char*    WIFI_SSID  = "TU_WIFI";          // <-- tu red Wi-Fi
const char*    WIFI_PASS  = "TU_PASSWORD";      // <-- tu contraseña
const char*    RELAY_HOST = "192.168.1.100";    // <-- IP de tu PC/servidor relay
const uint16_t RELAY_PORT = 8765;               // puerto del relay
const char*    RELAY_PATH = "/";
// =================================================================

// ---------- Audio ----------
#define SAMPLE_RATE   24000   // Hz (formato que pide OpenAI Realtime)

// ---------- Pines micrófono INMP441 ----------
#define MIC_SCK  5    // SCK  (bit clock)
#define MIC_WS   4    // WS / "Vs" (word select / LR clock)
#define MIC_SD   6    // SD   (data out del micrófono)

// ---------- Pines amplificador MAX98357A ----------
#define AMP_BCLK 15   // BCLK (bit clock)
#define AMP_LRC  16   // LRC  (word select / LR clock)
#define AMP_DIN  7    // DIN  (data in del amplificador)

// INMP441: con el pin L/R a GND el dato sale en el canal IZQUIERDO.
// Sube/baja este shift para ajustar el VOLUMEN del micrófono:
//   16 = conversión exacta 24->16 bit (más bajo)
//   14 = ~4x de ganancia (valor por defecto)
//   12 = ~16x (puede saturar)
#define MIC_SHIFT 14

// ---------- Handles I2S ----------
static i2s_chan_handle_t rx_handle = nullptr;  // micrófono (entrada)
static i2s_chan_handle_t tx_handle = nullptr;  // amplificador (salida)

WebSocketsClient webSocket;

// ---------- Half-duplex (evita que el mic capte al altavoz) ----------
// Mientras suena la respuesta, NO enviamos audio del micrófono.
volatile uint32_t lastPlaybackMs = 0;
const uint32_t    PLAYBACK_TAIL_MS = 400;  // cola de silencio tras reproducir

// =============================================================
//  Configuración I2S
// =============================================================
void setupMic() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, nullptr, &rx_handle);  // solo RX

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_SCK,
      .ws   = (gpio_num_t)MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din  = (gpio_num_t)MIC_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  // INMP441 con L/R -> GND = canal izquierdo
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);
  Serial.println("[I2S] micrófono listo");
}

void setupAmp() {
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, &tx_handle, nullptr);  // solo TX

  i2s_std_config_t std_cfg = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)AMP_BCLK,
      .ws   = (gpio_num_t)AMP_LRC,
      .dout = (gpio_num_t)AMP_DIN,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };

  i2s_channel_init_std_mode(tx_handle, &std_cfg);
  i2s_channel_enable(tx_handle);
  Serial.println("[I2S] amplificador listo");
}

// =============================================================
//  Lectura del micrófono y envío al relay
// =============================================================
void loopMic() {
  // Half-duplex: no capturamos mientras suena (o acaba de sonar) la respuesta.
  if (millis() - lastPlaybackMs < PLAYBACK_TAIL_MS) return;
  if (!webSocket.isConnected()) return;

  static int32_t raw[256];   // muestras crudas de 32 bit del INMP441
  static int16_t pcm[256];   // muestras PCM16 para enviar

  size_t bytesRead = 0;
  esp_err_t r = i2s_channel_read(rx_handle, raw, sizeof(raw),
                                 &bytesRead, 20 / portTICK_PERIOD_MS);
  if (r != ESP_OK || bytesRead == 0) return;

  int samples = bytesRead / sizeof(int32_t);
  for (int i = 0; i < samples; i++) {
    int32_t s = raw[i] >> MIC_SHIFT;       // 24 bit -> 16 bit (+ ganancia)
    if (s >  32767) s =  32767;            // recorte anti-saturación
    if (s < -32768) s = -32768;
    pcm[i] = (int16_t)s;
  }

  webSocket.sendBIN((uint8_t*)pcm, samples * sizeof(int16_t));
}

// =============================================================
//  Eventos WebSocket (recibe audio de respuesta -> altavoz)
// =============================================================
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] conectado al relay");
      break;

    case WStype_DISCONNECTED:
      Serial.println("[WS] desconectado");
      break;

    case WStype_BIN: {
      // Audio de respuesta (PCM16 24kHz) -> directo al amplificador
      size_t written = 0;
      i2s_channel_write(tx_handle, payload, length, &written,
                        100 / portTICK_PERIOD_MS);
      lastPlaybackMs = millis();
      break;
    }

    case WStype_TEXT:
      Serial.printf("[WS] texto: %.*s\n", (int)length, payload);
      break;

    default:
      break;
  }
}

// =============================================================
//  setup / loop
// =============================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Lumi - ESP32-S3 ===");

  setupMic();
  setupAmp();

  Serial.printf("Conectando a Wi-Fi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.print("\nWi-Fi OK. IP: ");
  Serial.println(WiFi.localIP());

  webSocket.begin(RELAY_HOST, RELAY_PORT, RELAY_PATH);
  webSocket.onEvent(onWsEvent);
  webSocket.setReconnectInterval(2000);
  Serial.printf("Conectando al relay ws://%s:%u%s\n", RELAY_HOST, RELAY_PORT, RELAY_PATH);
}

void loop() {
  webSocket.loop();   // mantiene viva la conexión y procesa audio entrante
  loopMic();          // captura y envía audio del micrófono
}
