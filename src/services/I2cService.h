#pragma once
#include <Wire.h>
#include <vector>
#include <String>

class I2cService {
public:
    // ---- Configuration ----
    void begin(uint8_t sda, uint8_t scl, uint32_t freq = 100000);
    void end();

    // ---- Master operations ----
    bool isDeviceReady(uint8_t addr);
    uint8_t writeBytes(uint8_t addr, const uint8_t* data, uint8_t len);
    int readBytes(uint8_t addr, uint8_t* buffer, uint8_t len);
    bool writeThenRead(uint8_t addr, const uint8_t* wdata, uint8_t wlen, uint8_t* rdata, uint8_t rlen);

    // ---- Slave mode (simple echo) ----
    void beginSlave(uint8_t addr);
    void endSlave();
    bool isSlaveActive() const;

    // ---- Utility ----
    void setSpeed(uint32_t freq); // in Hz

private:
    TwoWire* _wire = nullptr;
    bool _slaveActive = false;
    uint32_t _freq = 100000;
    uint8_t _sda, _scl;
};