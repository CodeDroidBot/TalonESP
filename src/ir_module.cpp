// ir_module.cpp
#include "ir_module.h"
#include "config.h"
#include "display.h"
#include "buttons.h"
#include "sdcard.h"
#include "menu.h"
#include "helpers.h"
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <map>
#include <math.h>

// ====================================================================
//  Include icon assets and compression support
// ====================================================================
#include "ir_remote.h"


// Forward declarations for icon decompression (defined in ir_remote.cpp)
extern CompressIcon* compress_icon_alloc(size_t decode_buf_size);
extern void compress_icon_free(CompressIcon* instance);
extern void compress_icon_decode(CompressIcon* instance, const uint8_t* icon_data, uint8_t** output);

// ====================================================================
//  Forward declarations
// ====================================================================
static void irRedrawCurrentScreen();
static void drawIrSignalPicker();

// ====================================================================
//  Hardware constants
// ====================================================================
#ifndef IR_DEFAULT_KHZ
#define IR_DEFAULT_KHZ 38
#endif

// A/C remotes are state-based protocols and their raw captures commonly
// run to several hundred (sometimes 700-900+) mark/space entries - far
// longer than the ~68 entries a simple NEC signal needs. 512 was cutting
// those off. 1024 matches both Flipper's own raw-timing file format cap
// ("Maximum timings amount is 1024") and IRremoteESP8266's documented
// buffer size for enabling A/C decode support, so captures/replays/.ir
// files stay compatible with either.
static constexpr uint16_t kMaxRawLen = 1024;
// Some A/C units leave tens of milliseconds between a message and its
// repeat; the default ~15ms timeout can split that into two truncated
// decodes instead of one. 50ms is the value IRremoteESP8266's own
// examples recommend for "messages with big gaps like ... some aircon
// units".
static constexpr uint8_t kIrRecvTimeoutMs = 50;
static constexpr uint16_t kIrKhz = IR_DEFAULT_KHZ;
static constexpr uint32_t kIrFreqHz = kIrKhz * 1000;
static constexpr uint8_t kMinRepeat = 1;
static constexpr uint8_t kMaxRepeat = 10;
static constexpr uint32_t kMinAutoInterval = 100;
static constexpr uint32_t kMaxAutoInterval = 5000;
static constexpr uint32_t kDefaultAutoInterval = 1000;

// ====================================================================
//  IR file signal structure
// ====================================================================
struct IrFileSignal {
    String name;
    bool isRaw = true;
    uint16_t freqKhz = kIrKhz;
    std::vector<uint16_t> raw;
    String protocol;
    uint32_t address = 0;
    uint32_t command = 0;
    uint64_t value = 0;
    uint16_t bits = 0;
};
static constexpr size_t kMaxSignalsPerFile = 64;

// ====================================================================
//  Internal static variables
// ====================================================================
// Buffer sized to kMaxRawLen (not the library's small default) so long,
// multi-hundred-entry A/C captures fit whole instead of overflowing.
static IRrecv   irrecv(PIN_IR_RX, kMaxRawLen, kIrRecvTimeoutMs, false);
static IRsend   irsend(PIN_IR_TX);

static bool             hasCapture = false;
static decode_results   lastDecode{};
static uint16_t         rawBuf[kMaxRawLen]{};
static uint16_t         rawLen = 0;

static uint8_t          repeatCount = 1;
static bool             autoTxEnabled = false;
static uint32_t         autoIntervalMs = kDefaultAutoInterval;
static uint32_t         lastAutoTxTime = 0;

bool g_irPickerMode = false;

// ---- Multi-signal file signals ----
static std::vector<IrFileSignal> g_openedSignals;
static String g_lastSendError;

// ---- Signal picker ----
static SimpleMenu irSignalMenu({});
static bool irSignalPickerActive = false;
static String irSignalPickerFileLabel;
static String currentIrFileName;

// ---- File action menu ----
static SimpleMenu irFileActionMenu({"Spam All", "Choose Command", "Back"});
static bool irInFileActions = false;

// Spam state
static bool irSpamRunning = false;
static unsigned long irSpamLastSend = 0;
static uint32_t irSpamIndex = 0;
static uint32_t irSpamTotalSignals = 0;
static const unsigned long IR_SPAM_INTERVAL_MS = 250;

// ---- Debounce for capture ----
static unsigned long lastCaptureTime = 0;
static const unsigned long CAPTURE_DEBOUNCE_MS = 200;

// ====================================================================
//  Icon drawing and key‑to‑icon mapping
// ====================================================================
static CompressIcon* g_iconDecoder = nullptr;

static void irDrawIcon(int x, int y, const Icon* icon, uint16_t fgColor, uint16_t bgColor) {
    if (!icon || icon->frame_count == 0) return;
    if (!g_iconDecoder) {
        g_iconDecoder = compress_icon_alloc(128 * 64 / 8 + 4);
    }
    const uint8_t* frameData = icon->frames[0];
    uint8_t* decodedBitmap = nullptr;
    compress_icon_decode(g_iconDecoder, frameData, &decodedBitmap);

    uint16_t w = icon->width;
    uint16_t h = icon->height;
    uint16_t bytesPerRow = (w + 7) / 8;
    for (uint16_t row = 0; row < h; row++) {
        for (uint16_t col = 0; col < w; col++) {
            uint16_t byteIdx = row * bytesPerRow + col / 8;
            uint8_t bit = 1 << (7 - (col % 8));
            bool pixel = (decodedBitmap[byteIdx] & bit) != 0;
            tft.drawPixel(x + col, y + row, pixel ? fgColor : bgColor);
        }
    }
}

static std::map<String, const Icon*> g_keyIconMap = {
    {"POWER", &I_power_19x20},
    {"VOL+", &I_volup_24x21},
    {"VOL-", &I_voldown_24x21},
    {"MUTE", &I_mute_19x20},
    {"CH+", &I_ch_up_24x21},
    {"CH-", &I_ch_down_24x21},
    {"PLAY", &I_play_19x20},
    {"PAUSE", &I_pause_19x20},
    {"NEXT", &I_next_19x20},
    {"PREV", &I_prev_19x20},
    {"STOP", &I_off_19x20},
    {"REC", &I_red_19x20},
    {"MENU", &I_settings_10px},
    {"OK", &I_Ok_btn_9x9},
    {"UP", &I_InfraredArrowUp_4x8},
    {"DOWN", &I_InfraredArrowDown_4x8},
    {"LEFT", &I_ButtonLeft_4x7},
    {"RIGHT", &I_ButtonRight_4x7},
};

// ====================================================================
//  Built‑in Universal Remote (centered landscape layout)
// ====================================================================

// Categories and actions
static const char* universalCategories[] = {"TV", "Audio", "Projector", "LEDs", "Fans", "ACs"};
static const char* universalFiles[] = {"tv.ir", "audio.ir", "projectors.ir", "leds.ir", "fans.ir", "ac.ir"};
static const char* universalActions[][8] = {
    {"Power", "Mute", "Ch_next", "Vol_up", "Ch_prev", "Vol_dn", nullptr, nullptr},
    {"Power", "Mute", "Play", "Vol_up", "Pause", "Vol_dn", "Prev", "Next"},
    {"Power", "Mute", "Vol_up", "Vol_dn", nullptr, nullptr, nullptr, nullptr},
    {"Power_on", "Power_off", "Brightness_up", "Brightness_dn", "Red", "Green", "Blue", "White"},
    {"Power", "Mode", "Speed_up", "Speed_dn", "Rotate", "Timer", nullptr, nullptr},
    {"Off", "Dh", "Cool_hi", "Heat_hi", "Cool_lo", "Heat_lo", nullptr, nullptr}
};
static const byte universalActionCounts[] = {6, 8, 6, 8, 6, 6};
static const char* universalLabels[][8] = {
    {"POWER",   "MUTE",   "CHANNEL", "VOLUME", "CHANNEL", "VOLUME", nullptr, nullptr},        // TV
    {"POWER",   "MUTE",   "PLAY",    "VOLUME", "PAUSE",   "VOLUME", "PREV",  "NEXT"},         // Audio
    {"POWER",   "MUTE",   "VOLUME",  "VOLUME", nullptr,   nullptr,  nullptr, nullptr},        // Projector
    {nullptr,   nullptr,  nullptr,   nullptr,  nullptr,   nullptr,  nullptr, nullptr},        // LEDs
    {nullptr,   nullptr,  nullptr,   nullptr,  nullptr,   nullptr,  nullptr, nullptr},        // Fans
    {"OFF",     "DRY",    "COOL+",   "HEAT+",  "COOL-",   "HEAT-",  nullptr, nullptr}         // ACs
};

// ---- Per-category preferred column count (-1 = auto-pick). ----
// TV, Audio, Projector, ACs use a fixed 2-column layout.
static const int8_t universalPreferredCols[] = { 2, 2, 2, -1, -1, 2 };

// Panel titles (displayed at the top of the remote screen)
static const char* universalPanelTitles[] = {"TV", "Audio player", "Projector", "LEDs", "Fan remote", "AC"};

// Layout constants (centered)
static int CELL_W = 26;               // recomputed each draw
static int CELL_H = 26;
static const int GAP = 8;             // gap between cells
static const int COLS = 4;            // max columns
static const int MARGIN = 10;         // outer screen margin
static const int FOOTER_HEIGHT = 14;  // bottom hint strip
static const int CELL_MAX = 110;      // cap for large panels
static const int CELL_MIN = 44;  // space for bottom hint

// Grid top‑left is computed in drawUniversalRemoteScreen()
static int gridLeft = 0;
static int gridTop = 0;
static int usedCols = 0;
static int usedRows = 0;

// Calculate button positions on the fly (relative to grid origin)
static void getButtonPosition(int index, int& x, int& y) {
    int col = index % usedCols;
    int row = index / usedCols;
    x = gridLeft + col * (CELL_W + GAP);
    y = gridTop + row * (CELL_H + GAP);
}

// Universal remote state
// universalMenuActive: true any time we're anywhere inside the Universal
//   Remote flow (category list OR the action/remote screen). The outer
//   dispatcher should use irIsUniversalRemoteActive() to check this before
//   routing input elsewhere, and stop once it goes false.
// universalActive: true only while the action/remote screen (the actual
//   button grid for a chosen category) is showing, false while the category
//   list is showing. This mirrors the flag's meaning in the original
//   reference implementation.
static bool universalMenuActive = false;
static bool universalActive = false;
static uint8_t universalSavedRotation = 0;
static bool   universalRotationForced = false;

// Forces landscape for the Universal Remote's grid/icon layout regardless
// of whatever orientation the rest of the app is using, and remembers
// what was active so it can be put back exactly on exit. TFT_eSPI's
// rotation values come in a portrait/landscape pair (0<->1, 2<->3), so
// toggling the low bit swaps orientation while preserving whichever of
// the two mirrored variants was already in use - this is what avoids
// the earlier "forcing rotation(0) renders sideways/mirrored" bug: that
// bug came from hardcoding an absolute rotation value instead of relating
// it to whatever was already active.
static void universalForceLandscape() {
    // Always force 270° (TFT_eSPI rotation value 3) while the universal
    // remote is on screen. The previous implementation toggled the low
    // bit of whatever rotation was already active, which produced 90°
    // on some devices and 270° on others depending on the user's
    // starting orientation. The remote is designed for a fixed
    // landscape layout, so hardcoding 3 gives the same result every time.
    universalSavedRotation = tft.getRotation();
    if (universalSavedRotation != 3) {
        tft.setRotation(0);
        universalRotationForced = true;
    } else {
        universalRotationForced = false;
    }
}

static void universalRestoreRotation() {
    if (universalRotationForced) {
        tft.setRotation(universalSavedRotation);
        universalRotationForced = false;
    }
}
static byte universalCategory = 0;
static byte universalCatIndex = 0;   // persists across category navigation
static byte universalSelectedAction = 0;
static bool universalPaused = false;
static bool universalLoadError = false;
static bool universalLoadCanceled = false;
static unsigned long universalBackIgnoreUntil = 0;
static uint16_t universalSignalCount = 0;
static uint16_t universalSignalPosition = 0;
#define MAX_UNIVERSAL_SIGNAL_INDICES 1000
static uint32_t universalSignalOffsets[MAX_UNIVERSAL_SIGNAL_INDICES];
static String currentUniversalFile = "";

// Sending state
static bool irUniversalSending = false;
static String irUniversalAction = "";
static unsigned long irUniversalLastSend = 0;
static const unsigned long UNIVERSAL_SEND_DELAY_MS = 200;

// ====================================================================
//  Helper functions for hex parsing
// ====================================================================
static String uint32ToString(uint32_t value) {
    char buffer[12] = {0};
    snprintf(buffer, sizeof(buffer), "%02X %02X %02X %02X",
             value & 0xFF, (value >> 8) & 0xFF, (value >> 16) & 0xFF, (value >> 24) & 0xFF);
    return String(buffer);
}

static uint64_t parseHexBytesToUint64LE(String str) {
    str.trim();
    uint64_t value = 0;
    int byteIndex = 0;
    while (str.length() > 0 && byteIndex < 8) {
        int spaceIndex = str.indexOf(' ');
        String byteStr;
        if (spaceIndex == -1) {
            byteStr = str;
            str = "";
        } else {
            byteStr = str.substring(0, spaceIndex);
            str = str.substring(spaceIndex + 1);
        }
        byteStr.trim();
        if (byteStr.length() > 0) {
            value |= (static_cast<uint64_t>(strtoul(byteStr.c_str(), nullptr, 16)) & 0xFF) << (byteIndex * 8);
            byteIndex++;
        }
    }
    return value;
}

