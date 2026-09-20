#pragma once
#include <HardwareSerial.h>

struct UartConfig {
    uint32_t baud;
    uint8_t dataBits;
    uint8_t parity; // 0=none, 1=odd, 2=even
    uint8_t stopBits;
};

class UartService {
public:
    void begin(uint8_t rx, uint8_t tx, const UartConfig& cfg);
    void end();
    void bridge(Stream& src, Stream& dst);
    size_t write(const uint8_t* data, size_t len);
    size_t write(uint8_t c);
    int available();
    int read();
    void setConfig(const UartConfig& cfg);
    UartConfig getConfig() const;

    void clearBuffer();
    String readLine(unsigned long timeout = 1000);

private:
    HardwareSerial* _serial = nullptr;
    uint8_t _rx, _tx;
    UartConfig _cfg;
};