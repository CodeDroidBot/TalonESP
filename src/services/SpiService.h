#pragma once
#include <SPI.h>

class SpiService {
public:
    void begin(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs);
    void end();
    void setMode(uint8_t mode);
    void setFrequency(uint32_t freq);
    uint8_t transfer(uint8_t data);
    void transfer(const uint8_t* tx, uint8_t* rx, size_t len);
    void csLow();
    void csHigh();

    // ---- Slave mode (stub) ----
    void beginSlave(uint8_t sck, uint8_t mosi, uint8_t miso, uint8_t cs);
    void endSlave();

private:
    SPIClass* _spi = nullptr;
    uint8_t _cs = 0;
    uint32_t _freq = 1000000;
    uint8_t _mode = 0;
};