static String normalizeProtocolName(String protocol) {
    protocol.trim();
    const String repeatSuffix = F(" (Repeat)");
    if (protocol.endsWith(repeatSuffix)) {
        protocol.remove(protocol.length() - repeatSuffix.length());
        protocol.trim();
    }
    return protocol;
}

static String aliasProtocolNameForLibrary(const String& upperProtocol) {
    if (upperProtocol == "NECEXT" || upperProtocol == "NEC_EXT") return "NEC";
    if (upperProtocol == "KASEIKYO" || upperProtocol == "PANASONIC_OLD") return "PANASONIC";
    if (upperProtocol == "SAMSUNG32") return "SAMSUNG";
    if (upperProtocol == "SIRC" || upperProtocol == "SIRC15" || upperProtocol == "SIRC20" ||
        upperProtocol == "SIRC12")
        return "SONY";
    if (upperProtocol == "RC5X") return "RC5";
    if (upperProtocol == "SANYO") return "SANYO_LC7461";
    if (upperProtocol == "FUJITSU") return "FUJITSU_AC";
    if (upperProtocol == "MITSUBISHI_AC" || upperProtocol == "MITSUBISHI2") return "MITSUBISHI_AC";
    if (upperProtocol == "DAIKIN2" || upperProtocol == "DAIKIN64") return upperProtocol;
    return upperProtocol;
}

static bool irProtocolIsSendable(String protocol) {
    protocol = normalizeProtocolName(protocol);
    protocol.toUpperCase();
    static const char* kExplicit[] = {
        "NEC", "NECEXT", "NEC42", "NEC42EXT", "SONY", "SIRC", "SIRC12", "SIRC15", "SIRC20",
        "SAMSUNG", "SAMSUNG32", "RC5", "RC5X", "RC6", "EPSON", "KASEIKYO",
        "LG", "SHARP", "JVC", "SANYO", "SANYO_LC7461", "PIONEER", "RCA"
    };
    for (const char* p : kExplicit) {
        if (protocol == p) return true;
    }
    return strToDecodeType(aliasProtocolNameForLibrary(protocol).c_str()) != decode_type_t::UNKNOWN;
}

// ====================================================================
//  Core IR hardware
// ====================================================================
static bool irInitialized = false;

void irInit() {
    if (irInitialized) return;

    pinMode(PIN_IR_RX, INPUT_PULLUP);
    irrecv.enableIRIn();
    irrecv.setUnknownThreshold(12);
    irsend.begin();

    // Ensure the TX pin starts idle (LED off)
    pinMode(PIN_IR_TX, OUTPUT);
    digitalWrite(PIN_IR_TX, LOW);

    irInitialized = true;
    Serial.println("[IR] Init OK");
}

void irDeinit() {
    if (!irInitialized) return;

    // 1. Stop the receiver — this detaches the interrupt and frees
    //    the RMT channel that IRrecv was using.
    irrecv.disableIRIn();

    // 2. Force the TX pin to a floating input so no residual current
    //    can flow through the IR LED. An OUTPUT-LOW would still sink
    //    any leakage; INPUT (high-Z) is safest.
    pinMode(PIN_IR_TX, INPUT);

    // 3. Optionally put the RX pin back to plain input so the
    //    internal pull-up doesn't keep sourcing current through the
    //    receiver's open-collector output.
    pinMode(PIN_IR_RX, INPUT);

    irInitialized = false;
    Serial.println("[IR] Deinit OK");
}

bool irIsInitialized() { return irInitialized; }

static constexpr uint16_t kMinAcceptedRawLen = 32;
static constexpr uint32_t kMinTotalDurationUs = 800;

bool irReceiveLoop() {
    if (!irrecv.decode(&lastDecode)) return false;

    Serial.printf("[IR] decode() ok: type=%d rawlen=%d overflow=%d\n",
                  (int)lastDecode.decode_type, lastDecode.rawlen, lastDecode.overflow);

    if (lastDecode.overflow) {
        // Signal ran past our capture buffer (kMaxRawLen raw entries) -
        // seen almost exclusively with long A/C state signals. The
        // capture is incomplete, so discard it rather than treat it as
        // a usable (but silently truncated) result, and log it
        // distinctly from plain noise so it's clear the fix is raising
        // kMaxRawLen further, not that the remote/receiver is faulty.
        Serial.println("[IR] Capture buffer overflow - signal longer than kMaxRawLen, discarded");
        irrecv.resume();
        return false;
    }

    const uint16_t rawlen = lastDecode.rawlen;
    const bool isKnownProtocol = (lastDecode.decode_type != decode_type_t::UNKNOWN);

    bool longEnough = rawlen > 1 && (rawlen - 1) <= kMaxRawLen &&
                       (isKnownProtocol || (rawlen - 1) >= kMinAcceptedRawLen);

    if (longEnough) {
        uint32_t totalUs = 0;
        for (uint16_t i = 1; i < rawlen; i++) totalUs += lastDecode.rawbuf[i] * kRawTick;
        if (totalUs < kMinTotalDurationUs) {
            Serial.println("Ignored short signal (noise)");
            irrecv.resume();
            return false;
        }

        unsigned long now = millis();
        if (now - lastCaptureTime < CAPTURE_DEBOUNCE_MS) {
            Serial.println("Ignored due to debounce");
            irrecv.resume();
            return false;
        }
        lastCaptureTime = now;

        Serial.print("IR received: ");
        Serial.println(resultToHumanReadableBasic(&lastDecode));
        for (uint16_t i = 1; i < rawlen; i++) rawBuf[i - 1] = lastDecode.rawbuf[i] * kRawTick;
        rawLen = rawlen - 1;
        hasCapture = true;
        Serial.printf("Raw len: %d pulses, total duration: %lu us\n", rawLen, totalUs);
        irRedrawCurrentScreen();
    } else {
        Serial.println("Ignored short/noise blip");
    }
    irrecv.resume();
    return hasCapture;
}

String irGetLastCodeSummary() {
    return hasCapture ? resultToHumanReadableBasic(&lastDecode) : "No code";
}
bool irHasCapture() { return hasCapture; }
const uint32_t* irGetLastRawUs() {
    static uint32_t converted[kMaxRawLen];
    if (!hasCapture || rawLen == 0) return nullptr;
    for (uint16_t i = 0; i < rawLen; i++) converted[i] = rawBuf[i];
    return converted;
}
uint16_t irGetLastRawLen() { return hasCapture ? rawLen : 0; }
uint32_t irGetLastRawDurationMs() {
    if (!hasCapture || rawLen == 0) return 0;
    uint32_t totalUs = 0;
    for (uint16_t i = 0; i < rawLen; i++) totalUs += rawBuf[i];
    return totalUs / 1000;
}
uint8_t irGetRepeat() { return repeatCount; }
void irSetRepeat(uint8_t n) {
    if (n < kMinRepeat) n = kMinRepeat;
    if (n > kMaxRepeat) n = kMaxRepeat;
    repeatCount = n;
}
bool irGetAutoTx() { return autoTxEnabled; }
void irSetAutoTx(bool on) { autoTxEnabled = on; if (on) lastAutoTxTime = 0; }
uint32_t irGetAutoTxIntervalMs() { return autoIntervalMs; }
void irSetAutoTxIntervalMs(uint32_t ms) {
    if (ms < kMinAutoInterval) ms = kMinAutoInterval;
    if (ms > kMaxAutoInterval) ms = kMaxAutoInterval;
    autoIntervalMs = ms;
}
void irAutoTxLoop() {
    if (!autoTxEnabled || !hasCapture) return;
    if (millis() - lastAutoTxTime >= autoIntervalMs) {
        lastAutoTxTime = millis();
        irReplayLastFromRAM();
    }
}
bool irReplayLastFromRAM() {
    irInit();
    if (!hasCapture || rawLen == 0) return false;
    for (uint8_t i = 0; i < repeatCount; i++) {
        irsend.sendRaw(rawBuf, rawLen, kIrKhz);
        if (i < repeatCount - 1) delay(40);
    }
    return true;
}

// ====================================================================
//  Save / Load .ir files
// ====================================================================
bool irSaveLastCodeToSD(const char* path) {
    if (!hasCapture || rawLen == 0 || !sdIsMounted()) return false;
    if (!SD.exists("/ir") && !SD.mkdir("/ir")) return false;
    File f = SD.open(path, FILE_WRITE);
    if (!f) return false;

    uint16_t freq = (uint16_t)kIrFreqHz;
    uint16_t len  = rawLen;
    bool ok = (f.write((const uint8_t*)&freq, sizeof(freq)) == sizeof(freq));
    if (ok) ok = (f.write((const uint8_t*)&len, sizeof(len)) == sizeof(len));
    if (ok) ok = (f.write((const uint8_t*)rawBuf, len * sizeof(uint16_t)) == (size_t)(len * sizeof(uint16_t)));
    f.close();
    return ok;
}

static uint8_t irHexByte(const String& tok) {
    uint8_t v = 0;
    for (unsigned i = 0; i < tok.length(); i++) {
        char c = tok[i];
        uint8_t nibble;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') nibble = 10 + (c - 'A');
        else continue;
        v = (v << 4) | nibble;
    }
    return v;
}

static uint32_t irParseLeHexBytes(String s) {
    s.trim();
    uint32_t value = 0;
    int shift = 0;
    int start = 0;
    while (start < (int)s.length() && shift < 32) {
        int sp = s.indexOf(' ', start);
        String tok = (sp < 0) ? s.substring(start) : s.substring(start, sp);
        tok.trim();
        if (tok.length() > 0) {
            value |= ((uint32_t)irHexByte(tok)) << shift;
            shift += 8;
        }
        if (sp < 0) break;
        start = sp + 1;
    }
    return value;
}

static std::vector<IrFileSignal> irParseFileText(const String& content) {
    std::vector<IrFileSignal> signals;
    IrFileSignal cur;
    bool haveSignal = false;

    auto flush = [&]() {
        bool valid = cur.isRaw ? !cur.raw.empty() : cur.protocol.length() > 0;
        if (haveSignal && valid && signals.size() < kMaxSignalsPerFile) {
            signals.push_back(cur);
        }
        cur = IrFileSignal();
        haveSignal = false;
    };

    int pos = 0;
    int len = content.length();
    while (pos < len) {
        int nl = content.indexOf('\n', pos);
        String line = (nl < 0) ? content.substring(pos) : content.substring(pos, nl);
        pos = (nl < 0) ? len : nl + 1;
        line.trim();
        if (line.length() == 0 || line.startsWith("#") ||
            line.startsWith("Filetype:") || line.startsWith("Version:")) continue;

        if (line.startsWith("name:")) {
            flush();
            cur.name = line.substring(5);
            cur.name.trim();
            haveSignal = true;
        } else if (line.startsWith("type:")) {
            String t = line.substring(5); t.trim();
            cur.isRaw = (t == "raw");
        } else if (line.startsWith("frequency:")) {
            uint32_t hz = (uint32_t)line.substring(10).toInt();
            cur.freqKhz = (uint16_t)(hz / 1000);
            if (cur.freqKhz == 0) cur.freqKhz = kIrKhz;
        } else if (line.startsWith("data:") && cur.isRaw) {
            String dataLine = line.substring(5); dataLine.trim();
            int start = 0;
            while (start < (int)dataLine.length() && cur.raw.size() < kMaxRawLen) {
                int sp = dataLine.indexOf(' ', start);
                String tok = (sp < 0) ? dataLine.substring(start) : dataLine.substring(start, sp);
                if (tok.length() > 0) cur.raw.push_back((uint16_t)tok.toInt());
                if (sp < 0) break;
                start = sp + 1;
            }
        } else if (line.startsWith("protocol:")) {
            cur.protocol = line.substring(9); cur.protocol.trim();
        } else if (line.startsWith("address:")) {
            cur.address = irParseLeHexBytes(line.substring(8));
        } else if (line.startsWith("command:")) {
            cur.command = irParseLeHexBytes(line.substring(8));
        } else if (line.startsWith("value:")) {
            cur.value = parseHexBytesToUint64LE(line.substring(6));
        } else if (line.startsWith("bits:")) {
            cur.bits = (uint16_t)line.substring(5).toInt();
        }
    }
    flush();
    return signals;
}

