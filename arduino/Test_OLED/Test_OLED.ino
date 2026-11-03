// =============================================================
//  Test OLED SSD1306 (I2C) para Lumi
//  Sirve para comprobar que la pantalla funciona y en qué
//  dirección responde (0x3C o 0x3D).
//
//  Conexión:  SDA -> GPIO 8 , SCL -> GPIO 9 , VCC -> 3V3 , GND -> GND
//  Abre el Monitor Serie a 115200 para ver el resultado.
// =============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define OLED_SDA 8
#define OLED_SCL 9
#define OLED_W   128
#define OLED_H   64

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
uint8_t foundAddr = 0;

// Escanea el bus I2C e imprime las direcciones encontradas
uint8_t scanI2C() {
  uint8_t addr = 0;
  Serial.println("Escaneando I2C...");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  dispositivo encontrado en 0x%02X\n", a);
      if (a == 0x3C || a == 0x3D) addr = a;   // típicas de la OLED
    }
  }
  if (addr == 0) Serial.println("  (no se encontro ninguna OLED)");
  return addr;
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Test OLED ===");

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(400000);

  foundAddr = scanI2C();
  if (foundAddr == 0) {
    Serial.println("Revisa el cableado: SDA->8, SCL->9, VCC->3V3, GND->GND");
    Serial.println("(prueba tambien intercambiar SDA y SCL)");
    return;
  }

  Serial.printf("Inicializando OLED en 0x%02X...\n", foundAddr);
  if (!display.begin(SSD1306_SWITCHCAPVCC, foundAddr)) {
    Serial.println("display.begin FALLO. Si tu pantalla es SH1106, necesita otra libreria.");
    return;
  }
  Serial.println("OLED OK. Deberias ver texto y formas en la pantalla.");

  // --- Prueba 1: texto ---
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(10, 8);
  display.println("OLED OK");
  display.setTextSize(1);
  display.setCursor(10, 40);
  display.printf("addr 0x%02X", foundAddr);
  display.display();
  delay(2500);

  // --- Prueba 2: marco + relleno ---
  display.clearDisplay();
  display.drawRect(0, 0, OLED_W, OLED_H, SSD1306_WHITE);
  display.fillRect(20, 20, 40, 24, SSD1306_WHITE);
  display.fillCircle(95, 32, 16, SSD1306_WHITE);
  display.display();
  delay(2500);
}

// --- Prueba 3: ojos animados (bucle) ---
void loop() {
  if (foundAddr == 0) return;

  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 40) return;
  last = now;

  static uint32_t nextBlink = 3000, blinkStart = 0;
  static bool blinking = false;
  if (!blinking && now > nextBlink) { blinking = true; blinkStart = now; }
  if (blinking && now - blinkStart > 140) { blinking = false; nextBlink = now + random(2500, 5000); }

  int h = 42;
  if (blinking) {
    float p = (now - blinkStart) / 140.0f;
    h = max(4, (int)(42 * fabs(2.0f * p - 1.0f)));
  }

  display.clearDisplay();
  int r = min(10, h / 2);
  display.fillRoundRect(40 - 17, 32 - h / 2, 34, h, r, SSD1306_WHITE);
  display.fillRoundRect(88 - 17, 32 - h / 2, 34, h, r, SSD1306_WHITE);
  display.display();
}
