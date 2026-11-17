// ============================================================
//  Medidor de humedad con gráfico en línea + registro en SD
//  Placa: Arduino Mega 2560 + pantalla MCUFRIEND (paralela)
//
//  Tarjeta SD: se usa el lector que trae la pantalla (pines 10-13)
//  mediante SPI por software (en el Mega el SPI real está en 50-52).
//
//  Por ahora graba solo el % de humedad (sin fecha/hora).
//
//  Librerías (Gestor de librerías del IDE):
//   - Adafruit GFX, MCUFRIEND_kbv  (ya las tienes)
//   - SdFat (de Bill Greiman)  -> ver paso obligatorio abajo
//
//  PASO OBLIGATORIO (una sola vez):
//   Abre  Documentos/Arduino/libraries/SdFat/src/SdFatConfig.h
//   busca la línea   #define SPI_DRIVER_SELECT 0
//   y cámbiala a     #define SPI_DRIVER_SELECT 2
// ============================================================
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>
#include <SdFat.h>

#if SPI_DRIVER_SELECT != 2
#error "Edita SdFatConfig.h y pon SPI_DRIVER_SELECT en 2 (ver comentario arriba)"
#endif

#define PIN_HUMEDAD A8    // Señal sensor(amarillo) A8; (Alimentacion sensor: GND(Negro) en GND Sobre 53; VCD(Rojo) en 22)
#define VALOR_SECO  584   // lectura al aire
#define VALOR_AGUA  227   // lectura en agua

// Pines del lector SD de la pantalla
const uint8_t SD_CS   = 10;
const uint8_t SD_MOSI = 11;
const uint8_t SD_MISO = 12;
const uint8_t SD_SCK  = 13;
SoftSpiDriver<SD_MISO, SD_MOSI, SD_SCK> softSpi;
#define SD_CONFIG SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(0), &softSpi)
SdFat32 sd;

#define ARCHIVO     "HUMEDAD.TXT"   // nombre máx. 8 caracteres + extensión

// ---- Tiempos (ajustables) ----
#define INTERVALO_LECTURA  1000UL  // ms entre lecturas (1 segundo)
#define SEG_POR_PUNTO      1       // cada cuántos segundos se agrega un punto al gráfico
#define SEG_POR_REGISTRO   2      // cada cuántos segundos se graba una línea en la SD (ej. 60 = 1 por minuto)

// ---- Colores ----
#define NEGRO    0x0000
#define BLANCO   0xFFFF
#define ROJO     0xF800
#define VERDE    0x07E0
#define GRIS     0x7BEF
#define GRIS_OSC 0x39E7
#define AMARILLO 0xFFE0

MCUFRIEND_kbv tft;

bool sdOK  = false;
unsigned long nRegistros = 0;    // cuántas líneas se han grabado

// ---- Zona del gráfico ----
int16_t GX, GY, GW, GH;          // posición y tamaño del recuadro
#define MAX_PUNTOS 480
int16_t hist[MAX_PUNTOS];        // historial en décimas de % (453 = 45.3 %)
int16_t nPuntos   = 0;
int16_t maxPuntos = 0;

unsigned long tUltimaLectura = 0;
unsigned long contador = 0;
float sumaPunto = 0;
int   nSumaPunto = 0;

// ------------------------------------------------------------
uint16_t colorDe(float pct) {
  // rojo <= 30, verde entre 30.1 y 70, rojo > 70
  if (pct <= 30.0 || pct > 70.0) return ROJO;
  return VERDE;
}

int16_t yDe(int16_t decimas) {   // convierte 0..1000 a coordenada Y
  return GY + GH - 2 - (long)decimas * (GH - 3) / 1000;
}

void lineaPunteada(int16_t y, uint16_t c) {
  for (int16_t x = GX + 1; x < GX + GW - 1; x += 6)
    tft.drawFastHLine(x, y, 3, c);
}

void dibujarGrilla() {
  lineaPunteada(yDe(700), ROJO);      // límite 70 %
  lineaPunteada(yDe(500), GRIS_OSC);  // 50 %
  lineaPunteada(yDe(300), ROJO);      // límite 30 %
}

void dibujarMarco() {
  tft.drawRect(GX, GY, GW, GH, GRIS);
  tft.setTextSize(1);
  tft.setTextColor(GRIS, NEGRO);
  const int niveles[] = {100, 70, 50, 30, 0};
  for (int i = 0; i < 5; i++) {
    tft.setCursor(GX - 24, yDe(niveles[i] * 10) - 3);
    if (niveles[i] < 100) tft.print(" ");
    if (niveles[i] < 10)  tft.print(" ");
    tft.print(niveles[i]);
    tft.print("%");
  }
  // Texto bajo el gráfico con la ventana de tiempo visible
  tft.setCursor(GX, GY + GH + 6);
  tft.print("Ultimos ");
  tft.print((unsigned long)maxPuntos * SEG_POR_PUNTO / 60);
  tft.print(" min  (1 punto cada ");
  tft.print(SEG_POR_PUNTO);
  tft.print(" s)");
  dibujarGrilla();
}

