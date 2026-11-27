// LU6JMF 2.1.9 - see include/lu6_oled.h
#ifdef LU6_OLED
#include "lu6_oled.h"

void Lu6Oled::command(uint8_t c)
{
    if (!_present)
        return; // no screen connected: nothing is sent on the I2C bus
    Wire.beginTransmission(_addr);
    Wire.write((uint8_t)0x00); // Co = 0, D/C = 0
    Wire.write(c);
    Wire.endTransmission();
}

// Read the screen's status byte (same check used by Meshtastic): the low 4 bits tell the chip.
// SH1106: 0x08 or 0x00; SSD1306: 0x03, 0x04, 0x06 or 0x07. Read until two reads agree (max 4).
uint8_t Lu6Oled::probe()
{
    uint8_t r = 0, prev = 0, n = 0, found = 0;
    do
    {
        prev = r;
        Wire.beginTransmission(_addr);
        Wire.write((uint8_t)0x00);
        Wire.endTransmission();
        if (Wire.requestFrom((int)_addr, 1) == 1 && Wire.available())
            r = Wire.read();
        _status = r;
        r &= 0x0F;
        if (r == 0x08 || r == 0x00)
            found = LU6_OLED_SH1106;
        else if (r == 0x03 || r == 0x04 || r == 0x06 || r == 0x07)
            found = LU6_OLED_SSD1306;
        n++;
    } while (r != prev && n < 4);
    return found;
}

bool Lu6Oled::begin(uint8_t vcc, uint8_t addr, bool reset, bool periphBegin)
{
    (void)vcc;
    (void)reset;
    _addr = addr ? addr : 0x3C;
    if (periphBegin)
        Wire.begin();
    _detected = false;
    // nothing answers at the screen address: no screen connected
    Wire.beginTransmission(_addr);
    _present = (Wire.endTransmission() == 0);
    if (!_present)
        return false;
    if (_want == LU6_OLED_SSD1306 || _want == LU6_OLED_SH1106)
        _type = _want;
    else
    {
        uint8_t t = probe();
        _detected = (t != 0);
        _type = t ? t : LU6_OLED_SSD1306; // not recognised: 0.96" (most common)
    }
    clearDisplay();
    // same start-up sequence the Adafruit SSD1306 and SH1106 drivers send for a 128x64 screen
    // with the internal charge pump (the SH1106 ignores the commands it does not have)
    static const uint8_t init[] = {
        0xAE,       // display off
        0xD5, 0x80, // clock
        0xA8, 0x3F, // multiplex 64
        0xD3, 0x00, // display offset
        0x40,       // start line 0
        0x8D, 0x14, // charge pump on (SSD1306)
        0x20, 0x00, // horizontal addressing (SSD1306)
        0xA1,       // segment remap
        0xC8,       // COM scan decrement
        0xDA, 0x12, // COM pins
        0x81, 0xCF, // contrast
        0xD9, 0xF1, // pre-charge
        0xDB, 0x40, // VCOM detect
        0xA4,       // follow RAM
        0xA6,       // normal (not inverted)
        0x2E,       // no scroll
        0xAF};      // display on
    for (uint8_t c : init)
        command(c);
    return true;
}

void Lu6Oled::display(void)
{
    if (!_present)
        return; // no screen connected
    if (_type == LU6_OLED_SH1106)
    {
        // SH1106: 132 columns, the visible 128 start at column 2; page by page
        for (uint8_t page = 0; page < 8; page++)
        {
            command(0xB0 | page);
            command(0x02); // low column = 2
            command(0x10); // high column = 0
            const uint8_t *p = _buf + page * 128;
            for (uint8_t i = 0; i < 128; i += 16)
            {
                Wire.beginTransmission(_addr);
                Wire.write((uint8_t)0x40);
                Wire.write(p + i, 16);
                Wire.endTransmission();
            }
        }
    }
    else
    {
        // SSD1306: whole screen in one go
        command(0x22); // page address
        command(0);
        command(0xFF);
        command(0x21); // column address
        command(0);
        command(127);
        for (uint16_t i = 0; i < sizeof(_buf); i += 16)
        {
            Wire.beginTransmission(_addr);
            Wire.write((uint8_t)0x40);
            Wire.write(_buf + i, 16);
            Wire.endTransmission();
        }
    }
}

void Lu6Oled::dim(bool d)
{
    command(0x81);
    command(d ? 0 : 0xCF);
}

void Lu6Oled::invertDisplay(bool i)
{
    command(i ? 0xA7 : 0xA6);
}

void Lu6Oled::drawPixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || x >= width() || y < 0 || y >= height())
        return;
    switch (getRotation())
    {
    case 1:
    {
        int16_t t = x;
        x = WIDTH - y - 1;
        y = t;
        break;
    }
    case 2:
        x = WIDTH - x - 1;
        y = HEIGHT - y - 1;
        break;
    case 3:
    {
        int16_t t = x;
        x = y;
        y = HEIGHT - t - 1;
        break;
    }
    }
    uint8_t *b = &_buf[x + (y / 8) * 128];
    uint8_t m = 1 << (y & 7);
    switch (color)
    {
    case WHITE:
        *b |= m;
        break;
    case BLACK:
        *b &= ~m;
        break;
    case INVERSE:
        *b ^= m;
        break;
    }
}
#endif // LU6_OLED