static bool irLoadFileSignals(const char* path, std::vector<IrFileSignal>& out) {
    out.clear();
    if (!sdIsMounted()) return false;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    size_t fileSize = f.size();
    f.seek(0);
    uint16_t freqHz, len;
    bool ok = false;
    if (f.read((uint8_t*)&freqHz, sizeof(freqHz)) == sizeof(freqHz) &&
        f.read((uint8_t*)&len, sizeof(len)) == sizeof(len)) {
        if (fileSize == (size_t)(4 + len * 2) && len > 0 && len <= kMaxRawLen) {
            std::vector<uint16_t> raw(len);
            if (f.read((uint8_t*)raw.data(), len * sizeof(uint16_t)) == (int)(len * sizeof(uint16_t))) {
                IrFileSignal sig;
                sig.name = "Signal";
                sig.isRaw = true;
                sig.freqKhz = freqHz / 1000;
                if (sig.freqKhz == 0) sig.freqKhz = kIrKhz;
                sig.raw = std::move(raw);
                out.push_back(std::move(sig));
                ok = true;
            }
        }
    }

    if (!ok) {
        f.seek(0);
        uint32_t magic;
        if (f.read((uint8_t*)&magic, sizeof(magic)) == sizeof(magic)) {
            if (magic == 0x49525230) {
                struct { uint32_t magic; uint16_t version, khz, rawLen; uint8_t decodeType, reserved; uint16_t bits; uint64_t value; } header;
                f.seek(0);
                if (f.read((uint8_t*)&header, sizeof(header)) == sizeof(header) &&
                    header.magic == 0x49525230 && header.version == 1 &&
                    header.rawLen > 0 && header.rawLen <= kMaxRawLen) {
                    std::vector<uint16_t> raw(header.rawLen);
                    if (f.read((uint8_t*)raw.data(), header.rawLen * sizeof(uint16_t)) == (int)(header.rawLen * sizeof(uint16_t))) {
                        IrFileSignal sig;
                        sig.name = "Signal";
                        sig.isRaw = true;
                        sig.freqKhz = header.khz ? header.khz : kIrKhz;
                        sig.raw = std::move(raw);
                        out.push_back(std::move(sig));
                        ok = true;
                    }
                }
            }
        }
    }

    if (!ok) {
        f.seek(0);
        String content;
        content.reserve(fileSize);
        while (f.available()) content += (char)f.read();
        std::vector<IrFileSignal> parsed = irParseFileText(content);
        if (!parsed.empty()) {
            out = std::move(parsed);
            ok = true;
        }
    }

    f.close();
    return ok && !out.empty();
}

// ====================================================================
//  RCA protocol (hand-encoded - IRremoteESP8266 has no native RCA
//  support, so we build the raw pulse train ourselves and hand it to
//  sendRaw(), the same way an unrecognized signal is replayed).
//
//  Timing/layout per Flipper Zero's documented RCA spec: 500us unit,
//  56kHz carrier, MSB-first 4-bit address then 8-bit command, each
//  immediately followed by its bitwise complement (matches the
//  documented 4000/4000us header and 4-bit/8-bit field widths).
// ====================================================================
static constexpr uint16_t kRcaUnitUs = 500;
static constexpr uint16_t kRcaKhz = 56;

static void rcaAppendBit(std::vector<uint16_t>& buf, bool bit) {
    buf.push_back(kRcaUnitUs);                            // mark
    buf.push_back(bit ? kRcaUnitUs * 4 : kRcaUnitUs * 2);  // space: 2u=0, 4u=1
}

static bool irSendRCA(uint8_t addr4, uint8_t cmd8) {
    addr4 &= 0x0F;
    const uint8_t addrInv = (~addr4) & 0x0F;
    const uint8_t cmdInv  = (~cmd8) & 0xFF;

    std::vector<uint16_t> buf;
    buf.reserve(2 + 24 * 2 + 2);
    buf.push_back(kRcaUnitUs * 8);  // header mark  (4000us)
    buf.push_back(kRcaUnitUs * 8);  // header space (4000us)
    for (int8_t i = 3; i >= 0; i--) rcaAppendBit(buf, (addr4   >> i) & 1);
    for (int8_t i = 7; i >= 0; i--) rcaAppendBit(buf, (cmd8    >> i) & 1);
    for (int8_t i = 3; i >= 0; i--) rcaAppendBit(buf, (addrInv >> i) & 1);
    for (int8_t i = 7; i >= 0; i--) rcaAppendBit(buf, (cmdInv  >> i) & 1);
    buf.push_back(kRcaUnitUs);       // final mark
    buf.push_back(kRcaUnitUs * 16);  // trailing gap before any repeat

    if (buf.size() > kMaxRawLen) return false;  // never happens (~52 entries)
    static uint16_t sendBuf[kMaxRawLen];
    for (size_t i = 0; i < buf.size(); i++) sendBuf[i] = buf[i];
    for (uint8_t i = 0; i < repeatCount; i++) {
        irsend.sendRaw(sendBuf, buf.size(), kRcaKhz);
        if (i < repeatCount - 1) delay(40);
    }
    return true;
}

// ====================================================================
//  Advanced IR sending
// ====================================================================
static bool irSendSignal(const IrFileSignal& sig, String& errOut) {
    errOut = "";
    if (sig.isRaw) {
        if (sig.raw.empty()) { errOut = "Empty raw data"; return false; }
        static uint16_t buf[kMaxRawLen];
        uint16_t n = (sig.raw.size() < (size_t)kMaxRawLen) ? (uint16_t)sig.raw.size() : kMaxRawLen;
        for (uint16_t i = 0; i < n; i++) buf[i] = sig.raw[i];
        for (uint8_t i = 0; i < repeatCount; i++) {
            irsend.sendRaw(buf, n, sig.freqKhz);
            if (i < repeatCount - 1) delay(40);
        }
        return true;
    }

    String protocol = normalizeProtocolName(sig.protocol);
    protocol.toUpperCase();

    uint64_t fullValue = sig.value;
    uint16_t sendBits = sig.bits;
    uint32_t addr = sig.address;
    uint32_t cmd = sig.command;

    // ---- SAMSUNG ----
    if (protocol == "SAMSUNG32" || protocol == "SAMSUNG") {
        uint32_t data = 0;
        if (fullValue != 0) data = (uint32_t)(fullValue & 0xFFFFFFFF);
        else {
            uint8_t addressValue = addr & 0xFF;
            uint8_t commandValue = cmd & 0xFF;
            data = irsend.encodeSAMSUNG(addressValue, commandValue);
        }
        Serial.print(F("Sending Samsung: 0x")); Serial.println(data, HEX);
        irsend.sendSAMSUNG(data, 32);
        return true;
    }

    // ---- SONY ----
    if (protocol == "SONY" || protocol == "SIRC" || protocol == "SIRC15" || protocol == "SIRC20") {
        uint32_t sonyCode = 0;
        uint16_t sonyBits = (sendBits > 0) ? sendBits : 12;
        if (protocol == "SIRC15") sonyBits = 15;
        if (protocol == "SIRC20") sonyBits = 20;
        if (fullValue != 0) sonyCode = (uint32_t)(fullValue & 0xFFFFFFFF);
        else {
            uint16_t commandValue = cmd & 0xFFFF;
            uint16_t addressValue = addr & 0xFFFF;
            sonyCode = irsend.encodeSony(sonyBits, commandValue, addressValue);
        }
        Serial.print(F("Sending Sony: 0x")); Serial.println(sonyCode, HEX);
        irsend.sendSony(sonyCode, sonyBits, 2);
        return true;
    }

    // ---- NEC family (NEC / NECext / NEC42 / NEC42ext) ----
    // All four share the same header/bit timing and differ only in how
    // many address/command bits are sent and whether each half is
    // followed by its bitwise complement. Values are packed LSB-first
    // (address in the low bits) to match IRsend::encodeNEC()/sendNEC()'s
    // own convention - necCode must stay uint64_t or the 42-bit variants
    // get silently truncated to 32 bits.
    if (protocol == "NEC" || protocol == "NECEXT" || protocol == "NEC42" || protocol == "NEC42EXT") {
        uint64_t necCode = fullValue;
        uint16_t necBits = 32;
        if (necCode == 0) {
            if (protocol == "NEC") {
                necCode = irsend.encodeNEC((uint16_t)(addr & 0xFF), (uint8_t)(cmd & 0xFF));
                necBits = 32;
            } else if (protocol == "NECEXT") {
                uint32_t a = addr & 0xFFFF;   // 16-bit address, no complement
                uint32_t c = cmd & 0xFFFF;    // 16-bit command, no complement
                necCode = (uint64_t)a | ((uint64_t)c << 16);
                necBits = 32;
            } else if (protocol == "NEC42") {
                uint32_t a = addr & 0x1FFF;              // 13-bit address
                uint32_t aInv = (~a) & 0x1FFF;
                uint32_t c = cmd & 0xFF;                  // 8-bit command
                uint32_t cInv = (~c) & 0xFF;
                necCode = (uint64_t)a | ((uint64_t)aInv << 13) |
                          ((uint64_t)c << 26) | ((uint64_t)cInv << 34);
                necBits = 42;
            } else {  // NEC42EXT
                uint32_t a = addr & 0x3FFFFFF;  // 26-bit address, no complement
                uint32_t c = cmd & 0xFFFF;       // 16-bit command, no complement
                necCode = (uint64_t)a | ((uint64_t)c << 26);
                necBits = 42;
            }
        } else if (sendBits > 0) {
            necBits = sendBits;
        } else if (protocol == "NEC42" || protocol == "NEC42EXT") {
            necBits = 42;
        }
        Serial.print(F("Sending NEC code: 0x")); Serial.print((uint32_t)(necCode & 0xFFFFFFFF), HEX);
        Serial.print(F(" (")); Serial.print(necBits); Serial.println(F(" bits)"));
        irsend.sendNEC(necCode, necBits);
        return true;
    }

    // ---- RCA ----
    if (protocol == "RCA") {
        uint8_t rcaAddr = (uint8_t)(addr & 0x0F);
        uint8_t rcaCmd  = (uint8_t)(cmd & 0xFF);
        if (fullValue != 0) {
            rcaAddr = (uint8_t)(fullValue & 0x0F);
            rcaCmd  = (uint8_t)((fullValue >> 4) & 0xFF);
        }
        Serial.print(F("Sending RCA: addr=0x")); Serial.print(rcaAddr, HEX);
        Serial.print(F(" cmd=0x")); Serial.println(rcaCmd, HEX);
        return irSendRCA(rcaAddr, rcaCmd);
    }

    // ---- RC6 ----
    if (protocol == "RC6") {
        uint64_t rc6Code = 0;
        uint16_t rc6Bits = (sendBits > 0) ? sendBits : kRC6Mode0Bits;
        if (fullValue != 0) rc6Code = fullValue;
        else rc6Code = irsend.encodeRC6(addr, (uint8_t)(cmd & 0xFF), rc6Bits);
        Serial.print(F("Sending RC6 code: 0x")); Serial.print((uint32_t)rc6Code, HEX);
        Serial.print(F(" (")); Serial.print(rc6Bits); Serial.println(F(" bits)"));
        irsend.sendRC6(rc6Code, rc6Bits);
        return true;
    }

    // ---- RC5 / RC5X ----
    if (protocol == "RC5" || protocol == "RC5X") {
        uint64_t rc5Code = 0;
        bool extended = (protocol == "RC5X") || (cmd > 0x3F);
        uint16_t rc5Bits = (sendBits > 0) ? sendBits : (extended ? kRC5XBits : kRC5Bits);
        if (fullValue != 0) rc5Code = fullValue;
        else if (extended) rc5Code = irsend.encodeRC5X((uint8_t)(addr & 0xFF), (uint8_t)(cmd & 0xFF));
        else rc5Code = irsend.encodeRC5((uint8_t)(addr & 0xFF), (uint8_t)(cmd & 0xFF));
        Serial.print(F("Sending RC5: 0x")); Serial.print((uint32_t)rc5Code, HEX);
        Serial.print(F(" (")); Serial.print(rc5Bits); Serial.println(F(" bits)"));
        irsend.sendRC5(rc5Code, rc5Bits);
        return true;
    }

    // ---- LG ----
    if (protocol == "LG" || protocol == "LG2") {
        uint32_t lgCode = 0;
        uint16_t lgBits = (sendBits > 0) ? sendBits : kLgBits;
        if (fullValue != 0) lgCode = (uint32_t)(fullValue & 0xFFFFFFFF);
        else lgCode = irsend.encodeLG((uint16_t)addr, (uint16_t)cmd);
        Serial.print(F("Sending LG code: 0x")); Serial.println(lgCode, HEX);
        if (protocol == "LG2") irsend.sendLG2(lgCode, lgBits);
        else irsend.sendLG(lgCode, lgBits);
        return true;
    }

    // ---- SHARP ----
    if (protocol == "SHARP") {
        uint32_t sharpCode = 0;
        uint16_t sharpBits = (sendBits > 0) ? sendBits : kSharpBits;
        if (fullValue != 0) sharpCode = (uint32_t)(fullValue & 0xFFFFFFFF);
        else sharpCode = irsend.encodeSharp((uint16_t)addr, (uint16_t)cmd);
        Serial.print(F("Sending Sharp code: 0x")); Serial.println(sharpCode, HEX);
        irsend.sendSharpRaw(sharpCode, sharpBits);
        return true;
    }

    // ---- JVC ----
    if (protocol == "JVC") {
        uint16_t jvcCode = 0;
        uint16_t jvcBits = (sendBits > 0) ? sendBits : kJvcBits;
        if (fullValue != 0) jvcCode = (uint16_t)(fullValue & 0xFFFF);
        else jvcCode = irsend.encodeJVC((uint8_t)(addr & 0xFF), (uint8_t)(cmd & 0xFF));
        Serial.print(F("Sending JVC code: 0x")); Serial.println(jvcCode, HEX);
        irsend.sendJVC(jvcCode, jvcBits);
        return true;
    }

    // ---- SANYO ----
    if (protocol == "SANYO" || protocol == "SANYO_LC7461") {
        uint64_t sanyoCode = 0;
        uint16_t sanyoBits = (sendBits > 0) ? sendBits : kSanyoLC7461Bits;
        if (fullValue != 0) sanyoCode = fullValue;
        else sanyoCode = irsend.encodeSanyoLC7461((uint16_t)addr, (uint8_t)(cmd & 0xFF));
        Serial.print(F("Sending Sanyo LC7461 code: 0x")); Serial.println((uint32_t)sanyoCode, HEX);
        irsend.sendSanyoLC7461(sanyoCode, sanyoBits);
        return true;
    }

    // ---- PIONEER ----
    if (protocol == "PIONEER") {
        uint64_t pioneerCode = 0;
        uint16_t pioneerBits = (sendBits > 0) ? sendBits : 64;
        if (fullValue != 0) pioneerCode = fullValue;
        else pioneerCode = irsend.encodePioneer((uint16_t)addr, (uint16_t)cmd);
        Serial.print(F("Sending Pioneer code: 0x")); Serial.println((uint32_t)pioneerCode, HEX);
        irsend.sendPioneer(pioneerCode, pioneerBits);
        return true;
    }

    // ---- EPSON ----
    if (protocol == "EPSON") {
        uint32_t epsonCode = 0;
        if (fullValue != 0) epsonCode = (uint32_t)(fullValue & 0xFFFFFFFF);
        else epsonCode = (cmd << 16) | (addr & 0xFFFF);
        uint16_t epsonBits = (sendBits > 0) ? sendBits : 32;
        Serial.print(F("Sending EPSON code: 0x")); Serial.print(epsonCode, HEX);
        Serial.print(F(" (")); Serial.print(epsonBits); Serial.println(F(" bits)"));
        irsend.sendEpson(epsonCode, epsonBits);
        return true;
    }

    // ---- Kaseikyo ----
    if (protocol == "KASEIKYO") {
        uint64_t kaseikyo = fullValue;
        if (kaseikyo == 0) {
            uint16_t manufacturer = (uint16_t)((addr >> 16) & 0xFFFF);
            uint8_t device = (uint8_t)((addr >> 8) & 0xFF);
            uint8_t subdevice = (uint8_t)(addr & 0xFF);
            uint8_t function = (uint8_t)(cmd & 0xFF);
            kaseikyo = irsend.encodePanasonic(manufacturer, device, subdevice, function);
        }
        Serial.print(F("Sending Kaseikyo: 0x")); Serial.println((uint32_t)(kaseikyo & 0xFFFFFFFF), HEX);
        irsend.sendPanasonic64(kaseikyo, sendBits > 0 ? sendBits : kPanasonicBits);
        return true;
    }

    // ---- Fallback ----
    decode_type_t protocolType = strToDecodeType(aliasProtocolNameForLibrary(protocol).c_str());
    if (protocolType != decode_type_t::UNKNOWN) {
        if (sendBits == 0) sendBits = IRsend::defaultBits(protocolType);
        if (fullValue != 0) {
            Serial.print(F("Sending generic ")); Serial.print(protocol);
            Serial.print(F(" value: 0x")); Serial.print(fullValue, HEX);
            Serial.print(F(" (")); Serial.print(sendBits); Serial.println(F(" bits)"));
            return irsend.send(protocolType, fullValue, sendBits);
        } else {
            uint64_t combined = ((uint64_t)(addr & 0xFFFFFFFF) << 32) | (cmd & 0xFFFFFFFF);
            Serial.print(F("Sending generic ")); Serial.print(protocol);
            Serial.print(F(" combined: 0x")); Serial.print(combined, HEX);
            Serial.print(F(" (")); Serial.print(sendBits); Serial.println(F(" bits)"));
            return irsend.send(protocolType, combined, sendBits);
        }
    }

    errOut = "Unsupported protocol: " + sig.protocol;
    Serial.println(errOut);
    return false;
}

