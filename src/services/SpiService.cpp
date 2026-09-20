#include "SpiService.h"
#include "gpio_module.h"   // for gpioGetSPI() — dedicated FSPI bus
#include <Arduino.h>

// ============================================================
//  FIX: this used to grab the global `SPI`, which is the same bus
//  TFT_eSPI and the SD card use. Calling SPI.begin(IO1, IO2, IO3, IO4)
//  here remapped SCK/MOSI/MISO away from the display's pins, which
//  blanked the screen and unmounted the SD.
//
//  Now we bind to gpioGetSPI() (a dedicated FSPI instance) — the same
//  bus pn532_module.cpp already uses — so the display/SD bus is
//  never touched.
// ============================================================

void SpiService::begin(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs) {
    _cs = cs;
    _spi = &gpioGetSPI();
    _spi->begin(sck, miso, mosi, cs);
    pinMode(cs, OUTPUT);
    digitalWrite(cs, HIGH);
}

void SpiService::end() {
    if (_spi) {
        _spi->end();
        _spi = nullptr;
    }
}

void SpiService::setMode(uint8_t mode) { _mode = mode; }
void SpiService::setFrequency(uint32_t freq) { _freq = freq; }

void SpiService::csLow()  { if (_spi) digitalWrite(_cs, LOW); }
void SpiService::csHigh() { if (_spi) digitalWrite(_cs, HIGH); }

uint8_t SpiService::transfer(uint8_t data) {
    if (!_spi) return 0;
    _spi->beginTransaction(SPISettings(_freq, MSBFIRST, _mode));
    uint8_t ret = _spi->transfer(data);
    _spi->endTransaction();
    return ret;
}

void SpiService::transfer(const uint8_t* tx, uint8_t* rx, size_t len) {
    if (!_spi) return;
    _spi->beginTransaction(SPISettings(_freq, MSBFIRST, _mode));
    for (size_t i = 0; i < len; i++) {
        rx[i] = _spi->transfer(tx[i]);
    }
    _spi->endTransaction();
}

// ---- Stubs ----
void SpiService::beginSlave(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs) {
    (void)sck; (void)mosi; (void)miso; (void)cs;
}
void SpiService::endSlave() {}