#include <Arduino.h>
#include <SPI.h>

#include <GxEPD2_7C.h>                 // include anche i driver 7C (tra cui GDEP073E01)
#include "image.h"                      // contiene Img_test[] in PROGMEM + IMG_W / IMG_H

// Pin Waveshare ESP32 Driver Board
static constexpr int EPD_MOSI = 14;  // DIN
static constexpr int EPD_SCK  = 13;  // CLK
static constexpr int EPD_CS   = 15;
static constexpr int EPD_DC   = 27;
static constexpr int EPD_RST  = 26;
static constexpr int EPD_BUSY = 25;


// === usa HSPI (consigliato dalla doc/esempi per questa board) + pin remap ===
SPIClass hspi(HSPI);

// SPISettings più conservativo per stabilità (poi puoi alzare)
static SPISettings epdSPI(4000000, MSBFIRST, SPI_MODE0);


// ====== IMPORTANTISSIMO PER LA RAM ======
// Invece di usare un buffer a piena altezza (che va in .dram0.bss e satura la DRAM),
// usiamo un page buffer piccolo (es. 16 o 32 righe).
// 16 righe: ~800*16/2 = 6400 bytes (4bpp -> 2 pixel/byte), molto più leggero.
// Se vuoi un po' più veloce, prova 32 (RAM raddoppia, ma ancora ok).
static constexpr uint16_t PAGE_HEIGHT = 32;

// Display: 7.3" 7-color 800x480 GoodDisplay GDEP073E01
GxEPD2_7C<GxEPD2_730c_GDEP073E01, PAGE_HEIGHT> display(
  GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);


struct ColorLine { uint16_t color; const char* text; };

// Colori disponibili in GxEPD2_7C per 7-color
static const ColorLine kLines[5] = {
  //{ GxEPD_BLACK,  "black" },
  //{ GxEPD_WHITE,  "white" },  // su sfondo chiaro potrebbe vedersi poco: metto un riquadro dietro
  { GxEPD_GREEN,  "green" },
  { GxEPD_BLUE,   "blue" },
  { GxEPD_RED,    "red" },
  { GxEPD_YELLOW, "yellow" },
  { GxEPD_ORANGE, "orange" },
};

// Disegna le 7 righe in alto a destra.
// Nota: "alto a destra" in coordinate display (landscape).
void drawOverlayTextTopRight()
{
  // Impostazioni testo
  display.setTextSize(2);
  display.setFont(nullptr);            // font di default (più leggero)
  display.setTextWrap(false);

  const int16_t margin = 10;
  const int16_t line_h = 22;           // distanza verticale tra righe (adatta a textSize=2)
  int16_t y = margin + line_h;

  for (int i = 0; i < 7; i++)
  {
    const char* s = kLines[i].text;

    // calcolo bounding box del testo per allinearlo a destra
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);

    // Allineamento a destra sul display
    int16_t x = display.width() - margin - (int16_t)w;

    // Per la riga bianca, disegno un fondo nero per renderla leggibile
    if (kLines[i].color == GxEPD_WHITE)
    {
      display.fillRect(x - 4, y - (int16_t)h, (int16_t)w + 8, (int16_t)h + 6, GxEPD_BLACK);
    }

    display.setCursor(x, y);
    display.setTextColor(kLines[i].color);
    display.print(s);

    y += line_h;
  }
}

// L'immagine viene mostrata a partire dall'angolo in alto a sinistra (0,0), qualsiasi dimensione IMG_W x IMG_H.
void renderFrame()
{
  // Posizione immagine: top-left
  const int16_t x0 = 0;
  const int16_t y0 = 0;

  // Finestra completa (landscape)
  display.setFullWindow();

  // Disegno paginato: con PAGE_HEIGHT piccolo non sforiamo la DRAM
  display.firstPage();
  do
  {
    // Pulizia sfondo: utile se IMG_W/IMG_H non coprono tutto il display
    //display.fillScreen(GxEPD_WHITE);

    // 1) Disegna l'immagine nativa 7-color dal PROGMEM nel buffer grafico
    //    Img_test è in formato 4bpp nibble-packed (2 pixel/byte) per GxEPD2_7C.
    //    Parametri tipici:
    //    - invert = false
    //    - mirror_y = false
    //    - pgm = true (dato che Img_test è in PROGMEM)

    display.drawImage(Img_test, x0, y0, IMG_W, IMG_H, false, false, true);
    //display.fillScreen(GxEPD_RED);
    // 2) Overlay testo in alto a destra (7 righe / 7 colori)
    drawOverlayTextTopRight();
  }
  while (display.nextPage());
}

void setup()
{
  Serial.begin(115200);
  delay(50);

  // HSPI remap: SCK=13, MISO=12 (non usato), MOSI=14, SS=15
  hspi.begin(EPD_SCK, 12, EPD_MOSI, EPD_CS);

  // IMPORTANT: reset pulse corto 2ms per Waveshare “clever reset circuit”
  // e init esteso che seleziona lo SPI giusto + settings
  display.init(115200, true, 2, false, hspi, epdSPI);

  // Landscape (con 800x480 spesso rotation(0) è già ok)
  display.setRotation(0);

  Serial.print("Display W x H = ");
  Serial.print(display.width());
  Serial.print(" x ");
  Serial.println(display.height());

  Serial.print("Image  W x H = ");
  Serial.print(IMG_W);
  Serial.print(" x ");
  Serial.println(IMG_H);

  //renderFrame();
}

void loop()
{
 for (const ColorLine& c : kLines)
  {
    display.setFullWindow();
    display.firstPage();
    do
    {
      display.fillScreen(c.color);

      Serial.print("Color: ");
      Serial.println(c.text);
    }
    while (display.nextPage());


    delay(20000);
  }
  //renderFrame();
}