void dibujarCurva(bool borrar) {
  if (nPuntos == 1) {
    tft.drawPixel(GX + 1, yDe(hist[0]), borrar ? NEGRO : colorDe(hist[0] / 10.0));
    return;
  }
  for (int16_t i = 1; i < nPuntos; i++) {
    uint16_t c = borrar ? NEGRO : colorDe(hist[i] / 10.0);
    tft.drawLine(GX + i, yDe(hist[i - 1]), GX + 1 + i, yDe(hist[i]), c);
  }
}

void agregarPunto(float pct) {
  dibujarCurva(true);                          // borra la curva anterior
  if (nPuntos >= maxPuntos) {                  // si está lleno, desplaza a la izquierda
    memmove(hist, hist + 1, (maxPuntos - 1) * sizeof(int16_t));
    nPuntos--;
  }
  hist[nPuntos++] = (int16_t)(pct * 10.0 + 0.5);
  dibujarGrilla();
  dibujarCurva(false);                         // dibuja la nueva
}

void dibujarEstado() {
  tft.setTextSize(1);
  tft.setCursor(5, 36);
  tft.setTextColor(sdOK ? VERDE : ROJO, NEGRO);
  tft.print(sdOK ? "SD:OK   " : "SD:ERROR");
  tft.setCursor(130, 36);
  tft.setTextColor(BLANCO, NEGRO);
  tft.print("Grabados: ");
  tft.print(nRegistros);
}

void dibujarEncabezado(float pct, int valor) {
  char buf[12];

  // Valor actual grande, con color según rango
  tft.setTextSize(3);
  tft.setTextColor(colorDe(pct), NEGRO);
  tft.setCursor(5, 5);
  dtostrf(pct, 5, 1, buf);
  tft.print(buf);
  tft.print(" %");

  // Raw
  tft.setTextSize(1);
  tft.setTextColor(BLANCO, NEGRO);
  tft.setCursor(70, 36);
  sprintf(buf, "Raw:%4d", valor);
  tft.print(buf);
}

void grabarRegistro(float pct) {
  char linea[10];
  dtostrf(pct, 1, 1, linea);       // solo el %, ej: 45.3

  Serial.print("Grabado: ");
  Serial.println(linea);           // también sale por el monitor serie

  if (!sdOK) return;
  File32 f;
  if (f.open(ARCHIVO, O_WRONLY | O_CREAT | O_APPEND)) {
    f.println(linea);
    f.close();                     // cerrar siempre: así no se pierden datos si se corta la luz
    nRegistros++;
    dibujarEstado();
  } else {
    sdOK = false;                  // tarjeta retirada o con falla
    dibujarEstado();
  }
}

float leerHumedad(int &valor) {
  // Promedio de 10 lecturas para reducir ruido
  long suma = 0;
  for (int i = 0; i < 10; i++) {
    suma += analogRead(PIN_HUMEDAD);
    delay(10);
  }
  valor = suma / 10;
  float porcentaje = (float)(valor - VALOR_SECO) * 100.0 / (VALOR_AGUA - VALOR_SECO);
  return constrain(porcentaje, 0.0, 100.0);
}

// ------------------------------------------------------------
void setup() {
  Serial.begin(9600);
  Serial.println("Medidor de Humedad");

  uint16_t id = tft.readID();
  if (id == 0xD3D3) id = 0x9486;
  tft.begin(id);
  tft.setRotation(1);
  tft.fillScreen(NEGRO);

  // Tarjeta SD
  sdOK = sd.begin(SD_CONFIG);
  if (sdOK && !sd.exists(ARCHIVO)) {
    File32 f;
    if (f.open(ARCHIVO, O_WRONLY | O_CREAT)) {
      f.println("Humedad(%)");
      f.close();
    }
  }
  Serial.println(sdOK ? "SD OK" : "SD no detectada");

  // Medidas del gráfico (se adapta a pantallas 320x240 o 480x320)
  GX = 30;
  GY = 50;
  GW = tft.width()  - GX - 5;
  GH = tft.height() - GY - 18;
  maxPuntos = GW - 2;
  if (maxPuntos > MAX_PUNTOS) maxPuntos = MAX_PUNTOS;

  dibujarMarco();
  dibujarEstado();
}

void loop() {
  if (millis() - tUltimaLectura < INTERVALO_LECTURA) return;
  tUltimaLectura = millis();
  contador++;

  int valor;
  float porcentaje = leerHumedad(valor);

  Serial.print("Raw: ");
  Serial.print(valor);
  Serial.print("  Humedad: ");
  Serial.print(porcentaje, 1);
  Serial.println(" %");

  dibujarEncabezado(porcentaje, valor);

  // Gráfico: promedio de las lecturas de cada intervalo
  sumaPunto += porcentaje;
  nSumaPunto++;
  if (contador % SEG_POR_PUNTO == 0) {
    agregarPunto(sumaPunto / nSumaPunto);
    sumaPunto = 0;
    nSumaPunto = 0;
  }

  // Registro en archivo
  if (contador % SEG_POR_REGISTRO == 0) {
    grabarRegistro(porcentaje);
  }
}