// ---------- Multi-signal picking API ----------
bool irReplayFromSD(const char* path) {
    irInit();
    std::vector<IrFileSignal> signals;
    if (!irLoadFileSignals(path, signals)) {
        Serial.println("[IR] Failed to parse file: " + String(path));
        return false;
    }
    String err;
    bool ok = irSendSignal(signals[0], err);
    if (!ok) Serial.println("[IR] " + err);
    return ok;
}

int irOpenFileForPicking(const char* path) {
    currentIrFileName = String(path);
    g_openedSignals.clear();
    irLoadFileSignals(path, g_openedSignals);
    return (int)g_openedSignals.size();
}

String irOpenedFileSignalLabel(int index) {
    if (index < 0 || index >= (int)g_openedSignals.size()) return "";
    const IrFileSignal& s = g_openedSignals[index];
    String label = s.name.length() ? s.name : (String("Signal ") + String(index + 1));
    if (!s.isRaw && !irProtocolIsSendable(s.protocol)) label += "  (unsupported)";
    return label;
}

bool irSendOpenedFileSignal(int index) {
    irInit();
    g_lastSendError = "";
    if (index < 0 || index >= (int)g_openedSignals.size()) {
        g_lastSendError = "No such signal";
        return false;
    }
    bool ok = irSendSignal(g_openedSignals[index], g_lastSendError);
    if (!ok && g_lastSendError.length() == 0) g_lastSendError = "Send failed";
    return ok;
}
String irLastSendError() { return g_lastSendError; }

// ---------- Built‑in universal codes ----------
struct BuiltinCode { const char* label; decode_type_t type; uint16_t bits; uint64_t code; };
static const BuiltinCode builtinCodes[] = {
    {"TV Power", decode_type_t::NEC, 32, 0xE0E040BF},
    {"TV Vol+",  decode_type_t::NEC, 32, 0xE0E0E01F},
    {"TV Vol-",  decode_type_t::NEC, 32, 0xE0E0D02F},
    {"TV Mute",  decode_type_t::NEC, 32, 0xE0E0F00F}
};
int irUniversalCodeCount() { return sizeof(builtinCodes) / sizeof(builtinCodes[0]); }
String irUniversalCodeLabel(int idx) {
    if (idx < 0 || idx >= (int)(sizeof(builtinCodes)/sizeof(builtinCodes[0]))) return "Unknown";
    return builtinCodes[idx].label;
}
void irSendUniversalCode(int idx) {
    if (idx < 0 || idx >= (int)(sizeof(builtinCodes)/sizeof(builtinCodes[0]))) return;
    const auto& c = builtinCodes[idx];
    for (uint8_t i = 0; i < repeatCount; i++) {
        irsend.send(c.type, c.code, c.bits);
        if (i < repeatCount - 1) delay(40);
    }
}

void irSendRawDemo() { irsend.sendNEC(0xE0E040BF, 32); }

// ====================================================================
//  Accent color palette (custom RGB565 colors, additive to the app's
//  displayColor*() theme). Base backgrounds/text keep using the theme
//  functions so this still respects light/dark mode; these are only
//  used for status highlights, borders, pills, and progress fills so
//  Read / Send / Jammer read as one coherent, colorful, professional
//  set of screens instead of everything staying single-tone.
// ====================================================================
constexpr uint16_t irRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t UI_ACCENT_BLUE   = irRgb565(66, 150, 255);  // primary / neutral action
constexpr uint16_t UI_ACCENT_CYAN   = irRgb565(72, 222, 255);  // idle / listening / info
constexpr uint16_t UI_ACCENT_PURPLE = irRgb565(168, 120, 255); // secondary accent
constexpr uint16_t UI_ACCENT_GREEN  = irRgb565(70, 226, 148);  // success / captured / go
constexpr uint16_t UI_ACCENT_AMBER  = irRgb565(255, 176, 59);  // caution / paused
constexpr uint16_t UI_ACCENT_RED    = irRgb565(255, 87, 97);   // danger / jamming / stop

// ====================================================================
//  RGB Rainbow waveform support
// ====================================================================
static uint16_t rainbowTable[240];

static void buildRainbowTable() {
    for (int i = 0; i < 240; i++) {
        float hue = (float)i / 240.0f * 360.0f;
        float h = hue / 60.0f;
        int region = (int)floor(h);
        float f = h - region;
        uint8_t p = 0;
        uint8_t q = (uint8_t)(255 * (1 - f));
        uint8_t t = (uint8_t)(255 * f);
        uint8_t r, g, b;
        switch (region % 6) {
            case 0: r = 255; g = t;   b = p;   break;
            case 1: r = q;   g = 255; b = p;   break;
            case 2: r = p;   g = 255; b = t;   break;
            case 3: r = p;   g = q;   b = 255; break;
            case 4: r = t;   g = p;   b = 255; break;
            case 5: r = 255; g = p;   b = q;   break;
        }
        rainbowTable[i] = tft.color565(r, g, b);
    }
}

static uint16_t rainbowColor(int x, int width) {
    if (width <= 1) return rainbowTable[0];
    int idx = (x * 240) / width;
    if (idx < 0) idx = 0;
    if (idx >= 240) idx = 239;
    return rainbowTable[idx];
}

// ====================================================================
//  UI for Capture/Replay screen
// ====================================================================
static void drawWaveformBox(int x, int y, int w, int h, uint16_t borderColor = displayColorFgDim()) {
    tft.drawRect(x, y, w, h, borderColor);

    int innerX = x + 1;
    int innerY = y + 1;
    int innerW = w - 2;
    int innerH = h - 2;

    if (!irHasCapture() || irGetLastRawLen() == 0) {
        tft.fillRect(innerX, innerY, innerW, innerH, displayColorBg());
        tft.setTextColor(displayColorFgDim(), displayColorBg());
        tft.setTextSize(2);
        tft.setCursor(innerX + 4, innerY + 4);
        tft.print("No capture");
        return;
    }

    const uint32_t* raw = irGetLastRawUs();
    uint16_t len = irGetLastRawLen();
    uint32_t total = 0;
    for (uint16_t i = 0; i < len; i++) total += raw[i];
    if (total == 0) return;

    tft.fillRect(innerX, innerY, innerW, innerH, displayColorBg());

    for (int g = 1; g < 4; g++) {
        int gy = innerY + (innerH * g) / 4;
        tft.drawFastHLine(innerX, gy, innerW, displayColorFgDim());
    }

    const int highY = innerY + 8;
    const int lowY  = innerY + innerH - 8;
    const int right = innerX + innerW - 1;

    uint32_t tUs = 0;
    bool mark = true;
    int prevY = highY;
    for (uint16_t i = 0; i < len && tUs < total; i++) {
        uint32_t segStart = tUs, segEnd = tUs + raw[i];
        int x1 = innerX + 1 + (int)(segStart * (innerW - 2) / total);
        int x2 = innerX + 1 + (int)(segEnd * (innerW - 2) / total);
        if (x2 > right) x2 = right;
        int curY = mark ? highY : lowY;
        if (x2 > x1) {
            uint16_t col;
            if (mark) {
                int midX = (x1 + x2) / 2;
                col = rainbowColor(midX - x, innerW);
            } else {
                int midX = (x1 + x2) / 2;
                uint16_t bright = rainbowColor(midX - x, innerW);
                uint8_t r = (bright >> 11) & 0x1F;
                uint8_t g = (bright >> 5) & 0x3F;
                uint8_t b = bright & 0x1F;
                r = r * 2 / 5;
                g = g * 2 / 5;
                b = b * 2 / 5;
                col = tft.color565(r, g, b);
            }
            tft.drawFastHLine(x1, curY, x2 - x1, col);
        }
        if (prevY != curY && x1 > innerX + 1)
            tft.drawFastVLine(x1, min(prevY, curY), abs(prevY - curY), displayColorFgDim());
        prevY = curY;
        tUs = segEnd;
        mark = !mark;
    }
}

