/*
RGBmatrixPanel Arduino library for Adafruit 16x32 and 32x32 RGB LED
matrix panels.  Pick one up at:
  http://www.adafruit.com/products/420
  http://www.adafruit.com/products/607

This version uses a few tricks to achieve better performance and/or
lower CPU utilization:

- To control LED brightness, traditional PWM is eschewed in favor of
  Binary Code Modulation, which operates through a succession of periods
  each twice the length of the preceeding one (rather than a direct
  linear count a la PWM).  It's explained well here:

    http://www.batsocks.co.uk/readme/art_bcm_1.htm

  I was initially skeptical, but it works exceedingly well in practice!
  And this uses considerably fewer CPU cycles than software PWM.

- Although many control pins are software-configurable in the user's
  code, a couple things are tied to specific PORT registers.  It's just
  a lot faster this way -- port lookups take time.  Please see the notes
  later regarding wiring on "alternative" Arduino boards.

- A tiny bit of inline assembly language is used in the most speed-
  critical section.  The C++ compiler wasn't making optimal use of the
  instruction set in what seemed like an obvious chunk of code.  Since
  it's only a few short instructions, this loop is also "unrolled" --
  each iteration is stated explicitly, not through a control loop.

Written by Limor Fried/Ladyada & Phil Burgess/PaintYourDragon for
Adafruit Industries.
BSD license, all text above must be included in any redistribution.
*/

#include "RGBmatrixPanel.h"
#include "gamma.h"

#ifndef _swap
#define _swap(a, b) { auto t = a; a = b; b = t; }
#endif

// A full PORT register is required for the data lines, though only the
// top 6 output bits are used.  For performance reasons, the port # cannot
// be changed via library calls, only by changing constants in the library.
// For similar reasons, the clock pin is only semi-configurable...it can
// be specified as any pin within a specific PORT register stated below.

void drawInterrupt();

#define DATAPORT PORTD
#define DATADIR  DDRD
#define nPlanes 4

// The fact that the display driver interrupt stuff is tied to the
// singular Timer1 doesn't really take well to object orientation with
// multiple RGBmatrixPanel instances.  The solution at present is to
// allow instances, but only one is active at any given time, via its
// begin() method.  The implementation is still incomplete in parts;
// the prior active panel really should be gracefully disabled, and a
// stop() method should perhaps be added...assuming multiple instances
// are even an actual need.
static RGBmatrixPanel *activePanel = NULL;

// Code common to both the 16x32 and 32x32 constructors:
void RGBmatrixPanel::init(uint8_t rows, boolean dbuf) {
  nRows = rows; // Number of multiplexed rows; actual height is 2X this

  // Allocate and initialize matrix buffer:
  uint32_t buffsize = RWIDTH * RHEIGHT * 3, // we'll pack things efficiently
  allocsize = (dbuf == true) ? (buffsize * 2) : buffsize;
  if (NULL == (matrixbuff[0] = (uint8_t *)malloc(allocsize))) return;
  memset(matrixbuff[0], 0, allocsize);
  // If not double-buffered, both buffers then point to the same address:
  matrixbuff[1] = (dbuf == true) ? &matrixbuff[0][buffsize] : matrixbuff[0];

  plane = nPlanes - 1;
  row = nRows - 1;
  swapflag = false;
  backindex = 0;     // Array index of back buffer
}

// Constructor for 32x32 or 32x64 panel:
RGBmatrixPanel::RGBmatrixPanel(boolean dbuf, uint8_t width, uint8_t height, uint8_t _panel_x, uint8_t _panel_y, uint8_t nRows) :
  Adafruit_GFX(width, height) {
    panel_x = _panel_x;
    panel_y = _panel_y;
    
    // Copy these
    RWIDTH=WIDTH/panel_x * panel_y;
    RHEIGHT=HEIGHT/panel_y;

  init(nRows, dbuf);
}

