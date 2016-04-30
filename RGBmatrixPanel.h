#if ARDUINO >= 100
 #include "Arduino.h"
#else
 #include "WProgram.h"
 #include "pins_arduino.h"
#endif
#include "Adafruit_GFX.h"

	#define CLK 19
	#define LAT 18
	#define OE	17
	#define A   23
	#define B   22
	#define C   21
	#define D   20

class RGBmatrixPanel : public Adafruit_GFX {

 public:
	 RGBmatrixPanel(boolean dbuf, uint8_t width, uint8_t height, uint8_t panel_x, uint8_t panel_y, uint8_t nRows = 8);

  void
    begin(void),
    drawPixel(int16_t x, int16_t y, uint16_t c),
    fillScreen(uint16_t c),
    updateDisplay(void),
    swapBuffers(boolean),
    dumpMatrix(void);
  uint8_t
    *backBuffer(void);
  uint16_t
    Color333(uint8_t r, uint8_t g, uint8_t b),
    Color444(uint8_t r, uint8_t g, uint8_t b),
    Color888(uint8_t r, uint8_t g, uint8_t b),
    Color888(uint8_t r, uint8_t g, uint8_t b, boolean gflag),
    ColorHSV(long hue, uint8_t sat, uint8_t val, boolean gflag);
      uint8_t RWIDTH, RHEIGHT;
      
  protected:
    uint8_t panel_x, panel_y; // number of panels in each direction

 private:

  uint8_t         *matrixbuff[2];
  uint8_t nRows;
  volatile uint8_t backindex;
  volatile boolean swapflag;

  IntervalTimer drawTimer;

  // Init/alloc code common to both constructors:
  void init(uint8_t rows, boolean dbuf);


  // Counters/pointers for interrupt handler:
  volatile uint8_t row, plane;
  volatile uint8_t *buffptr;
};