static void irRedrawCurrentScreen() {
    const int margin = 8;
    const int headerH = displayHeaderHeight();
    const int w = tft.width() - 2 * margin;
    const bool captured = irHasCapture();
    const uint16_t accent = captured ? UI_ACCENT_GREEN : UI_ACCENT_CYAN;

    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("IR Read");

    int availH = tft.height() - headerH - 4;
    const int statusH = (availH >= 220) ? 120 : (availH * 48 / 100);
    const int gap1 = 10;
    const int graphH = (availH >= 220) ? 90 : (availH * 34 / 100);
    const int gap2 = 10;
    const int ctrlH = (availH >= 220) ? 36 : (availH * 16 / 100);

    int y = headerH + 4;

    // Status card border/pill reflect state at a glance (green = captured,
    // cyan = still listening) instead of a flat neutral outline.
    tft.drawRoundRect(margin, y, w, statusH, 3, accent);
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setTextSize(2);
    tft.setCursor(margin + 4, y + 3);
    tft.print("Status");

    String pillTxt = captured ? "CAPTURED" : "LISTENING";
    int pillW = tft.textWidth(pillTxt) + 10;
    int pillX = margin + w - pillW - 6;
    int pillY = y + 5;
    tft.fillRoundRect(pillX, pillY, pillW, 14, 7, accent);
    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(pillX + 5, pillY + 3);
    tft.print(pillTxt);

    int lineY = y + 20;
    const int lineSpacing = 20;

    tft.setTextColor(accent, displayColorBg());
    tft.setTextSize(1);
    tft.setCursor(margin + 4, lineY);
    tft.print(captured ? "State: Captured" : "State: Listening");
    lineY += lineSpacing;

    if (captured) {
        tft.setTextColor(displayColorFg(), displayColorBg());
        tft.setCursor(margin + 4, lineY);
        String proto = irGetLastCodeSummary();
        tft.print("Proto: " + displayFitText(proto, w - 4, 2));
        lineY += lineSpacing;

        tft.setCursor(margin + 4, lineY);
        uint32_t durMs = irGetLastRawDurationMs();
        tft.print("Raw: " + String(irGetLastRawLen()) + " pulses  " + String(durMs) + "ms");
        lineY += lineSpacing;

        // Code readout is the one field that actually identifies the
        // signal, so it gets the accent color to stand out from the rest.
        tft.setCursor(margin + 4, lineY);
        tft.setTextColor(UI_ACCENT_PURPLE, displayColorBg());
        if (lastDecode.value != 0) {
            char buf[28];
            snprintf(buf, sizeof(buf), "Code: 0x%llX (%d bits)", (unsigned long long)lastDecode.value, lastDecode.bits);
            tft.print(buf);
        } else {
            tft.print("Code: --");
        }
        lineY += lineSpacing;
    } else {
        tft.setTextColor(displayColorFg(), displayColorBg());
        tft.setCursor(margin + 4, lineY);
        tft.print("Point remote at receiver");
        lineY += lineSpacing;
        tft.setCursor(margin + 4, lineY);
        tft.print("Press any button...");
        lineY += lineSpacing;
    }

    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setTextSize(1);
    tft.setCursor(margin + 4, lineY);
    tft.print("Repeat: " + String(irGetRepeat()) + "x  Auto: " + (irGetAutoTx() ? "ON" : "OFF"));

    y += statusH + gap1;
    drawWaveformBox(margin, y, w, graphH, accent);
    y += graphH + gap2;

    tft.fillRect(margin, y, w, ctrlH, displayColorBg());
    tft.drawRect(margin, y, w, ctrlH, displayColorFgDim());

    // Each hint gets its own accent color so the four actions read as
    // distinct commands rather than one undifferentiated line of text.
    tft.setTextSize(1);
    int cx = margin + 4;
    tft.setTextColor(UI_ACCENT_GREEN, displayColorBg());
    tft.setCursor(cx, y + 4);
    tft.print("OK:Save");
    cx += 65;
    tft.setTextColor(UI_ACCENT_CYAN, displayColorBg());
    tft.setCursor(cx, y + 4);
    tft.print("RIGHT:Replay");
    cx += 85;
    tft.setTextColor(UI_ACCENT_AMBER, displayColorBg());
    tft.setCursor(cx, y + 4);
    tft.print("UP/DN:Repeat");
    cx += 100;
    tft.setTextColor(UI_ACCENT_PURPLE, displayColorBg());
    tft.setCursor(cx, y + 4);
    tft.print("LEFT:Auto");

    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setTextSize(1);
    tft.setCursor(margin + 4, y + 20);
    tft.print("Repeat:" + String(irGetRepeat()) + "  Auto:" + (irGetAutoTx() ? "ON" : "OFF"));
}

void irEnterCaptureScreen() {
    irInit();  
    hasCapture = false;
    rawLen = 0;
    lastCaptureTime = 0;
    buildRainbowTable();
    irRedrawCurrentScreen();
}

void irHandleCaptureEvent(int evt) {
    if (evt == EVT_UP) {
        irSetRepeat(irGetRepeat() + 1);
        irRedrawCurrentScreen();
    } else if (evt == EVT_DOWN) {
        irSetRepeat(irGetRepeat() - 1);
        irRedrawCurrentScreen();
    } else if (evt == EVT_LEFT) {
        irSetAutoTx(!irGetAutoTx());
        irRedrawCurrentScreen();
    } else if (evt == EVT_OK) {
        if (irHasCapture()) {
            irSaveLastCodeToSD("/ir/capture.ir");
            irRedrawCurrentScreen();
        }
    } else if (evt == EVT_RIGHT) {
        if (irHasCapture()) {
            irReplayLastFromRAM();
            irRedrawCurrentScreen();
        }
    }
}

// ====================================================================
//  Universal Remote UI (centered landscape layout)
// ====================================================================

// cellX, cellY are the button cell's top-left corner (CELL_W x CELL_H).
// Icon glyphs are centered inside the cell using their real dimensions;
// fallback vector glyphs use a fixed +1,+1 inset that was tuned for them.
static void drawUniversalGlyph(const String& action,
                               int16_t cellX, int16_t cellY,
                               int16_t cellW, int16_t cellH,
                               bool selected) {
    const Icon* icon = nullptr;
    if (action == "Power" || action == "Power_on") icon = selected ? &I_power_hover_19x20 : &I_power_19x20;
    else if (action == "Power_off" || action == "Off") icon = selected ? &I_off_hover_19x20 : &I_off_19x20;
    else if (action == "Mute") icon = selected ? &I_mute_hover_19x20 : &I_mute_19x20;
    else if (action == "Vol_up" || action == "Speed_up") icon = selected ? &I_volup_hover_24x21 : &I_volup_24x21;
    else if (action == "Vol_dn" || action == "Speed_dn") icon = selected ? &I_voldown_hover_24x21 : &I_voldown_24x21;
    else if (action == "Ch_next") icon = selected ? &I_ch_up_hover_24x21 : &I_ch_up_24x21;
    else if (action == "Ch_prev") icon = selected ? &I_ch_down_hover_24x21 : &I_ch_down_24x21;
    else if (action == "Play") icon = selected ? &I_play_hover_19x20 : &I_play_19x20;
    else if (action == "Pause") icon = selected ? &I_pause_hover_19x20 : &I_pause_19x20;
    else if (action == "Prev") icon = selected ? &I_prev_hover_19x20 : &I_prev_19x20;
    else if (action == "Next") icon = selected ? &I_next_hover_19x20 : &I_next_19x20;
    else if (action == "Brightness_up") icon = selected ? &I_plus_hover_19x20 : &I_plus_19x20;
    else if (action == "Brightness_dn") icon = selected ? &I_minus_hover_19x20 : &I_minus_19x20;
    else if (action == "Red") icon = selected ? &I_red_hover_19x20 : &I_red_19x20;
    else if (action == "Green") icon = selected ? &I_green_hover_19x20 : &I_green_19x20;
    else if (action == "Blue") icon = selected ? &I_blue_hover_19x20 : &I_blue_19x20;
    else if (action == "White") icon = selected ? &I_white_hover_19x20 : &I_white_19x20;
    else if (action == "Mode") icon = selected ? &I_mode_hover_19x20 : &I_mode_19x20;
    else if (action == "Rotate") icon = selected ? &I_rotate_hover_19x20 : &I_rotate_19x20;
    else if (action == "Timer") icon = selected ? &I_timer_hover_19x20 : &I_timer_19x20;
    else if (action == "Dh") icon = selected ? &I_dry_hover_19x20 : &I_dry_19x20;
    else if (action == "Cool_hi" || action == "Heat_hi") icon = selected ? &I_max_hover_24x23 : &I_max_24x23;
    else if (action == "Cool_lo" || action == "Heat_lo") icon = selected ? &I_celsius_hover_24x23 : &I_celsius_24x23;

    // ---- Bitmap path (only when frame_count > 0, i.e. real assets present) ----
    if (icon && icon->frame_count > 0) {
        int16_t ix = cellX + (cellW - (int)icon->width) / 2;
        int16_t iy = cellY + (cellH - (int)icon->height) / 2;
        irDrawIcon(ix, iy, icon,
                   selected ? displayColorSelectFg() : displayColorFg(),
                   selected ? displayColorSelectBg() : displayColorBg());
        return;
    }

    // ---- Vector fallback (scaled to the cell) ----
    const int DESIGN = 20;               // design grid size (units)
    const int PAD    = 10;                // padding inside the cell, px
    int avail = min(cellW, cellH) - 2 * PAD;
    if (avail < DESIGN) avail = DESIGN;  // safety floor
    int s = avail / DESIGN;              // integer scale factor
    if (s < 1) s = 1;

    int size = DESIGN * s;
    int16_t ox = cellX + (cellW - size) / 2;
    int16_t oy = cellY + (cellH - size) / 2;

    auto X = [&](int u) -> int16_t { return (int16_t)(ox + u * s); };
    auto Y = [&](int v) -> int16_t { return (int16_t)(oy + v * s); };
    auto U = [&](int n) -> int     { return n * s; };

    uint16_t color = selected ? displayColorSelectFg() : displayColorFg();
    uint16_t bg    = selected ? displayColorSelectBg() : displayColorBg();

    auto tline = [&](int u0, int v0, int u1, int v1) {
        if (u0 == u1) {
            for (int i = 0; i < s; i++) tft.drawLine(X(u0) + i, Y(v0), X(u1) + i, Y(v1), color);
        } else if (v0 == v1) {
            for (int i = 0; i < s; i++) tft.drawLine(X(u0), Y(v0) + i, X(u1), Y(v1) + i, color);
        } else {
            for (int i = 0; i < s; i++) tft.drawLine(X(u0) + i, Y(v0), X(u1) + i, Y(v1), color);
        }
    };
    auto tcircle = [&](int cu, int cv, int ru) {
        for (int i = 0; i < s; i++) tft.drawCircle(X(cu), Y(cv), U(ru) + i, color);
    };

    if (action == "Power" || action == "Power_on" || action == "Power_off" || action == "Off") {
        tcircle(9, 11, 7);
        tft.fillRect(X(6), Y(2), U(7), U(4), bg);
        tline(9, 2, 9, 11);
        return;
    }
    if (action == "Mute") {
        tft.fillRect(X(1), Y(7), U(4), U(6), color);
        tft.fillTriangle(X(5), Y(7), X(10), Y(3), X(10), Y(17), color);
        tline(13, 6, 19, 14);
        tline(19, 6, 13, 14);
        return;
    }
    if (action == "Vol_up") {
        tft.fillRect(X(1), Y(7), U(4), U(6), color);
        tft.fillTriangle(X(5), Y(7), X(10), Y(3), X(10), Y(17), color);
        tline(13, 7, 16, 10);
        tline(16, 10, 13, 13);
        tline(16, 4, 20, 10);
        tline(20, 10, 16, 16);
        return;
    }
    if (action == "Vol_dn") {
        tft.fillRect(X(1), Y(7), U(4), U(6), color);
        tft.fillTriangle(X(5), Y(7), X(10), Y(3), X(10), Y(17), color);
        tline(13, 7, 16, 10);
        tline(16, 10, 13, 13);
        return;
    }
    if (action == "Speed_up" || action == "Brightness_up") {
        tline(10, 3, 10, 17);
        tline(3, 10, 17, 10);
        return;
    }
    if (action == "Speed_dn" || action == "Brightness_dn") {
        tline(3, 10, 17, 10);
        return;
    }
    if (action == "Ch_next") {
        tft.fillTriangle(X(3), Y(12), X(10), Y(5),  X(17), Y(12), color);
        tft.fillTriangle(X(3), Y(17), X(10), Y(10), X(17), Y(17), color);
        return;
    }
    if (action == "Ch_prev") {
        tft.fillTriangle(X(3), Y(3),  X(10), Y(10), X(17), Y(3),  color);
        tft.fillTriangle(X(3), Y(8),  X(10), Y(15), X(17), Y(8),  color);
        return;
    }
    if (action == "Play") {
        tft.fillTriangle(X(4), Y(3), X(16), Y(10), X(4), Y(17), color);
        return;
    }
    if (action == "Pause") {
        tft.fillRect(X(4),  Y(3), U(4), U(14), color);
        tft.fillRect(X(12), Y(3), U(4), U(14), color);
        return;
    }
    if (action == "Next") {
        tft.fillTriangle(X(2), Y(3), X(13), Y(10), X(2), Y(17), color);
        tft.fillRect(X(14), Y(3), U(3), U(14), color);
        return;
    }
    if (action == "Prev") {
        tft.fillTriangle(X(18), Y(3), X(7), Y(10), X(18), Y(17), color);
        tft.fillRect(X(3), Y(3), U(3), U(14), color);
        return;
    }
    if (action == "Red" || action == "Green" || action == "Blue" || action == "White") {
        uint16_t fill = (action == "Red")   ? TFT_RED    :
                        (action == "Green") ? TFT_GREEN  :
                        (action == "Blue")  ? TFT_BLUE   : TFT_WHITE;
        tft.fillCircle(X(10), Y(10), U(7), fill);
        tcircle(10, 10, 8);
        return;
    }
    if (action == "Mode") {
        tcircle(10, 10, 5);
        tline(10, 1,  10, 3);
        tline(10, 17, 10, 19);
        tline(1,  10, 3,  10);
        tline(17, 10, 19, 10);
        return;
    }
    if (action == "Rotate") {
        tcircle(10, 10, 7);
        tft.fillTriangle(X(15), Y(3), X(19), Y(6), X(13), Y(8), color);
        return;
    }
    if (action == "Timer") {
        tcircle(10, 10, 8);
        tline(10, 10, 10, 4);
        tline(10, 10, 15, 12);
        return;
    }
    if (action == "Dh") {
        tft.fillTriangle(X(10), Y(2), X(4), Y(13), X(16), Y(13), color);
        tft.fillCircle(X(10), Y(13), U(5), color);
        return;
    }
    if (action == "Cool_hi" || action == "Heat_hi") {
        bool isCool = (action == "Cool_hi");

        // ---- Up-pointing pentagon outline ----
        tline(10, 1, 18, 5);     // right slope (tip to right shoulder)
        tline(18, 5, 18, 19);    // right vertical side
        tline(18, 19, 2, 19);    // bottom edge
        tline(2, 19, 2, 5);      // left vertical side
        tline(2, 5, 10, 1);      // left slope (shoulder to tip)

        // ---- "MAX" text inside the top tip ----
        tft.setTextSize(1);
        tft.setTextColor(color, bg);
        {
            const char* t = "MAX";
            int w = tft.textWidth(t);
            tft.setCursor(X(10) - w / 2, Y(3) + 1);
            tft.print(t);
        }

        // ---- Center icon: snowflake (COOL) or sun (HEAT) ----
        if (isCool) {
            // Snowflake: three crossing lines through the center
            int cxu = 10, cyu = 11, r = 3;
            tline(cxu, cyu - r, cxu, cyu + r);
            tline(cxu - r, cyu, cxu + r, cyu);
            tline(cxu - r + 1, cyu - r + 1, cxu + r - 1, cyu + r - 1);
            tline(cxu - r + 1, cyu + r - 1, cxu + r - 1, cyu - r + 1);
        } else {
            // Sun: outline circle with 8 rays
            tcircle(10, 11, 2);
            for (int a = 0; a < 8; a++) {
                float r = a * 3.14159265f / 4.0f;
                int x1 = 10 + (int)(cosf(r) * 4.0f);
                int y1 = 11 + (int)(sinf(r) * 4.0f);
                int x2 = 10 + (int)(cosf(r) * 5.0f);
                int y2 = 11 + (int)(sinf(r) * 5.0f);
                tline(x1, y1, x2, y2);
            }
        }

        // ---- Temperature readout at the bottom ----
        {
            const char* t = "23";       // placeholder; replace with live value if desired
            int w = tft.textWidth(t);
            tft.setCursor(X(10) - w / 2, Y(15) + 1);
            tft.print(t);
        }
        return;
    }

    if (action == "Cool_lo" || action == "Heat_lo") {
        bool isCool = (action == "Cool_lo");

        // ---- Down-pointing pentagon outline ----
        tline(10, 19, 18, 15);   // right slope (tip to right shoulder)
        tline(18, 15, 18, 1);    // right vertical side
        tline(18, 1, 2, 1);      // top edge
        tline(2, 1, 2, 15);      // left vertical side
        tline(2, 15, 10, 19);    // left slope (shoulder to tip)

        // ---- Temperature readout at the top ----
        tft.setTextSize(1);
        tft.setTextColor(color, bg);
        {
            const char* t = "23";
            int w = tft.textWidth(t);
            tft.setCursor(X(10) - w / 2, Y(3) + 1);
            tft.print(t);
        }

        // ---- Center icon: snowflake (COOL) or sun (HEAT) ----
        if (isCool) {
            int cxu = 10, cyu = 9, r = 3;
            tline(cxu, cyu - r, cxu, cyu + r);
            tline(cxu - r, cyu, cxu + r, cyu);
            tline(cxu - r + 1, cyu - r + 1, cxu + r - 1, cyu + r - 1);
            tline(cxu - r + 1, cyu + r - 1, cxu + r - 1, cyu - r + 1);
        } else {
            tcircle(10, 9, 2);
            for (int a = 0; a < 8; a++) {
                float r = a * 3.14159265f / 4.0f;
                int x1 = 10 + (int)(cosf(r) * 4.0f);
                int y1 = 9 + (int)(sinf(r) * 4.0f);
                int x2 = 10 + (int)(cosf(r) * 5.0f);
                int y2 = 9 + (int)(sinf(r) * 5.0f);
                tline(x1, y1, x2, y2);
            }
        }

        // ---- "MIN" text inside the bottom tip ----
        {
            const char* t = "MIN";
            int w = tft.textWidth(t);
            tft.setCursor(X(10) - w / 2, Y(15) + 1);
            tft.print(t);
        }
        return;
    }

    // Fallback placeholder for any unmatched action
    tft.drawRoundRect(X(2), Y(2), U(15), U(15), 2, color);
}
// ====================================================================
//  Draw the universal remote screen (centered, landscape)
// ====================================================================