void RGBmatrixPanel::begin(void) {

  backindex   = 0;                         // Back buffer
  buffptr     = matrixbuff[1 - backindex]; // -> front buffer
  activePanel = this;                      // For interrupt hander

  // Enable all comm & address pins as outputs, set default states:
  pinMode(CLK, OUTPUT); digitalWriteFast(CLK, LOW);
  pinMode(LAT, OUTPUT); digitalWriteFast(LAT, LOW);
  pinMode(OE, OUTPUT); digitalWriteFast(OE, HIGH);
  pinMode(A, OUTPUT); digitalWriteFast(A, LOW);
  pinMode(B, OUTPUT); digitalWriteFast(B, LOW);
  pinMode(C, OUTPUT); digitalWriteFast(C, LOW);
  if (nRows > 8) {
    pinMode(D, OUTPUT); digitalWriteFast(D, LOW);
  }

  DATADIR = B11111111;
  DATAPORT = 0;

  drawTimer.begin(drawInterrupt, 150 * (panel_x * panel_y)); // Scale the interrupt time by the number of panels

  sei();                // Enable global interrupts
}

// Original RGBmatrixPanel library used 3/3/3 color.  Later version used
// 4/4/4.  Then Adafruit_GFX (core library used across all Adafruit
// display devices now) standardized on 5/6/5.  The matrix still operates
// internally on 4/4/4 color, but all the graphics functions are written
// to expect 5/6/5...the matrix lib will truncate the color components as
// needed when drawing.  These next functions are mostly here for the
// benefit of older code using one of the original color formats.

// Promote 3/3/3 RGB to Adafruit_GFX 5/6/5
uint16_t RGBmatrixPanel::Color333(uint8_t r, uint8_t g, uint8_t b) {
  // RRRrrGGGgggBBBbb
  return ((r & 0x7) << 13) | ((r & 0x6) << 10) |
         ((g & 0x7) <<  8) | ((g & 0x7) <<  5) |
         ((b & 0x7) <<  2) | ((b & 0x6) >>  1);
}

// Promote 4/4/4 RGB to Adafruit_GFX 5/6/5
uint16_t RGBmatrixPanel::Color444(uint8_t r, uint8_t g, uint8_t b) {
  // RRRRrGGGGggBBBBb
  return ((r & 0xF) << 12) | ((r & 0x8) << 8) |
         ((g & 0xF) <<  7) | ((g & 0xC) << 3) |
         ((b & 0xF) <<  1) | ((b & 0x8) >> 3);
}

