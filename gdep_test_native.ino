// -----------------------------------------------------------------------------
//  Arduino + Waveshare ESP32 Driver Board + GxEPD2_7C (7-color E-paper 800×480)
//  Versione ottimizzata: nessun color mapping runtime
// -----------------------------------------------------------------------------

#include <Arduino.h>
#include <SPI.h>

#include <GxEPD2_7C.h>
#include <epd7c/GxEPD2_730c_GDEP073E01.h>

// Bitmap 7-color quantizzato offline
// Deve essere un array di uint8_t di dimensione IMG_W*IMG_H
// Ogni byte è un indice 0…6 nella palette PALETTE_GX
// Esempio generato offline con script Python/ImageMagick
#include "image.h"  // contiene: const uint8_t img_test[] PROGMEM

// ----------------- PIN (WAVESHARE ESP32 DRIVER BOARD - OFFICIAL) -----------------
static const int16_t EPD_CS   = 15;
static const int16_t EPD_DC   = 27;
static const int16_t EPD_RST  = 26;
static const int16_t EPD_BUSY = 25;

#define SPI_MOSI 14
#define SPI_CLK  13
#define SPI_MISO -1   // non collegato


// Altezza pagina “paged drawing”
static const uint16_t PAGE_HEIGHT = 32;

// Display instance GxEPD2 7-color
GxEPD2_7C<GxEPD2_730c_GDEP073E01, PAGE_HEIGHT> display(
  GxEPD2_730c_GDEP073E01(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// Dimensioni immagine
static const int16_t IMG_W = 800;
static const int16_t IMG_H = 480;

// Palette colori usata *solo per testo e riferimenti*
static const uint16_t PALETTE_GX[7] = {
  GxEPD_WHITE,
  GxEPD_BLACK,
  GxEPD_RED,
  GxEPD_GREEN,
  GxEPD_BLUE,
  GxEPD_YELLOW,
  GxEPD_ORANGE
};

// ---------------------- Funzione testo top-right ----------------------------
static void drawTextTopRight(const char* txt, int16_t yBaseline, uint16_t color)
{
  display.setTextSize(2);
  display.setTextWrap(false);

  int16_t tbx, tby;
  uint16_t tbw, tbh;
  display.getTextBounds(txt, 0, yBaseline, &tbx, &tby, &tbw, &tbh);

  const int16_t margin = 6;
  int16_t x = (int16_t)display.width() - (int16_t)tbw - margin;
  if (x < margin) x = margin;

  display.setCursor(x, yBaseline);
  display.setTextColor(color);
  display.print(txt);
}

// ---------------------- Disegna un frame -------------------------------
static void drawOneFrame()
{
  // Imposta orientamento (LANDSCAPE)
  display.setRotation(0);
  display.setFullWindow();

  const uint16_t pageH = display.pageHeight();
  uint16_t page = 0;

  display.firstPage();
  do
  {
    // Calcola range Y della pagina corrente
    int16_t y0 = (int16_t)page * (int16_t)pageH;
    int16_t y1 = y0 + (int16_t)pageH;
    if (y1 > IMG_H) y1 = IMG_H;

    // Disegna solo i pixel di questa pagina
    // Ogni pixel è un indice 0…6 nella bitmap prequantizzata
    for (int16_t y = y0; y < y1; y++)
    {
      uint32_t rowBase = (uint32_t)y * (uint32_t)IMG_W;
      for (int16_t x = 0; x < IMG_W; x++)
      {
        uint8_t idx = pgm_read_byte(&img_test[rowBase + (uint32_t)x]);
        uint16_t c = PALETTE_GX[idx];
        display.drawPixel(x, y, c);
      }
    }

    // Overlay testo (ritorna sulla pagina se tocca questo frame)
    const char* word = "prova";
    const int16_t firstY = 22;
    const int16_t stepY  = 26;
    for (int i = 0; i < 7; i++)
    {
      drawTextTopRight(word, firstY + i * stepY, PALETTE_GX[i]);
    }

    page++;
  }
  while (display.nextPage());

  display.powerOff();
}

void setup()
{
  Serial.begin(115200);
  delay(200);

  // Inizializza SPI hardware
  SPI.begin(SPI_CLK, SPI_MISO, SPI_MOSI, EPD_CS);

  // Inizializza display
  display.init(); 
  // Richiesto per pannello 7C nel driver GxEPD2
  display.epd2.setPaged(); 
}

void loop()
{
  drawOneFrame();
  delay(120000);
}