static void chooseGridShape(int count, int availW, int availH,
                            int& outCols, int& outRows,
                            int preferredCols = -1) {
    const int MAX_COLS = 6;
    const int MAX_ROWS = 5;

    if (count < 1) count = 1;

    // ---- Manual override (from universalPreferredCols) ----
    if (preferredCols >= 1 && preferredCols <= MAX_COLS) {
        outCols = preferredCols;
        outRows = (count + preferredCols - 1) / preferredCols;
        if (outRows < 1) outRows = 1;
        if (outRows > MAX_ROWS) outRows = MAX_ROWS;
        return;
    }

    // ---- Auto-pick ----
    float targetAspect = (float)availW / (float)availH;
    if (targetAspect < 0.01f) targetAspect = 1.0f;

    int bestCols = 1, bestRows = count;
    float bestScore = 1e9f;

    for (int r = 1; r <= MAX_ROWS && r <= count; r++) {
        int c = (count + r - 1) / r;
        if (c > MAX_COLS) continue;
        if (c * r < count) continue;

        float gridAspect = (float)c / (float)r;
        float score = fabsf(gridAspect - targetAspect);
        score += (float)(c * r - count) * 1.0f;

        if (score < bestScore) {
            bestScore = score;
            bestCols = c;
            bestRows = r;
        }
    }

    outCols = bestCols;
    outRows = bestRows;
}

static void drawUniversalRemoteScreen() {
    tft.fillScreen(displayColorBg());

    const int screenW = tft.width();
    const int screenH = tft.height();

    displayDrawHeaderBar(universalPanelTitles[universalCategory]);
    const int headerH = displayHeaderHeight();

    byte count = universalActionCounts[universalCategory];

    // ---- Available region for the grid ----
    int availTop    = headerH + 4;
    int availBottom = screenH - FOOTER_HEIGHT;
    int availW      = screenW - 2 * MARGIN;
    int availH      = availBottom - availTop;

    // ---- Pick the grid shape (cols x rows) for this button count ----
    chooseGridShape(count, availW, availH, usedCols, usedRows,
                    universalPreferredCols[universalCategory]);

    // ---- Compute the largest square cell that fits that grid ----
    int cellFromW = (availW - (usedCols - 1) * GAP) / usedCols;
    int cellFromH = (availH - (usedRows - 1) * GAP) / usedRows;
    CELL_W = CELL_H = min(cellFromW, cellFromH);
    if (CELL_W > CELL_MAX) CELL_W = CELL_H = CELL_MAX;
    if (CELL_W < CELL_MIN) CELL_W = CELL_H = CELL_MIN;

    // ---- Center the grid ----
    int gridWidth  = usedCols * CELL_W + (usedCols - 1) * GAP;
    int gridHeight = usedRows * CELL_H + (usedRows - 1) * GAP;

    gridLeft = (screenW - gridWidth) / 2;
    gridTop  = availTop + (availH - gridHeight) / 2;

    if (gridLeft < 0) gridLeft = 0;
    if (gridTop < availTop) gridTop = availTop;

    // ---- Draw the buttons ----
    for (byte i = 0; i < count; ++i) {
        int x, y;
        getButtonPosition(i, x, y);
        bool selected = (i == universalSelectedAction);

        // Cell chrome
        tft.fillRoundRect(x, y, CELL_W, CELL_H, 5,
                          selected ? displayColorSelectBg() : displayColorBg());
        tft.drawRoundRect(x, y, CELL_W, CELL_H, 5,
                          selected ? displayColorSelectFg() : displayColorFgDim());
        if (selected) {
            tft.drawRoundRect(x + 2, y + 2, CELL_W - 4, CELL_H - 4, 4,
                              displayColorSelectFg());
        }

        // Reserve a strip at the bottom for a label if this button has one.
        const char* label = universalLabels[universalCategory][i];
        bool hasLabel = (label && label[0] != '\0');
        const int LABEL_H = hasLabel ? 14 : 0;
        int iconH = CELL_H - LABEL_H;

        drawUniversalGlyph(universalActions[universalCategory][i],
                           x, y, CELL_W, iconH, selected);

        if (hasLabel) {
            tft.setTextSize(1);
            tft.setTextColor(selected ? displayColorSelectFg() : displayColorFgDim(),
                             selected ? displayColorSelectBg() : displayColorBg());
            int lw = tft.textWidth(label);
            // Auto-shrink long labels like CHANNEL
            if (lw > CELL_W - 4) {
                String s(label);
                int maxChars = (CELL_W - 4) / 6;
                if (maxChars < 3) maxChars = 3;
                if ((int)s.length() > maxChars) s = s.substring(0, maxChars - 2) + "..";
                lw = tft.textWidth(s);
                int lx = x + (CELL_W - lw) / 2;
                int ly = y + iconH + (LABEL_H - 8) / 2;
                tft.setCursor(lx, ly);
                tft.print(s);
            } else {
                int lx = x + (CELL_W - lw) / 2;
                int ly = y + iconH + (LABEL_H - 8) / 2;
                tft.setCursor(lx, ly);
                tft.print(label);
            }
        }
    }   // <-- only ONE closing brace here now

    // ---- Footer hint (vertically centered in the footer strip) ----
    tft.setTextSize(1);
    int fy = screenH - FOOTER_HEIGHT + (FOOTER_HEIGHT - 8) / 2;
    int fx = MARGIN;
    tft.setTextColor(displayColorFgDim(), displayColorBg());
    tft.setCursor(fx, fy);
    tft.print("Arrows=move  ");
    fx += tft.textWidth("Arrows=move  ");
    tft.setTextColor(UI_ACCENT_GREEN, displayColorBg());
    tft.setCursor(fx, fy);
    tft.print("OK=Send  ");
    fx += tft.textWidth("OK=Send  ");
    tft.setTextColor(UI_ACCENT_RED, displayColorBg());
    tft.setCursor(fx, fy);
    tft.print("BACK=Exit");
}

// Draw the sending progress popup (overlay)
static void drawUniversalSendingScreen(int progress) {
    drawUniversalRemoteScreen();

    const int screenW = tft.width();
    const int screenH = tft.height();
    const uint16_t accent = universalLoadError ? UI_ACCENT_RED
                           : universalPaused    ? UI_ACCENT_AMBER
                                                 : UI_ACCENT_GREEN;

    // Overlay a popup sized/centered relative to the real screen instead of
    // a fixed 88x44 box positioned for a 128x64 display (which used to end
    // up off-center, or clipped, on larger/smaller panels).
    int popW = min(screenW - 16, 140);
    int popH = min(screenH - 16, 56);
    int popX = (screenW - popW) / 2;
    int popY = (screenH - popH) / 2;

    tft.fillRoundRect(popX, popY, popW, popH, 6, displayColorBg());
    tft.drawRoundRect(popX, popY, popW, popH, 6, accent);

    tft.setTextColor(accent, displayColorBg());
    tft.setTextSize(1);
    tft.setCursor(popX + 6, popY + 8);
    tft.print(universalLoadError ? "Error" : universalPaused ? "Paused" : "Sending...");

    // Progress bar, filled in the same accent color as the card border.
    int barX = popX + 8, barY = popY + 22, barW = popW - 16, barH = 10;
    tft.drawRect(barX, barY, barW, barH, displayColorFgDim());
    int fillW = map(progress, 0, 100, 0, barW - 2);
    if (fillW > 0) tft.fillRect(barX + 1, barY + 1, fillW, barH - 2, accent);

    // Progress text
    char buf[16];
    if (universalLoadError) snprintf(buf, sizeof(buf), "Error");
    else if (universalSignalCount == 0) snprintf(buf, sizeof(buf), "Loading...");
    else {
        uint16_t sent = min(universalSignalPosition, universalSignalCount);
        snprintf(buf, sizeof(buf), "%d/%d", sent, universalSignalCount);
    }
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(popX + 6, popY + 40);
    tft.print(buf);
}

// Collect all signal offsets for a given action
static bool collectUniversalSignalIndices(const String& action) {
    universalSignalCount = 0;
    universalSignalPosition = 0;
    universalLoadError = false;
    universalLoadCanceled = false;

    String path = "/infrared/assets/" + currentUniversalFile;
    File file = SD.open(path, FILE_READ);
    if (!file) {
        universalLoadError = true;
        return false;
    }

    int signalIndex = -1;
    while (file.available()) {
        uint32_t pos = file.position();
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("name:")) {
            signalIndex++;
            String name = line.substring(5);
            name.trim();
            if (name == action && universalSignalCount < MAX_UNIVERSAL_SIGNAL_INDICES) {
                universalSignalOffsets[universalSignalCount++] = pos;
            }
        }
    }
    file.close();
    return universalSignalCount > 0;
}