// Demote 8/8/8 to Adafruit_GFX 5/6/5
// If no gamma flag passed, assume linear color
uint16_t RGBmatrixPanel::Color888(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// 8/8/8 -> gamma -> 5/6/5
uint16_t RGBmatrixPanel::Color888(
  uint8_t r, uint8_t g, uint8_t b, boolean gflag) {
  if(gflag) { // Gamma-corrected color?
    r = pgm_read_byte(&gammaVal[r]); // Gamma correction table maps
    g = pgm_read_byte(&gammaVal[g]); // 8-bit input to 4-bit output
    b = pgm_read_byte(&gammaVal[b]);
    return ((uint16_t)r << 12) | ((uint16_t)(r & 0x8) << 8) | // 4/4/4->5/6/5
           ((uint16_t)g <<  7) | ((uint16_t)(g & 0xC) << 3) |
           (          b <<  1) | (           b        >> 3);
  } // else linear (uncorrected) color
  return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

uint16_t RGBmatrixPanel::ColorHSV(
  long hue, uint8_t sat, uint8_t val, boolean gflag) {

  uint8_t  r, g, b, lo;
  uint16_t s1, v1;

  // Hue
  hue %= 1536;             // -1535 to +1535
  if(hue < 0) hue += 1536; //     0 to +1535
  lo = hue & 255;          // Low byte  = primary/secondary color mix
  switch(hue >> 8) {       // High byte = sextant of colorwheel
    case 0 : r = 255     ; g =  lo     ; b =   0     ; break; // R to Y
    case 1 : r = 255 - lo; g = 255     ; b =   0     ; break; // Y to G
    case 2 : r =   0     ; g = 255     ; b =  lo     ; break; // G to C
    case 3 : r =   0     ; g = 255 - lo; b = 255     ; break; // C to B
    case 4 : r =  lo     ; g =   0     ; b = 255     ; break; // B to M
    default: r = 255     ; g =   0     ; b = 255 - lo; break; // M to R
  }

  // Saturation: add 1 so range is 1 to 256, allowig a quick shift operation
  // on the result rather than a costly divide, while the type upgrade to int
  // avoids repeated type conversions in both directions.
  s1 = sat + 1;
  r  = 255 - (((255 - r) * s1) >> 8);
  g  = 255 - (((255 - g) * s1) >> 8);
  b  = 255 - (((255 - b) * s1) >> 8);

  // Value (brightness) & 16-bit color reduction: similar to above, add 1
  // to allow shifts, and upgrade to int makes other conversions implicit.
  v1 = val + 1;
  if(gflag) { // Gamma-corrected color?
    r = pgm_read_byte(&gammaVal[(r * v1) >> 8]); // Gamma correction table maps
    g = pgm_read_byte(&gammaVal[(g * v1) >> 8]); // 8-bit input to 4-bit output
    b = pgm_read_byte(&gammaVal[(b * v1) >> 8]);
  } else { // linear (uncorrected) color
    r = (r * v1) >> 12; // 4-bit results
    g = (g * v1) >> 12;
    b = (b * v1) >> 12;
  }
  return (r << 12) | ((r & 0x8) << 8) | // 4/4/4 -> 5/6/5
         (g <<  7) | ((g & 0xC) << 3) |
         (b <<  1) | ( b        >> 3);
}

void RGBmatrixPanel::drawPixel(int16_t x, int16_t y, uint16_t c) {
  uint8_t r, g, b, bit, limit, *ptr;

  if((x < 0) || (x >= _width) || (y < 0) || (y >= _height)) return;

  switch(rotation) {
   case 1:
    _swap(x, y);
    x = WIDTH  - 1 - x;
    break;
   case 2:
    x = WIDTH  - 1 - x;
    y = HEIGHT - 1 - y;
    break;
   case 3:
    _swap(x, y);
    y = HEIGHT - 1 - y;
    break;
  }

  // specific to x16 panels, make generic
  int yoff = y / RHEIGHT;
  
  x = x + yoff * WIDTH; // move over 32
  y = y % RHEIGHT;
  
  // Adafruit_GFX uses 16-bit color in 5/6/5 format, while matrix needs
  // 4/4/4.  Pluck out relevant bits while separating into R,G,B:
  r =  c >> 12;        // RRRRrggggggbbbbb
  g = (c >>  7) & 0xF; // rrrrrGGGGggbbbbb
  b = (c >>  1) & 0xF; // rrrrrggggggBBBBb

  // Loop counter stuff
  bit   = 2;
  limit = 1 << nPlanes;

  if(y < nRows) {
    // Data for the upper half of the display is stored in the lower
    // bits of each byte.
    ptr = &matrixbuff[backindex][y * RWIDTH * (nPlanes - 1) + x]; // Base addr
    // Plane 0 is a tricky case -- its data is spread about,
    // stored in least two bits not used by the other planes.
    ptr[WIDTH*2] &= ~B00000011;           // Plane 0 R,G mask out in one op
    if(r & 1) ptr[RWIDTH*2] |=  B00000001; // Plane 0 R: 64 bytes ahead, bit 0
    if(g & 1) ptr[RWIDTH*2] |=  B00000010; // Plane 0 G: 64 bytes ahead, bit 1
    if(b & 1) ptr[RWIDTH]   |=  B00000001; // Plane 0 B: 32 bytes ahead, bit 0
    else      ptr[RWIDTH]   &= ~B00000001; // Plane 0 B unset; mask out
    // The remaining three image planes are more normal-ish.
    // Data is stored in the high 6 bits so it can be quickly
    // copied to the DATAPORT register w/6 output lines.
    for(; bit < limit; bit <<= 1) {
      *ptr &= ~B00011100;            // Mask out R,G,B in one op
      if(r & bit) *ptr |= B00000100; // Plane N R: bit 2
      if(g & bit) *ptr |= B00001000; // Plane N G: bit 3
      if(b & bit) *ptr |= B00010000; // Plane N B: bit 4
      ptr  += RWIDTH;                 // Advance to next bit plane
    }
  } else {
    // Data for the lower half of the display is stored in the upper
    // bits, except for the plane 0 stuff, using 2 least bits.
    ptr = &matrixbuff[backindex][(y - nRows) * RWIDTH * (nPlanes - 1) + x];
    *ptr &= ~B00000011;                  // Plane 0 G,B mask out in one op
    if(r & 1)  ptr[RWIDTH] |=  B00000010; // Plane 0 R: 32 bytes ahead, bit 1
    else       ptr[RWIDTH] &= ~B00000010; // Plane 0 R unset; mask out
    if(g & 1) *ptr        |=  B00000001; // Plane 0 G: bit 0
    if(b & 1) *ptr        |=  B00000010; // Plane 0 B: bit 0
    for(; bit < limit; bit <<= 1) {
      *ptr &= ~B11100000;            // Mask out R,G,B in one op
      if(r & bit) *ptr |= B00100000; // Plane N R: bit 5
      if(g & bit) *ptr |= B01000000; // Plane N G: bit 6
      if(b & bit) *ptr |= B10000000; // Plane N B: bit 7
      ptr  += RWIDTH;                 // Advance to next bit plane
    }
  }
}

void RGBmatrixPanel::fillScreen(uint16_t c) {
  if((c == 0x0000) || (c == 0xffff)) {
    // For black or white, all bits in frame buffer will be identically
    // set or unset (regardless of weird bit packing), so it's OK to just
    // quickly memset the whole thing:
    memset(matrixbuff[backindex], c, RWIDTH * nRows * 3);
  } else {
    // Otherwise, need to handle it the long way:
    Adafruit_GFX::fillScreen(c);
  }
}

// Return address of back buffer -- can then load/store data directly
uint8_t *RGBmatrixPanel::backBuffer() {
  return matrixbuff[backindex];
}

// For smooth animation -- drawing always takes place in the "back" buffer;
// this method pushes it to the "front" for display.  Passing "true", the
// updated display contents are then copied to the new back buffer and can
// be incrementally modified.  If "false", the back buffer then contains
// the old front buffer contents -- your code can either clear this or
// draw over every pixel.  (No effect if double-buffering is not enabled.)
void RGBmatrixPanel::swapBuffers(boolean copy) {
  if(matrixbuff[0] != matrixbuff[1]) {
    // To avoid 'tearing' display, actual swap takes place in the interrupt
    // handler, at the end of a complete screen refresh cycle.
    swapflag = true;                  // Set flag here, then...
    while(swapflag == true) delay(1); // wait for interrupt to clear it
    if(copy == true)
      memcpy(matrixbuff[backindex], matrixbuff[1-backindex], WIDTH * nRows * 3);
  }
}

// Dump display contents to the Serial Monitor, adding some formatting to
// simplify copy-and-paste of data as a PROGMEM-embedded image for another
// sketch.  If using multiple dumps this way, you'll need to edit the
// output to change the 'img' name for each.  Data can then be loaded
// back into the display using a pgm_read_byte() loop.
void RGBmatrixPanel::dumpMatrix(void) {

  int i, buffsize = WIDTH * nRows * 3;

  Serial.print(F("\n\n"
    "#include <avr/pgmspace.h>\n\n"
    "static const uint8_t PROGMEM img[] = {\n  "));

  for(i=0; i<buffsize; i++) {
    Serial.print(F("0x"));
    if(matrixbuff[backindex][i] < 0x10) Serial.write('0');
    Serial.print(matrixbuff[backindex][i],HEX);
    if(i < (buffsize - 1)) {
      if((i & 7) == 7) Serial.print(F(",\n  "));
      else             Serial.write(',');
    }
  }
  Serial.println(F("\n};"));
}

// -------------------- Interrupt handler stuff --------------------

void drawInterrupt()
{
  activePanel->updateDisplay();
  //drawTimer.end();
}

// Two constants are used in timing each successive BCM interval.
// These were found empirically, by checking the value of TCNT1 at
// certain positions in the interrupt code.
// CALLOVERHEAD is the number of CPU 'ticks' from the timer overflow
// condition (triggering the interrupt) to the first line in the
// updateDisplay() method.  It's then assumed (maybe not entirely 100%
// accurately, but close enough) that a similar amount of time will be
// needed at the opposite end, restoring regular program flow.
// LOOPTIME is the number of 'ticks' spent inside the shortest data-
// issuing loop (not actually a 'loop' because it's unrolled, but eh).
// Both numbers are rounded up slightly to allow a little wiggle room
// should different compilers produce slightly different results.
#define CALLOVERHEAD 60   // Actual value measured = 56
#define LOOPTIME     400  // Actual value measured = 188
// The "on" time for bitplane 0 (with the shortest BCM interval) can
// then be estimated as LOOPTIME + CALLOVERHEAD * 2.  Each successive
// bitplane then doubles the prior amount of time.  We can then
// estimate refresh rates from this:
// 4 bitplanes = 320 + 640 + 1280 + 2560 = 4800 ticks per row.
// 4800 ticks * 16 rows (for 32x32 matrix) = 76800 ticks/frame.
// 16M CPU ticks/sec / 76800 ticks/frame = 208.33 Hz.
// Actual frame rate will be slightly less due to work being done
// during the brief "LEDs off" interval...it's reasonable to say
// "about 200 Hz."  The 16x32 matrix only has to scan half as many
// rows...so we could either double the refresh rate (keeping the CPU
// load the same), or keep the same refresh rate but halve the CPU
// load.  We opted for the latter.
// Can also estimate CPU use: bitplanes 1-3 all use 320 ticks to
// issue data (the increasing gaps in the timing invervals are then
// available to other code), and bitplane 0 takes 920 ticks out of
// the 2560 tick interval.
// 320 * 3 + 920 = 1880 ticks spent in interrupt code, per row.
// From prior calculations, about 4800 ticks happen per row.
// CPU use = 1880 / 4800 = ~39% (actual use will be very slightly
// higher, again due to code used in the LEDs off interval).
// 16x32 matrix uses about half that CPU load.  CPU time could be
// further adjusted by padding the LOOPTIME value, but refresh rates
// will decrease proportionally, and 200 Hz is a decent target.

// The flow of the interrupt can be awkward to grasp, because data is
// being issued to the LED matrix for the *next* bitplane and/or row
// while the *current* plane/row is being shown.  As a result, the
// counter variables change between past/present/future tense in mid-
// function...hopefully tenses are sufficiently commented.

void RGBmatrixPanel::updateDisplay(void) {
  uint8_t  i, *ptr;

  digitalWriteFast(OE, HIGH); // Disable LED output during row/plane switchover
  digitalWriteFast(LAT, HIGH); // Latch data loaded during *prior* interrupt

  // Borrowing a technique here from Ray's Logic:
  // www.rayslogic.com/propeller/Programming/AdafruitRGB/AdafruitRGB.htm
  // This code cycles through all four planes for each scanline before
  // advancing to the next line.  While it might seem beneficial to
  // advance lines every time and interleave the planes to reduce
  // vertical scanning artifacts, in practice with this panel it causes
  // a green 'ghosting' effect on black pixels, a much worse artifact.
  
  if (++plane >= nPlanes) {      // Advance plane counter.  Maxed out?
    plane = 0;                  // Yes, reset to plane 0, and
    if (++row >= nRows) {        // advance row counter.  Maxed out?
      row = 0;              // Yes, reset row counter, then...
      if (swapflag == true) {    // Swap front/back buffers if requested
        backindex = 1 - backindex;
        swapflag = false;
      }
      buffptr = matrixbuff[1 - backindex]; // Reset into front buffer
    }
  }
  else if (plane == 1) {
    digitalWriteFast(A, row & 0x1 ? HIGH : LOW);
    digitalWriteFast(B, row & 0x2 ? HIGH : LOW);
    digitalWriteFast(C, row & 0x4 ? HIGH : LOW);
    if (nRows >= 8)
      digitalWriteFast(D, row & 0x8 ? HIGH : LOW);
  }

  
  // buffptr, being 'volatile' type, doesn't take well to optimization.
  // A local register copy can speed some things up:
  ptr = (uint8_t *)buffptr;

  //drawTimer.begin(drawInterrupt, 10000);
  digitalWriteFast(OE, LOW); // Re-enable output
  digitalWriteFast(LAT, LOW); // Latch down

  // Record current state of SCLKPORT register, as well as a second
  // copy with the clock bit set.  This makes the innnermost data-
  // pushing loops faster, as they can just set the PORT state and
  // not have to load/modify/store bits every single time.  It's a
  // somewhat rude trick that ONLY works because the interrupt
  // handler is set ISR_BLOCK, halting any other interrupts that
  // might otherwise also be twiddling the port at the same time
  // (else this would clobber them).

  if (plane > 0) { // 188 ticks from TCNT1=0 (above) to end of function

    // Planes 1-3 copy bytes directly from RAM to PORT without unpacking.
    // The least 2 bits (used for plane 0 data) are presumed masked out
    // by the port direction bits.

    for (i = 0; i < RWIDTH; i++)
    {
      DATAPORT = ptr[i]>>2;
      //SCLKPORT = tick; // Clock lo
      digitalWriteFast(CLK, HIGH);
#if   F_CPU == 96000000
      asm volatile("nop"); // we need a very short delay to let the clock line actually change before we turn it LOW again,  48Mhz and below doesn't need this.
#elif F_CPU == 72000000
#error "This can't work at 72MHz, there's something even wronger about the timing, go up to 96MHz"
#endif

      //SCLKPORT = tock; // Clock hip
      digitalWriteFast(CLK, LOW);
    }
    ptr = &(ptr[RWIDTH]);

    buffptr = ptr; //+= 32;

  }
  else { // 920 ticks from TCNT1=0 (above) to end of function

    // Planes 1-3 (handled above) formatted their data "in place,"
    // their layout matching that out the output PORT register (where
    // 6 bits correspond to output data lines), maximizing throughput
    // as no conversion or unpacking is needed.  Plane 0 then takes up
    // the slack, with all its data packed into the 2 least bits not
    // used by the other planes.  This works because the unpacking and
    // output for plane 0 is handled while plane 3 is being displayed...
    // because binary coded modulation is used (not PWM), that plane
    // has the longest display interval, so the extra work fits.
    for (i = 0; i<RWIDTH; i++) {
      DATAPORT =
        ((ptr[i] << 6) |
         ((ptr[i + RWIDTH] << 4) & 0x30) |
         ((ptr[i + RWIDTH * 2] << 2) & 0x0C)) >> 2;

      //SCLKPORT = tick; // Clock lo
      digitalWriteFast(CLK, HIGH);
      //delayMicroseconds(1);
#if   F_CPU == 96000000
      asm volatile("nop"); // we need a very short delay to let the clock line actually change before we turn it LOW again,  48Mhz and below doesn't need this.
#elif F_CPU == 72000000
#error "This can't work at 72MHz, there's something even wronger about the timing, go up to 96MHz"
#endif
      //SCLKPORT = tock; // Clock hip
      digitalWriteFast(CLK, LOW);
    }
  }
  //delayMicroseconds(plane);
}
