#include "UartService.h"

void UartService::begin(uint8_t rx, uint8_t tx, const UartConfig& cfg) {
    _rx = rx; _tx = tx; _cfg = cfg;
    _serial = &Serial1;
    // Map config to Arduino constants (simplified)
    uint32_t config = SERIAL_8N1;
    if (cfg.dataBits == 8 && cfg.parity == 0 && cfg.stopBits == 1) config = SERIAL_8N1;
    else if (cfg.dataBits == 8 && cfg.parity == 1 && cfg.stopBits == 1) config = SERIAL_8O1;
    else if (cfg.dataBits == 8 && cfg.parity == 2 && cfg.stopBits == 1) config = SERIAL_8E1;
    else if (cfg.dataBits == 8 && cfg.parity == 0 && cfg.stopBits == 2) config = SERIAL_8N2;
    // add more as needed
    _serial->begin(cfg.baud, config, rx, tx);
}

void UartService::end() {
    if (_serial) {
        _serial->end();
        _serial = nullptr;
    }
}

void UartService::bridge(Stream& src, Stream& dst) {
    if (!_serial) return;
    while (src.available()) _serial->write(src.read());
    while (_serial->available()) dst.write(_serial->read());
}

size_t UartService::write(const uint8_t* data, size_t len) {
    if (!_serial) return 0;
    return _serial->write(data, len);
}

size_t UartService::write(uint8_t c) {
    if (!_serial) return 0;
    return _serial->write(c);
}

int UartService::available() {
    if (!_serial) return 0;
    return _serial->available();
}

int UartService::read() {
    if (!_serial) return -1;
    return _serial->read();
}

void UartService::setConfig(const UartConfig& cfg) {
    if (_serial) {
        _serial->end();
        begin(_rx, _tx, cfg);
    } else {
        _cfg = cfg;
    }
}

UartConfig UartService::getConfig() const { return _cfg; }

void UartService::clearBuffer() {
    if (_serial) while (_serial->available()) _serial->read();
}

String UartService::readLine(unsigned long timeout) {
    if (!_serial) return "";
    unsigned long start = millis();
    String line;
    while (millis() - start < timeout) {
        if (_serial->available()) {
            char c = _serial->read();
            if (c == '\n' || c == '\r') {
                if (line.length() > 0) break;
            } else {
                line += c;
            }
        }
    }
    return line;
}