// Send one IR signal from the file at a specific offset
static bool sendUniversalSignal(const String& fileName, int signalIdx, uint32_t offset) {
    String path = "/infrared/assets/" + fileName;
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    if (!f.seek(offset)) {
        f.close();
        return false;
    }

    IrFileSignal sig;
    bool haveSignal = false;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.startsWith("name:")) {
            if (haveSignal) break;
            haveSignal = true;
            sig.name = line.substring(5);
            sig.name.trim();
        } else if (haveSignal) {
            if (line.startsWith("type:")) {
                String t = line.substring(5); t.trim();
                sig.isRaw = (t == "raw");
            } else if (line.startsWith("frequency:")) {
                uint32_t hz = (uint32_t)line.substring(10).toInt();
                sig.freqKhz = (uint16_t)(hz / 1000);
                if (sig.freqKhz == 0) sig.freqKhz = kIrKhz;
            } else if (line.startsWith("data:") && sig.isRaw) {
                String dataLine = line.substring(5); dataLine.trim();
                int start = 0;
                while (start < (int)dataLine.length() && sig.raw.size() < kMaxRawLen) {
                    int sp = dataLine.indexOf(' ', start);
                    String tok = (sp < 0) ? dataLine.substring(start) : dataLine.substring(start, sp);
                    if (tok.length() > 0) sig.raw.push_back((uint16_t)tok.toInt());
                    if (sp < 0) break;
                    start = sp + 1;
                }
            } else if (line.startsWith("protocol:")) {
                sig.protocol = line.substring(9); sig.protocol.trim();
            } else if (line.startsWith("address:")) {
                sig.address = irParseLeHexBytes(line.substring(8));
            } else if (line.startsWith("command:")) {
                sig.command = irParseLeHexBytes(line.substring(8));
            } else if (line.startsWith("value:")) {
                sig.value = parseHexBytesToUint64LE(line.substring(6));
            } else if (line.startsWith("bits:")) {
                sig.bits = (uint16_t)line.substring(5).toInt();
            }
        }
    }
    f.close();

    if (!haveSignal || (sig.isRaw && sig.raw.empty()) || (!sig.isRaw && sig.protocol.length() == 0))
        return false;

    String err;
    return irSendSignal(sig, err);
}

// ====================================================================
//  Universal Remote menu and event handling
// ====================================================================

void irEnterUniversalCategory() {
    std::vector<String> items;
    int count = sizeof(universalCategories) / sizeof(universalCategories[0]);
    for (int i = 0; i < count; i++) items.push_back(String(universalCategories[i]));
    static SimpleMenu catMenu(items);
    catMenu.setItems(items);
    catMenu.setIndex(universalCatIndex);
    displayShowMenu("Universal Remote", catMenu.items(), catMenu.index());
}

// True any time the Universal Remote feature (category list OR the action
// screen for a chosen device) is on screen. The outer input dispatcher
// should route events to irHandleUniversalEvent() while this is true, and
// return to its own parent menu as soon as it goes false.
bool irIsUniversalRemoteActive() {
    return universalMenuActive;
}

bool irUniversalOnActionScreen() {
    return universalMenuActive && universalActive;
}

void irHandleUniversalEvent(int evt) {
    if (evt == EVT_NONE) return;

    // Sending state
    if (irUniversalSending) {
        if (evt == EVT_OK) {
            universalPaused = !universalPaused;
            int progress = map(universalSignalPosition, 0, universalSignalCount, 0, 100);
            drawUniversalSendingScreen(progress);
            return;
        }
        if (evt == EVT_BACK) {
            irUniversalSending = false;
            universalPaused = false;
            universalSignalCount = 0;
            universalSignalPosition = 0;
            drawUniversalRemoteScreen();
            return;
        }
        if (universalPaused) {
            if (evt == EVT_UP && universalSignalPosition > 0) {
                universalSignalPosition--;
                drawUniversalSendingScreen(map(universalSignalPosition, 0, universalSignalCount, 0, 100));
                return;
            }
            if (evt == EVT_DOWN && universalSignalPosition < universalSignalCount - 1) {
                universalSignalPosition++;
                drawUniversalSendingScreen(map(universalSignalPosition, 0, universalSignalCount, 0, 100));
                return;
            }
        }
        return;
    }

    // Not sending: category or action selection.
    // universalActive (persistent module state, NOT a function-local static)
    // tells us which of the two screens is showing. Using a real state
    // variable that irEnterUniversalRemote() resets on every entry, instead
    // of a static local that silently kept its value between sessions, is
    // what fixes "select a device and the remote screen doesn't appear" —
    // that used to happen whenever this flag was left true from a previous
    // visit, so the code thought it was already on the remote screen while
    // the category list was what was actually drawn.
    if (!universalActive) {
        // Category selection
        int count = sizeof(universalCategories) / sizeof(universalCategories[0]);
        if (evt == EVT_UP) {
            universalCatIndex = (universalCatIndex - 1 + count) % count;
            std::vector<String> items;
            for (int i = 0; i < count; i++) items.push_back(String(universalCategories[i]));
            displayShowMenu("Universal Remote", items, universalCatIndex);
        } else if (evt == EVT_DOWN) {
            universalCatIndex = (universalCatIndex + 1) % count;
            std::vector<String> items;
            for (int i = 0; i < count; i++) items.push_back(String(universalCategories[i]));
            displayShowMenu("Universal Remote", items, universalCatIndex);
        } else if (evt == EVT_OK) {
            universalCategory = universalCatIndex;
            currentUniversalFile = String(universalFiles[universalCategory]);
            universalSelectedAction = 0;
            universalActive = true;
            // Brief "loading" feedback so device -> remote reads as a real
            // transition, the way selecting a remote does on a Flipper.
            displayShowMessage("Universal", (String(universalPanelTitles[universalCategory]) + "...").c_str());
            delay(150);
            drawUniversalRemoteScreen();
        } else if (evt == EVT_BACK) {
            // Exiting the category list exits the whole feature. This case
            // was missing entirely before, so backing out of the category
            // list had no defined behavior here and depended entirely on
            // code outside this file to notice and clean up.
            universalMenuActive = false;
            universalRestoreRotation();
        }
    } else {
        // Action selection (remote screen)
        byte count = universalActionCounts[universalCategory];
        int cols = usedCols; // use the actual number of columns used for this category
        int rows = (count + cols - 1) / cols;
        int currentCol = universalSelectedAction % cols;
        int currentRow = universalSelectedAction / cols;

        if (evt == EVT_UP) {
            if (currentRow > 0) {
                int newIdx = (currentRow - 1) * cols + currentCol;
                if (newIdx < count) universalSelectedAction = newIdx;
            }
            drawUniversalRemoteScreen();
        } else if (evt == EVT_DOWN) {
            if (currentRow < rows - 1) {
                int newIdx = (currentRow + 1) * cols + currentCol;
                if (newIdx < count) universalSelectedAction = newIdx;
            }
            drawUniversalRemoteScreen();
        } else if (evt == EVT_LEFT) {
            if (currentCol > 0) {
                int newIdx = currentRow * cols + (currentCol - 1);
                if (newIdx < count) universalSelectedAction = newIdx;
            }
            drawUniversalRemoteScreen();
        } else if (evt == EVT_RIGHT) {
            if (currentCol < cols - 1) {
                int newIdx = currentRow * cols + (currentCol + 1);
                if (newIdx < count) universalSelectedAction = newIdx;
            }
            drawUniversalRemoteScreen();
        } else if (evt == EVT_OK) {
            String action = universalActions[universalCategory][universalSelectedAction];
            if (collectUniversalSignalIndices(action)) {
                universalPaused = false;
                universalLoadError = false;
                universalSignalPosition = 0;
                irUniversalSending = true;
                irUniversalAction = action;
                irUniversalLastSend = 0;
                drawUniversalSendingScreen(0);
            } else {
                if (universalLoadCanceled) {
                    universalLoadCanceled = false;
                    drawUniversalRemoteScreen();
                } else {
                    displayShowMessage("Universal", "No signals found");
                    delay(500);
                    drawUniversalRemoteScreen();
                }
            }
        } else if (evt == EVT_BACK) {
            universalActive = false;
            irEnterUniversalCategory();
        }
    }
}

// Called from main loop to process sending
void irUniversalSendingLoop() {
    if (!irUniversalSending) return;

    if (universalPaused) return;

    if (universalSignalPosition >= universalSignalCount) {
        irUniversalSending = false;
        universalPaused = false;
        universalSignalCount = 0;
        universalSignalPosition = 0;
        drawUniversalRemoteScreen();
        return;
    }

    unsigned long now = millis();
    if (now - irUniversalLastSend >= UNIVERSAL_SEND_DELAY_MS) {
        irUniversalLastSend = now;
        bool sent = sendUniversalSignal(currentUniversalFile, 0, universalSignalOffsets[universalSignalPosition]);
        if (sent) {
            universalSignalPosition++;
            int progress = map(universalSignalPosition, 0, universalSignalCount, 0, 100);
            drawUniversalSendingScreen(progress);
        } else {
            universalSignalPosition++; // skip unsupported
        }
    }
}

// ====================================================================
//  Public entry for Universal Remote (called from main menu)
// ====================================================================
void irEnterUniversalRemote() {
    irInit(); 
    universalMenuActive = true;
    universalActive = false;      // start on the category list, not a remote
    universalCatIndex = 0;
    universalSelectedAction = 0;
    irUniversalSending = false;
    universalPaused = false;
    universalSignalCount = 0;
    universalSignalPosition = 0;
    universalLoadError = false;
    universalLoadCanceled = false;
    universalForceLandscape();
    irEnterUniversalCategory();
}

// ====================================================================
//  IR Play File (custom .ir files from SD)
// ====================================================================
void irEnterPlayFile() {
    irInit();
    g_irPickerMode = true;
    enterState(STATE_IR_PLAY_PICKER);
}


static const char* TVBG_PATH = "/tvbgone.bin";

static int      tvbRegion   = 0;
static int      tvbIndex    = 0;
static bool     tvbRunning  = false;
static uint32_t tvbLastSend = 0;
static constexpr uint32_t kTvbIntervalMs = 200;
static constexpr const char* kTvbRegionNames[2] = {"NA", "EU"};
static constexpr int kTvbMaxPairs = 160;

// File handle kept open while the TV-B-Gone screen is active
static File     tvbgFile;
static bool     tvbgFileOpen = false;

// Region counts and file offsets (region 0 = NA, 1 = EU)
static uint16_t tvbgRegionCodeCount[2] = {0, 0};
static uint32_t tvbgRegionStart[2]     = {0, 0};

// Scratch buffers for one code at a time
static uint16_t tvbgTimesBuf[64];
static uint8_t  tvbgCodesBuf[128];
static uint16_t tvbgRawBuf[kTvbMaxPairs * 2];

static bool tvbgOpenFile() {
    if (tvbgFileOpen) return true;
    if (!SD.exists(TVBG_PATH)) {
        Serial.printf("[TVBG] %s not found\n", TVBG_PATH);
        return false;
    }
    tvbgFile = SD.open(TVBG_PATH, FILE_READ);
    if (!tvbgFile) {
        Serial.println("[TVBG] open failed");
        return false;
    }

    char magic[4];
    if (tvbgFile.read((uint8_t*)magic, 4) != 4 ||
        memcmp(magic, "TVBG", 4) != 0) {
        Serial.println("[TVBG] bad magic");
        tvbgFile.close();
        return false;
    }
    uint8_t version = 0;
    tvbgFile.read(&version, 1);
    if (version != 1) {
        Serial.printf("[TVBG] unsupported version %d\n", version);
        tvbgFile.close();
        return false;
    }

    tvbgFile.seek(8);
    uint8_t regionCount = 0;
    tvbgFile.read(&regionCount, 1);
    if (regionCount < 2) {
        tvbgFile.close();
        return false;
    }
    tvbgFile.seek(12);

    for (int r = 0; r < 2; r++) {
        uint16_t count = 0;
        tvbgFile.read((uint8_t*)&count, 2);
        tvbgFile.seek(tvbgFile.position() + 2);
        tvbgRegionCodeCount[r] = count;
        tvbgRegionStart[r]     = tvbgFile.position();

        for (uint16_t i = 0; i < count; i++) {
            uint32_t tv; uint8_t p, b;
            tvbgFile.read((uint8_t*)&tv, 4);
            tvbgFile.read(&p, 1);
            tvbgFile.read(&b, 1);
            tvbgFile.seek(tvbgFile.position() + 2);
            uint16_t timesCount = 2 * (1 << b);
            uint16_t codesCount = (p * b + 7) / 8;
            tvbgFile.seek(tvbgFile.position() + timesCount * 2 + codesCount);
        }
    }

    tvbgFileOpen = true;
    Serial.printf("[TVBG] loaded: NA=%d EU=%d\n",
                  tvbgRegionCodeCount[0], tvbgRegionCodeCount[1]);
    return true;
}

static void tvbgCloseFile() {
    if (tvbgFileOpen) {
        tvbgFile.close();
        tvbgFileOpen = false;
    }
}

static bool tvbgReadCode(int region, int idx, uint32_t& timerVal,
                          uint8_t& pairs, uint8_t& bpi) {
    if (region < 0 || region > 1) return false;
    if (idx < 0 || idx >= tvbgRegionCodeCount[region]) return false;

    tvbgFile.seek(tvbgRegionStart[region]);
    for (int i = 0; i < idx; i++) {
        uint32_t tv; uint8_t p, b;
        tvbgFile.read((uint8_t*)&tv, 4);
        tvbgFile.read(&p, 1);
        tvbgFile.read(&b, 1);
        tvbgFile.seek(tvbgFile.position() + 2);
        uint16_t tc = 2 * (1 << b);
        uint16_t cc = (p * b + 7) / 8;
        tvbgFile.seek(tvbgFile.position() + tc * 2 + cc);
    }

    tvbgFile.read((uint8_t*)&timerVal, 4);
    tvbgFile.read(&pairs, 1);
    tvbgFile.read(&bpi, 1);
    tvbgFile.seek(tvbgFile.position() + 2);

    uint16_t timesCount = 2 * (1 << bpi);
    uint16_t codesCount = (pairs * bpi + 7) / 8;
    if (timesCount > sizeof(tvbgTimesBuf) / 2 ||
        codesCount > sizeof(tvbgCodesBuf)) {
        Serial.println("[TVBG] code exceeds buffers");
        return false;
    }
    tvbgFile.read((uint8_t*)tvbgTimesBuf, timesCount * 2);
    tvbgFile.read(tvbgCodesBuf, codesCount);
    return true;
}

