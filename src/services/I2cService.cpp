#include "I2cService.h"
#include "gpio_module.h"   // for gpioGetWire() — dedicated Wire1 bus

// ============================================================
//  FIX: this used to grab the global `Wire`, which is the same bus
//  buttons.cpp uses for the MCP23017. Calling Wire.begin(IO1, IO2)
//  here reassigned the I2C peripheral's pins away from the buttons,
//  so every button press stopped working until you left the screen.
//
//  Now we bind to gpioGetWire() (a dedicated Wire1 instance) — the
//  same bus pn532_module.cpp already uses — so the buttons' Wire
//  bus is never touched.
// ============================================================

// File-scope pointer used by the slave callbacks (which are static
// and can't reach the instance's _wire member).
static TwoWire* g_slaveWire = nullptr;

void I2cService::begin(uint8_t sda, uint8_t scl, uint32_t freq) {
    _sda = sda;
    _scl = scl;
    _freq = freq;
    _wire = &gpioGetWire();
    _wire->begin(sda, scl, freq);
}

void I2cService::end() {
    if (_wire) {
        _wire->end();
        _wire = nullptr;
    }
    g_slaveWire = nullptr;
    if (_slaveActive) endSlave();
}

bool I2cService::isDeviceReady(uint8_t addr) {
    if (!_wire) return false;
    _wire->beginTransmission(addr);
    return _wire->endTransmission() == 0;
}

uint8_t I2cService::writeBytes(uint8_t addr, const uint8_t* data, uint8_t len) {
    if (!_wire) return 4;
    _wire->beginTransmission(addr);
    for (uint8_t i = 0; i < len; i++) _wire->write(data[i]);
    return _wire->endTransmission();
}

int I2cService::readBytes(uint8_t addr, uint8_t* buffer, uint8_t len) {
    if (!_wire) return -1;
    _wire->requestFrom((int)addr, (int)len);
    int got = 0;
    while (_wire->available() && got < len) {
        buffer[got++] = _wire->read();
    }
    return got;
}

bool I2cService::writeThenRead(uint8_t addr, const uint8_t* wdata, uint8_t wlen,
                                uint8_t* rdata, uint8_t rlen) {
    if (!_wire) return false;
    if (wlen > 0) {
        _wire->beginTransmission(addr);
        for (uint8_t i = 0; i < wlen; i++) _wire->write(wdata[i]);
        if (_wire->endTransmission() != 0) return false;
    }
    if (rlen > 0) {
        _wire->requestFrom((int)addr, (int)rlen);
        int got = 0;
        while (_wire->available() && got < rlen) rdata[got++] = _wire->read();
        return got == rlen;
    }
    return true;
}

void I2cService::setSpeed(uint32_t freq) {
    _freq = freq;
    if (_wire) {
        _wire->end();
        _wire->begin(_sda, _scl, freq);
    }
}

// ---- Slave mode ----
static void onSlaveReceive(int len) { (void)len; }

static void onSlaveRequest() {
    if (g_slaveWire) g_slaveWire->write(0xAA);
}

void I2cService::beginSlave(uint8_t addr) {
    if (_wire) {
        g_slaveWire = _wire;
        _wire->begin(addr);
        _wire->onReceive(onSlaveReceive);
        _wire->onRequest(onSlaveRequest);
        _slaveActive = true;
    }
}

void I2cService::endSlave() {
    if (_wire) {
        _wire->onReceive(nullptr);
        _wire->onRequest(nullptr);
        _wire->end();
        _wire->begin(_sda, _scl, _freq);
        _slaveActive = false;
        g_slaveWire = nullptr;
    }
}

bool I2cService::isSlaveActive() const { return _slaveActive; }