// LU6JMF 2.1.9 - one OLED driver for both common 128x64 I2C screens:
//   0.96" (SSD1306) and 1.3" (SH1106). One firmware for both; the screen type is chosen
//   on the System page (Display Setting) or found automatically at start-up.
// It draws exactly like the Adafruit drivers it replaces (same Adafruit_GFX drawing).
#pragma once
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>

#ifndef SSD1306_SWITCHCAPVCC
#define SSD1306_SWITCHCAPVCC 0x02
#endif
#ifndef SH1106_SWITCHCAPVCC
#define SH1106_SWITCHCAPVCC 0x02
#endif
#ifndef BLACK
#define BLACK 0
#endif
#ifndef WHITE
#define WHITE 1
#endif
#ifndef INVERSE
#define INVERSE 2
#endif

// screen type setting (config.disp_type)
#define LU6_OLED_AUTO 0
#define LU6_OLED_SSD1306 1 // 0.96"
#define LU6_OLED_SH1106 2  // 1.3"

class Lu6Oled : public Adafruit_GFX
{
public:
    Lu6Oled() : Adafruit_GFX(128, 64) {}
    void setType(uint8_t t) { _want = t; }
    uint8_t type() const { return _type; }          // chip in use (LU6_OLED_SSD1306 / LU6_OLED_SH1106)
    bool detected() const { return _detected; }     // true = the type came from the automatic check
    uint8_t status() const { return _status; }      // status byte read from the screen (diagnostic)
    bool present() const { return _present; }       // false = no screen answered at start-up
    // same call as Adafruit_SSD1306::begin / Adafruit_SH1106::begin
    bool begin(uint8_t vcc = SSD1306_SWITCHCAPVCC, uint8_t addr = 0x3C, bool reset = false, bool periphBegin = true);
    void display(void);
    void clearDisplay(void) { memset(_buf, 0, sizeof(_buf)); }
    void dim(bool d);
    void invertDisplay(bool i) override;
    void ssd1306_command(uint8_t c) { command(c); }
    void SH1106_command(uint8_t c) { command(c); }
    void drawPixel(int16_t x, int16_t y, uint16_t color) override;
    void fillScreen(uint16_t color) override { memset(_buf, color ? 0xFF : 0x00, sizeof(_buf)); }
    uint8_t *getBuffer(void) { return _buf; }

private:
    void command(uint8_t c);
    uint8_t probe();
    uint8_t _buf[128 * 64 / 8];
    uint8_t _addr = 0x3C;
    uint8_t _want = LU6_OLED_AUTO;
    uint8_t _type = LU6_OLED_SSD1306;
    uint8_t _status = 0xFF;
    bool _detected = false;
    bool _present = false;
};