static void tvbgSendBuffered(uint32_t timerVal, uint8_t pairs, uint8_t bpi) {
    int rawIdx = 0;
    uint8_t bitPos = 0, byteIdx = 0;

    if (pairs > kTvbMaxPairs) pairs = kTvbMaxPairs;

    for (int i = 0; i < pairs; i++) {
        uint8_t index = 0;
        for (int b = 0; b < bpi; b++) {
            uint8_t bit = (tvbgCodesBuf[byteIdx] >> (7 - bitPos)) & 1;
            index = (index << 1) | bit;
            bitPos++;
            if (bitPos >= 8) { bitPos = 0; byteIdx++; }
        }
        if (rawIdx + 1 >= kTvbMaxPairs * 2) break;
        tvbgRawBuf[rawIdx++] = tvbgTimesBuf[index * 2];
        tvbgRawBuf[rawIdx++] = tvbgTimesBuf[index * 2 + 1];
    }

    uint16_t freq = (uint16_t)timerVal;
    if (freq < 1000) freq = 38000;
    irsend.sendRaw(tvbgRawBuf, rawIdx, freq);
}

static void tvbSendCurrent() {
    if (!tvbgFileOpen) return;
    int total = tvbgRegionCodeCount[tvbRegion];
    if (total == 0) return;
    if (tvbIndex >= total) tvbIndex = 0;

    uint32_t timerVal; uint8_t pairs, bpi;
    if (!tvbgReadCode(tvbRegion, tvbIndex, timerVal, pairs, bpi)) return;
    tvbgSendBuffered(timerVal, pairs, bpi);
}

static void tvbDraw() {
    int total = tvbgRegionCodeCount[tvbRegion];
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("TV-B-Gone");

    const int margin = 10;
    const int headerH = displayHeaderHeight();
    const int w = tft.width() - 2 * margin;
    const uint16_t accent = tvbRunning ? TFT_GREEN : TFT_YELLOW;

    int cardY = headerH + 8;
    int cardH = 54;
    tft.drawRoundRect(margin, cardY, w, cardH, 6, accent);

    String pillTxt = tvbRunning ? "SENDING" : "PAUSED";
    int pillW = tft.textWidth(pillTxt) + 10;
    tft.fillRoundRect(margin + w - pillW - 8, cardY + 6, pillW, 14, 7, accent);
    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(margin + w - pillW - 3, cardY + 9);
    tft.print(pillTxt);

    tft.setTextSize(2);
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setCursor(margin + 8, cardY + 8);
    tft.print(String("Region: ") + kTvbRegionNames[tvbRegion]);

    tft.setTextColor(accent, displayColorBg());
    tft.setCursor(margin + 8, cardY + 30);
    tft.print(tvbRunning ? "Sending..." : "Paused");

    int y = cardY + cardH + 14;

    if (total > 0 && tvbIndex >= 0 && tvbIndex < total) {
        tft.setTextColor(displayColorFg(), displayColorBg());
        tft.setTextSize(1);
        tft.setCursor(10, y);
        tft.print(String(tvbIndex + 1) + "/" + String(total));
        y += 22;

        int barX = 10, barY = y;
        int barW = tft.width() - 20, barH = 16;
        tft.drawRect(barX, barY, barW, barH, displayColorFgDim());
        int fillW = (tvbIndex + 1) * barW / total;
        if (fillW > barW) fillW = barW;
        tft.fillRect(barX + 1, barY + 1, fillW - 2, barH - 2, accent);
    }

    displayDrawFooterBar("LEFT/RIGHT=Region  OK=Start/Pause  BACK=Exit");
}

void irEnterTvBGone() {
    irInit();

    if (!tvbgOpenFile()) {
        displayShowMessage("TV-B-Gone",
            "Missing /tvbgone.bin\n\nRun tools/tvbgconv.py on\n"
            "src/tvbgcodes.h and copy\nits output to the SD card\nroot.");
        delay(3000);
        enterState(STATE_INFRARED_MENU);
        return;
    }

    tvbIndex    = 0;
    tvbRunning  = true;
    tvbLastSend = 0;
    tvbDraw();
}

void irHandleTvBGoneEvent(int evt) {
    if (evt == EVT_NONE) return;
    if ((evt == EVT_LEFT || evt == EVT_RIGHT) && !tvbRunning) {
        tvbRegion = 1 - tvbRegion;
        tvbIndex  = 0;
        tvbDraw();
    } else if (evt == EVT_OK) {
        tvbRunning = !tvbRunning;
        tvbDraw();
    }
}

void irTvBGoneLoop() {
    if (!tvbRunning) return;
    int total = tvbgRegionCodeCount[tvbRegion];
    if (total == 0) return;

    uint32_t now = millis();
    if (now - tvbLastSend >= kTvbIntervalMs) {
        tvbLastSend = now;
        tvbSendCurrent();
        tvbIndex++;
        if (tvbIndex >= total) tvbIndex = 0;
        tvbDraw();
    }
}

void irTvBGoneStop() {
    tvbRunning = false;
    tvbgCloseFile();
}

// ====================================================================
//  IR Jammer (unchanged)
// ====================================================================
static bool     jammerRunning  = false;
static uint32_t jammerLastSend = 0;
static constexpr uint32_t kJammerIntervalMs = 15;

static void jammerDraw() {
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("IR Jammer");

    const int margin = 10;
    const int headerH = displayHeaderHeight();
    const int w = tft.width() - 2 * margin;
    const uint16_t accent = jammerRunning ? UI_ACCENT_RED : UI_ACCENT_CYAN;

    // Status card, same visual language as the Read screen's status box,
    // so the two "always-on" style screens read as one consistent app.
    int cardY = headerH + 12;
    int cardH = 54;
    tft.drawRoundRect(margin, cardY, w, cardH, 6, accent);

    String pillTxt = jammerRunning ? "JAMMING" : "STOPPED";
    int pillW = tft.textWidth(pillTxt) + 10;
    tft.fillRoundRect(margin + w - pillW - 8, cardY + 6, pillW, 14, 7, accent);
    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(margin + w - pillW - 3, cardY + 9);
    tft.print(pillTxt);

    tft.setTextSize(2);
    tft.setTextColor(accent, displayColorBg());
    tft.setCursor(margin + 8, cardY + 24);
    tft.print(jammerRunning ? "Jamming..." : "Stopped");

    displayDrawFooterBar("OK=Start/Stop  BACK=Exit");
}

void irEnterJammer() {
    irInit();
    jammerRunning  = false;
    jammerLastSend = 0;
    jammerDraw();
}

void irHandleJammerEvent(int evt) {
    if (evt == EVT_OK) {
        jammerRunning = !jammerRunning;
        jammerDraw();
    }
}

void irJammerLoop() {
    if (!jammerRunning) return;
    uint32_t now = millis();
    if (now - jammerLastSend >= kJammerIntervalMs) {
        jammerLastSend = now;
        uint16_t noise[4];
        for (int i = 0; i < 4; i++) noise[i] = (uint16_t)random(300, 1000);
        irsend.sendRaw(noise, 4, kIrKhz);
    }
}

// ====================================================================
//  File Action Menu (Spam All / Choose Command / Back)
// ====================================================================
void irEnterFileActions() {
    if (g_openedSignals.empty()) return;
    irInFileActions = true;
    irSignalPickerActive = false;
    irSpamRunning = false;
    irSpamIndex = 0;
    irSpamTotalSignals = g_openedSignals.size();
    irFileActionMenu.setIndex(0);
    int lastSlash = currentIrFileName.lastIndexOf('/');
    String baseName = (lastSlash >= 0) ? currentIrFileName.substring(lastSlash + 1) : currentIrFileName;
    irSignalPickerFileLabel = baseName;
    displayShowMenu("IR File Actions", irFileActionMenu.items(), irFileActionMenu.index());
}

void irHandleFileActionsEvent(int evt) {
    if (!irInFileActions) return;
    if (evt == EVT_UP) {
        irFileActionMenu.up();
        displayShowMenu("IR File Actions", irFileActionMenu.items(), irFileActionMenu.index());
    } else if (evt == EVT_DOWN) {
        irFileActionMenu.down();
        displayShowMenu("IR File Actions", irFileActionMenu.items(), irFileActionMenu.index());
    } else if (evt == EVT_OK) {
        switch (irFileActionMenu.index()) {
            case 0:
                irSpamStart();
                break;
            case 1: {
                irInFileActions = false;
                std::vector<String> labels;
                for (size_t i = 0; i < g_openedSignals.size(); i++) {
                    labels.push_back(irOpenedFileSignalLabel(i));
                }
                irEnterSignalPicker(irSignalPickerFileLabel, labels);
                break;
            }
            case 2:
                irExitFileActions();
                break;
        }
    }
}

void irExitFileActions() {
    irInFileActions = false;
    irSignalPickerActive = false;
    enterState(STATE_IR_PLAY_PICKER);
}

bool irIsInFileActions() {
    return irInFileActions;
}

// ====================================================================
//  Spam All implementation
// ====================================================================
void irSpamStart() {
    if (g_openedSignals.empty()) return;
    irSpamRunning = true;
    irSpamIndex = 0;
    irSpamLastSend = 0;
    irSpamTotalSignals = g_openedSignals.size();
    irInFileActions = false;
    tft.fillScreen(displayColorBg());
    displayDrawHeaderBar("IR Spam");
    tft.setTextColor(displayColorFg(), displayColorBg());
    tft.setTextSize(2);
    tft.setCursor(10, displayHeaderHeight() + 20);
    tft.print("Spamming...");
    tft.setTextSize(1);
    tft.setCursor(10, displayHeaderHeight() + 50);
    tft.print("Press OK or BACK to stop");
}

void irSpamStop() {
    irSpamRunning = false;
    irInFileActions = true;
    displayShowMessage("IR Spam", "Stopped");
    delay(600);
    irEnterFileActions();
}

bool irSpamIsRunning() { return irSpamRunning; }

uint32_t irSpamGetCount() { return irSpamIndex; }

uint32_t irSpamGetTotal() { return irSpamTotalSignals; }

void irSpamTick() {
    if (!irSpamRunning || g_openedSignals.empty()) return;
    unsigned long now = millis();
    if (now - irSpamLastSend < IR_SPAM_INTERVAL_MS) return;
    irSpamLastSend = now;

    bool ok = irSendOpenedFileSignal(irSpamIndex);
    if (!ok) {
        String err = irLastSendError();
        if (err.length() > 0) Serial.println("[IR Spam] Error: " + err);
    }

    static unsigned long lastDisplayUpdate = 0;
    if (now - lastDisplayUpdate > 150) {
        lastDisplayUpdate = now;
        tft.fillRect(10, displayHeaderHeight() + 40, tft.width() - 20, 20, displayColorBg());
        tft.setTextColor(displayColorFg(), displayColorBg());
        tft.setTextSize(1);
        tft.setCursor(10, displayHeaderHeight() + 40);
        tft.print("Signal " + String(irSpamIndex + 1) + "/" + String(irSpamTotalSignals));
    }

    irSpamIndex++;
    if (irSpamIndex >= irSpamTotalSignals) {
        irSpamStop();
    }
}

// ====================================================================
//  Signal picker (for multi-signal .ir files)
// ====================================================================
static void drawIrSignalPicker() {
    String title = "IR: " + irSignalPickerFileLabel;
    displayShowMenu(title.c_str(), irSignalMenu.items(), irSignalMenu.index());
}

void irEnterSignalPicker(const String& fileLabel, const std::vector<String>& labels) {
    irSignalPickerFileLabel = fileLabel;
    irSignalMenu.setItems(labels);
    irSignalMenu.setIndex(0);
    irSignalPickerActive = true;
    drawIrSignalPicker();
}

void irHandleSignalPickerEvent(int evt) {
    if (!irSignalPickerActive) return;
    if (evt == EVT_UP)   { irSignalMenu.up();   drawIrSignalPicker(); }
    if (evt == EVT_DOWN) { irSignalMenu.down(); drawIrSignalPicker(); }
    if (evt == EVT_OK) {
        int idx = irSignalMenu.index();
        String label = irOpenedFileSignalLabel(idx);
        bool ok = irSendOpenedFileSignal(idx);
        displayShowMessage("IR", ok ? ("Sent: " + label).c_str() : irLastSendError().c_str());
        delay(500);
        drawIrSignalPicker();
    }
}

void irSignalPickerBack() {
    if (!irSignalPickerActive) return;
    irSignalPickerActive = false;
}

bool irIsSignalPickerActive() {
    return irSignalPickerActive;
}

// Add after the existing jammer functions
void irJammerStart() {
    if (!jammerRunning) {
        jammerRunning = true;
        jammerDraw();
    }
}
void irJammerStop() {
    if (jammerRunning) {
        jammerRunning = false;
        jammerDraw();
    }
}
bool irJammerIsRunning() { return jammerRunning; }