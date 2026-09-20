#include "rf_module.h"
#include "config.h"
#include "led.h"
#include "display.h"
#include "buttons.h"
#include "sdcard.h"
#include "menu.h"
#include "helpers.h"
#include <ELECHOUSE_CC1101_SRC_DRV.h> 
#include <RCSwitch.h>
#include <SD.h>
#include <vector>
#include <cstring>
#include <math.h>

//  Hardware pin definitions (fallback if not defined in config.h)

#ifndef PIN_CC1101_SCK
  #define PIN_CC1101_SCK   TFT_SCLK
#endif
#ifndef PIN_CC1101_MISO
  #define PIN_CC1101_MISO  TFT_MISO
#endif
#ifndef PIN_CC1101_MOSI
  #define PIN_CC1101_MOSI  TFT_MOSI
#endif
#ifndef PIN_CC1101_CS
  #error "PIN_CC1101_CS must be defined in config.h"
#endif
#ifndef PIN_CC1101_GDO0
  #error "PIN_CC1101_GDO0 must be defined in config.h"
#endif

#define CC1101_CS    PIN_CC1101_CS
#define CC1101_GDO0  PIN_CC1101_GDO0
#define CC1101_SCK   PIN_CC1101_SCK
#define CC1101_MISO  PIN_CC1101_MISO
#define CC1101_MOSI  PIN_CC1101_MOSI


//  CC1101 and RCSwitch objects
RCSwitch rcswitch = RCSwitch();


//  Frequency handling
float frequency = 433.92;
const float frequencies[] = {315.0, 433.92, 868.0, 915.0};
const int numFrequencies = sizeof(frequencies) / sizeof(frequencies[0]);
int freqIndex = 1;


//  Data structures (from subghz.cpp)
#define MAX_DATA_LOG 512

struct tpKeyData {
    float frequency;
    uint8_t keyID[8];
    uint8_t type;
    uint16_t bitLength;
    uint16_t codeLenth;
    uint16_t te;
    uint8_t rcProtocolNum;  // exact RCSwitch library protocol # (only meaningful when type == kRcSwitch)
    char preset[8];
    char rawData[1024];
};

enum emKeys {
    kUnknown = 0,
    kP12bt,
    k12bt,
    k24bt,
    k64bt,
    kKeeLoq,
    kANmotors64,
    kPrinceton,
    kRcSwitch,
    kStarLine,
    kCAME,
    kNICE,
    kHOLTEK,
    kLINEAR,
    kChamberlain,   // Chamberlain/LiftMaster (fixed code) - tables already existed for bruteforce
    kAnsonic,       // Ansonic (fixed code) - tables already existed for bruteforce
    kLinearDelta3,  // Linear Delta-3 garage/gate remotes (fixed code, 8-bit)
    kSomfyRTS,      // Somfy Telis/Keytis RTS - rolling code, like KeeLoq/StarLine
    kNiceFlorS,     // Nice FloR-S - rolling code sibling of the fixed-code "Nice Flo"
    kSecPlusV1,     // Security+1.0 (LiftMaster/Chamberlain) - rolling code
    kSecPlusV2,     // Security+2.0 (LiftMaster/Chamberlain) - rolling code
    kFaacSLH,       // FAAC SLH - rolling code (KeeLoq-family with seed)
    kBftMitto,      // BFT Mitto - rolling code (KeeLoq-family with seed)
    kCameAtomo,     // CAME Atomo - rolling code (CAME's rolling-code line)
    kAnMotors,      // AN-Motors - rolling code
    kHcs101,
    kAlutechAT4N,
    kGateTX,
    kDoorHan,
    kSMC5326,
    kHormann,
    kMarantec,
    kKiaHyundai,
    kMegaCode,
    kAprimatic,
    kIronLogic,
    kSommer,
    kMutancode,
    kMHouse,
    kUNILARM,
    kiDo,         
};

// Brute force protocol structures
struct BruteProtocol {
    const int* zero;
    size_t zeroLen;
    const int* one;
    size_t oneLen;
    const int* pilot;
    size_t pilotLen;
    const int* stop;
    size_t stopLen;
};


//  Global state variables (from subghz.cpp)

bool validKeyReceived = false;
bool readRAW = true;
bool autoSave = false;
int signals = 0;
uint64_t lastSavedKey = 0;
tpKeyData keyData1;
bool isJamming = false;

// Frequency hopping (Read screen) - cycles through `frequencies[]` while
// listening so a signal on any of the supported bands gets picked up
// without the user having to manually step through them.
bool rfHopEnabled = false;
unsigned long rfHopLastMs = 0;
const unsigned long RF_HOP_INTERVAL_MS = 300; // dwell time per frequency

// Bruteforce state
const char* bruteTypes[] = {
    "Came", "Nice", "Ansonic", "Holtek", "Chamberlain",
    "Gate TX", "SMC5326", "MegaCode", "UNILARM"
};
const int BRUTE_TYPE_COUNT = sizeof(bruteTypes) / sizeof(bruteTypes[0]);
const float bruteFreqOptions[] = {315.0, 433.92, 868.0, 915.0};
const int bruteBitsArray[] = {
    12,  // Came
    12,  // Nice
    12,  // Ansonic
    12,  // Holtek
    12,  // Chamberlain
    25,  // Gate TX
    25,  // SMC5326
    24,  // MegaCode
    24   // UNILARM
};
const char* bruteFreqLabels[] = {"315.00MHz", "433.92MHz", "868.00MHz", "915.00MHz"};
const int BRUTE_FREQ_COUNT = 4;
const int bruteBits = 12;
int bruteTypeIndex = 0;
int bruteFreqIndex = 1;
int bruteConfigSelection = 0;
uint16_t bruteProgress = 0;
const uint16_t bruteTotal = 4096;
unsigned long bruteLastStep = 0;
bool bruteRunning = false;
bool bruteRfActive = false;
int bruteTxPin = CC1101_GDO0;

// Brute protocols
const int cameZero[] = {-320, 640};
const int cameOne[] = {-640, 320};
const int camePilot[] = {-11520, 320};
const BruteProtocol protoCame = {cameZero, 2, cameOne, 2, camePilot, 2, nullptr, 0};

const int niceZero[] = {-700, 1400};
const int niceOne[] = {-1400, 700};
const int nicePilot[] = {-25200, 700};
const BruteProtocol protoNice = {niceZero, 2, niceOne, 2, nicePilot, 2, nullptr, 0};

const int ansonicZero[] = {-1111, 555};
const int ansonicOne[] = {-555, 1111};
const int ansonicPilot[] = {-19425, 555};
const BruteProtocol protoAnsonic = {ansonicZero, 2, ansonicOne, 2, ansonicPilot, 2, nullptr, 0};

const int holtekZero[] = {-870, 430};
const int holtekOne[] = {-430, 870};
const int holtekPilot[] = {-15480, 430};
const BruteProtocol protoHoltek = {holtekZero, 2, holtekOne, 2, holtekPilot, 2, nullptr, 0};

const int chamberZero[] = {-870, 430};
const int chamberOne[] = {-430, 870};
const int chamberStop[] = {-3000, 1000};
const BruteProtocol protoChamber = {chamberZero, 2, chamberOne, 2, nullptr, 0, chamberStop, 2};
const BruteProtocol* currentBruteProto = nullptr;

// Gate TX (25‑bit, short=400, long=800, pilot=~8000)
static const int gateZero[] = {-400, 800};
static const int gateOne[]  = {-800, 400};
static const int gatePilot[] = {-8000, 400};
static const BruteProtocol protoGateTX = {gateZero, 2, gateOne, 2, gatePilot, 2, nullptr, 0};

// SMC5326 (25‑bit, short=500, long=1000, pilot=~10000)
static const int smcZero[] = {-500, 1000};
static const int smcOne[]  = {-1000, 500};
static const int smcPilot[] = {-10000, 500};
static const BruteProtocol protoSMC5326 = {smcZero, 2, smcOne, 2, smcPilot, 2, nullptr, 0};

// MegaCode (24‑bit Manchester, bit time~500, sync=~8000)
static const int megaZero[] = {-500, 500};
static const int megaOne[]  = {500, -500};
static const int megaSync[] = {-8000, 500};
static const BruteProtocol protoMegaCode = {megaZero, 2, megaOne, 2, megaSync, 2, nullptr, 0};

// UNILARM (24‑bit, short=400, long=800, pilot=~8000)
static const int unilarmZero[] = {-400, 800};
static const int unilarmOne[]  = {-800, 400};
static const int unilarmPilot[] = {-8000, 400};
static const BruteProtocol protoUNILARM = {unilarmZero, 2, unilarmOne, 2, unilarmPilot, 2, nullptr, 0};

const int linearZero[] = {2000, -2000};
const int linearOne[]  = {500, -3500};
const int linearPilot[] = {-35000};
const BruteProtocol protoLinearDelta3 = {linearZero, 2, linearOne, 2, linearPilot, 1, nullptr, 0};

// Analyzer state
const float analyzerFreqs[] = {
    300.0, 302.75, 303.0, 303.87, 303.90, 304.25,
    307.0, 307.50, 307.80, 309.0, 310.0,
    312.0, 312.1, 312.2, 313.0, 313.85, 314.0, 314.35, 314.98, 315.0,
    318.0, 320.0, 320.15,
    330.0, 345.0, 348.0, 350.0,
    387.0, 390.0,
    418.0,
    430.0, 430.5, 431.0, 431.5,
    433.22, 433.42, 433.65, 433.88, 433.92,
    434.07, 434.17, 434.19, 434.39, 434.42, 434.62, 434.77,
    438.90, 440.17,
    462.75, 464.0, 467.75,
    779.0,
    868.35, 868.4, 868.46, 868.80, 868.95,
    906.4
};
const int ANALYZER_FREQ_COUNT = sizeof(analyzerFreqs) / sizeof(analyzerFreqs[0]);
const float ANALYZER_RSSI_LOW = -97.0f;
const float ANALYZER_RSSI_HIGH = -30.0f;
const float ANALYZER_RSSI_MUL = 2.3f;
const float ANALYZER_DEFAULT_TRIG = -75.0f;
const uint8_t ANALYZER_HIST_CNT = 4;
const int ANALYZER_TRIG_STEP = 5;
const uint16_t ANALYZER_STEP_DELAY_US = 3200;
const float ANALYZER_RSSI_GRAPH_SCALE = 2.0f;
const float ANALYZER_LOCK_MARGIN_DB = 6.0f;



struct AnalyzerNoiseBand {
    uint32_t loHz;
    uint32_t hiHz;
    const char* label; // for future on-screen/debug use
};
static const AnalyzerNoiseBand analyzerNoiseBands[] = {
    {310500000UL, 312300000UL, "310.5-312.3 ISM noise"},
    {467500000UL, 468200000UL, "467.5-468.2 ISM noise"},
    {868000000UL, 868200000UL, "868.0-868.2 LoRa gateways"},
};
static const int ANALYZER_NOISE_BAND_COUNT = sizeof(analyzerNoiseBands) / sizeof(analyzerNoiseBands[0]);

struct AnalyzerScanData {
    float rough_rssi = -127.0f;
    uint32_t rough_freq = 0;
    float fine_rssi = -127.0f;
    uint32_t fine_freq = 0;
};

struct AnalyzerState {
    uint32_t curr_freq = 0;
    uint32_t saved_freq = 0;
    float rssi_now = 0.0f;
    uint32_t hist_freq[ANALYZER_HIST_CNT] = {0};
    uint8_t hist_count[ANALYZER_HIST_CNT] = {0};
    float hist_peak_rssi[ANALYZER_HIST_CNT]; // strongest RSSI seen for each remembered freq this session
    bool has_signal = false;
    float last_rssi = 0.0f;
    float threshold = ANALYZER_DEFAULT_TRIG;
    float noise_floor = ANALYZER_RSSI_LOW; // ambient floor from auto-calibration, see analyzerCalibrateNoiseFloor()
};

AnalyzerScanData analyzerScanResult;
AnalyzerState analyzerState;
float analyzerFilterVal = 0.0f;
float analyzerTrigLevel = ANALYZER_DEFAULT_TRIG;
uint8_t analyzerHoldCount = 0;
bool analyzerLocked = false;
unsigned long analyzerLastDraw = 0;
bool analyzerExitRequested = false;
// Set for a short window right after OK saves a frequency, so
// drawAnalyzerScreen() can flash a confirmation instead of the save
// happening invisibly (previously saved_freq was written but never
// drawn anywhere, so pressing OK looked like it did nothing).
unsigned long analyzerSaveFlashUntil = 0;

// RAW Recorder state
const float RAW_REC_DEFAULT_RSSI = -90.0f;
const float RAW_REC_MIN_RSSI = -90.0f;
const float RAW_REC_MAX_RSSI = -45.0f;
const float RAW_REC_RSSI_STEP = 5.0f;
const unsigned long RAW_REC_FRAME_GAP_US = 7000;
const int RAW_REC_MIN_EDGES = 20;
const int RAW_REC_MAX_EDGES = 900;

bool rawRecorderRunning = false;
bool rawRecorderStopped = false;
bool rawRecorderIgnoreOkRelease = false;
bool rawRecorderIgnoreBackRelease = false;
bool transmitIgnoreOkRelease = false;
bool transmitIgnoreBackRelease = false;
bool rawPlaybackActive = false;
bool rawPlaybackStopRequestedFlag = false;
bool rawPlaybackOkWasPressed = false;
bool rawPlaybackBackWasPressed = false;
unsigned long rawPlaybackBackPressedAt = 0;
bool rawPlaybackIgnoreOkRelease = false;
bool rawPlaybackIgnoreBackRelease = false;
float rawPlaybackShownPct = 0.0f;
float rawPlaybackTargetPct = 0.0f;
unsigned long rawPlaybackStartMs = 0;
unsigned long rawPlaybackLastDrawMs = 0;
TaskHandle_t rawPlaybackTaskHandle = nullptr;
float rawRecorderRssiThreshold = RAW_REC_DEFAULT_RSSI;
float rawRecorderLastRssi = -127.0f;
uint16_t rawRecorderSavedCount = 0;
unsigned long rawRecorderLastDraw = 0;
String rawRecorderLastFile = "";
String rawRecorderSessionFile = "";
String rawRecorderStoppedFile = "";
uint8_t rawRecorderSpectrumVals[52];
int rawRecorderEdges[RAW_REC_MAX_EDGES];
int rawRecorderEdgeCount = 0;
int rawRecorderPrevLevel = LOW;
unsigned long rawRecorderPrevEdgeUs = 0;
unsigned long rawRecorderLastEdgeUs = 0;


//  Global variables for signal list (used by rfPrepareSignalList etc.)

static String currentSubFilePath = "";
static std::vector<String> currentSignalNames;
static bool isRawFile = false;
static tpKeyData currentKeyData;


//  Self-contained "Send" file browser (menuTransmit)

static std::vector<String> transmitFileList;
static int transmitFileIndex = 0;
static bool transmitBrowsing = true;


//  Forward declarations of internal functions

static bool setupCC1101();
static void configureCC1101();
static void restoreReceiveMode();
static void read_rcswitch(tpKeyData* kd);
static void read_raw(tpKeyData* kd);

// Sub-GHz Read screen 
static void drawWaitingSignal();                
static void drawWaitingSignalStatic();           
static void updateWaitingSignalAnimation();     

// RAW Recorder screen
static void drawRawRecorderScreen();
static void drawRawRecorderStatic();             
static void updateRawRecorderAnimation();        
static void drawRawRecorderButton();
static void drawRawRecorderSpectrum(uint8_t x, uint8_t y, uint8_t w, uint8_t h);
static void resetRawRecorderSpectrum();
static void stepRawRecorderRssiThreshold();

static bool fileNameNeedsScroll(const String& fileName);
static void printFileName(const String& fileName, int16_t x, int16_t y);

static void drawKeyScreen(tpKeyData* kd, String fileName = "",
                          bool isSending = false, bool showActions = false);
static void drawError(String st, bool err = true);
static void drawCC1101InitError();
static void waitBackFromCC1101InitError();

static bool saveKeyToSD(tpKeyData* kd);
static bool rfKeyIdIsAllZero(const tpKeyData* kd);
static String allocateNextSignalFileName();

static bool startRawRecorderSession();
static void stopRawRecorderSession(bool flushPending = true, bool discardFile = false);
static bool saveRawFrameToSession(const String& rawData, float rssi);
static void flushRawRecorderFrame();
static void handleRawRecorderCapture();

static bool loadKeyFromSD(String fileName, tpKeyData* kd);
static void syncNextSignalIndexFromFiles();
static void sendSynthKey(tpKeyData* kd);
static bool playRawRecorderFile(const String& fileName);
static void drawRawPlaybackWave(float phase, uint8_t progressPct);

static void stepFrequency(int step);

static void RCSwitch_send(uint64_t data, unsigned int bits, int pulse, int protocol, int repeat);
static void RCSwitch_RAW_send(int *ptrtransmittimings);

static bool sendFixedCodeProtocol(const BruteProtocol* p, uint64_t key, int bitLength, int repeat);
static int  decodeFixedCodeProtocol(const unsigned int* raw, int transitions,
                                     const BruteProtocol* p, uint64_t* outKey);
static void packKeyBits(uint64_t value, int bits, uint8_t keyID[8]);

static void analyzerInit();
static uint32_t analyzerSmoothAvg(uint32_t newVal);
static uint32_t analyzerNearestFreq(uint32_t input);
static void analyzerAddHistory(uint32_t freq, float rssi);
static bool analyzerSaveFreq();
static bool analyzerHandleInput();
static void analyzerDoScan();
static float analyzerRssiToFrac(float rssi);
static void analyzerDrawMeter(int x, int y, int w, int h);
static bool analyzerIsNoiseFreq(uint32_t freqHz);
static float analyzerCalibrateNoiseFloor();
static void analyzerLogFrequency(uint32_t freqHz, float rssi);

static void drawAnalyzerScreen();
static void updateAnalyzerLive();               

static void drawJammerScreen();
static void drawJammerStatic();                 
static void updateJammerAnimation();              
static void startJamming();
static void stopJamming();

static String getTypeName(emKeys tp);
static bool initRfModule(String mode, float freq);
static void deinitRfModule();
static void resetButtonStates();

static void drawBruteIntro();
static void drawBruteIntroStatic();             
static void updateBruteIntroAnimation();        
static void drawBruteConfig();
static void drawBruteProgress(uint16_t progress, uint16_t total);
static void drawBruteProgressStatic(uint16_t total);  
static void updateBruteProgress(uint16_t progress, uint16_t total);  
static bool bruteInitTx();
static void bruteStopTx();
static void bruteSendCode(uint16_t code);
static const BruteProtocol* getBruteProtocolByIndex(int idx);
static void bruteSendSequence(const int* seq, size_t len);

static bool rawPlaybackStopRequested();
static void rawPlaybackDelayMicroseconds(unsigned int durationUs);
static void startRawPlaybackAnimationTask();
static void stopRawPlaybackAnimationTask();
static void pumpRawPlaybackAnimation(bool force = false);
static void armRawPlaybackReleaseGuards();
static void finishRawPlaybackAnimation(uint16_t durationMs);
static bool sendRawBlock(const String& block);

static int nextSignalIndex = 1;
static bool rfSendKey(tpKeyData* kd);
static void transmitScanFiles();
static void transmitScanOneDir(const String& dirPath);
static String rfBaseName(const String& path);
static void drawTransmitFileList();

static void rfHandleSubState(int evt);


//  CC1101 initialization (using ELECHOUSE_CC1101_SRC_DRV)

static bool cc1101PinsReady = false;
static bool cc1101Initialized = false;
static bool cc1101Present = false;
static bool rcSwitchReceiveEnabled = false;

static void prepareCC1101Pins() {
    if (cc1101PinsReady) return;
    pinMode(CC1101_CS, OUTPUT);
    digitalWrite(CC1101_CS, HIGH);
    ELECHOUSE_cc1101.setSpiPin(CC1101_SCK, CC1101_MISO, CC1101_MOSI, CC1101_CS);
    ELECHOUSE_cc1101.setGDO0(CC1101_GDO0);
    cc1101PinsReady = true;
}

// Actually verifies the chip answers on the SPI bus (reads its
// PARTNUM/VERSION registers) instead of assuming success. Returns false,
// and leaves cc1101Initialized false, if nothing answers - callers must
// not proceed to read/show data in that case.
static bool ensureCC1101Initialized() {
    prepareCC1101Pins();
    if (cc1101Initialized) return true;

    if (!ELECHOUSE_cc1101.getCC1101()) {
        cc1101Present = false;
        cc1101Initialized = false;
        return false;
    }

    ELECHOUSE_cc1101.SpiStrobe(0x30);
    delayMicroseconds(100);
    ELECHOUSE_cc1101.Init();

    cc1101Present = true;
    cc1101Initialized = true;
    return true;
}

bool initRfModule(String mode, float freq) {
    if (!ensureCC1101Initialized()) return false;
    ELECHOUSE_cc1101.setMHZ(freq);
    ELECHOUSE_cc1101.setModulation(2);
    if (mode == "tx" || mode == "TX") {
        ELECHOUSE_cc1101.setSyncMode(0);
        ELECHOUSE_cc1101.setCrc(0);
        ELECHOUSE_cc1101.setPktFormat(3);
        ELECHOUSE_cc1101.SetTx();
        pinMode(CC1101_GDO0, OUTPUT);
        digitalWrite(CC1101_GDO0, LOW);
    } else {
        ELECHOUSE_cc1101.setPktFormat(0);
        ELECHOUSE_cc1101.SetRx();
    }
    ELECHOUSE_cc1101.SpiStrobe(0x33);
    delayMicroseconds(100);
    return true;
}

void deinitRfModule() {
    ELECHOUSE_cc1101.SpiStrobe(0x36);
    ELECHOUSE_cc1101.SetRx();
    pinMode(CC1101_GDO0, OUTPUT);
    digitalWrite(CC1101_GDO0, LOW);
}

void restoreReceiveMode() {
    if (!ensureCC1101Initialized()) return;
    configureCC1101();
    if (rcSwitchReceiveEnabled) {
        rcswitch.disableReceive();
        rcSwitchReceiveEnabled = false;
    }
    rcswitch.enableReceive(CC1101_GDO0);
    rcSwitchReceiveEnabled = true;
    rcswitch.resetAvailable();
}

static void enableRcSwitchReceive() {
    if (rcSwitchReceiveEnabled) return;
    pinMode(CC1101_GDO0, INPUT);
    rcswitch.enableReceive(CC1101_GDO0);
    rcSwitchReceiveEnabled = true;
}

static void disableRcSwitchReceive() {
    if (!rcSwitchReceiveEnabled) return;
    rcswitch.disableReceive();
    rcSwitchReceiveEnabled = false;
}

static bool setupCC1101() {
    if (!ensureCC1101Initialized()) return false;
    ELECHOUSE_cc1101.SpiStrobe(0x36);
    delayMicroseconds(100);
    configureCC1101();
    return true;
}

static void configureCC1101() {
    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setMHZ(frequency);
    ELECHOUSE_cc1101.setRxBW(270.0);
    ELECHOUSE_cc1101.setDeviation(0);
    ELECHOUSE_cc1101.setPA(12);
    ELECHOUSE_cc1101.SpiStrobe(0x36);
    delayMicroseconds(100);
    ELECHOUSE_cc1101.SetRx();
}


//  Menu state machine (internal)

enum SubMenuState {
    menuMain,
    menuReceive,
    menuRawRecorder,
    menuTransmit,
    menuAnalyzer,
    menuJammer,
    menuBruteforce,
    menuBruteConfig,
    menuBruteRun
};

static SubMenuState subState = menuMain;
static byte menuIndex = 0;
static bool inSubMenu = false;

// External references from main.cpp
extern SimpleMenu fileMenu;
extern String currentDir;

// Gate for every screen that reads/sends real RF data. If the chip isn't
// found, shows "CC1101 not found", waits for BACK, and drops back to the
// Sub-GHz main menu. Callers must `return` immediately when this is false
// instead of continuing on to display data.
static bool requireCC1101() {
    if (setupCC1101()) return true;
    disableRcSwitchReceive();
    rfLedSet(RF_LED_ERROR);
    drawCC1101InitError();
    waitBackFromCC1101InitError();
    subState = menuMain;
    inSubMenu = true;
    resetButtonStates();
    rfEnterMenu();
    return false;
}


//  Public functions

bool rfInit() {
    rfLedInit();
    bool ok = setupCC1101();
    if (!ok) {
        Serial.println("CC1101 not found");
        rfLedSet(RF_LED_ERROR);
        subState = menuMain;
        inSubMenu = false;
        return false;
    }
    enableRcSwitchReceive();
    subState = menuMain;
    inSubMenu = false;
    return true;
}

void rfProfilesInit() {}

void rfEnterMenu() {
    // Lazy init: bring up the CC1101 on first entry, not at boot.
    // requireCC1101() shows the "not found" screen and bounces back
    // to the main menu if the chip doesn't respond.
    if (!cc1101Initialized) {
        if (!ensureCC1101Initialized()) {
            rfLedSet(RF_LED_ERROR);
            displayShowMessage("CC1101", "Module not found.\nCheck wiring.");
            delay(1200);
            inSubMenu = false;
            subState = menuMain;
            enterState(STATE_MAIN_MENU);
            return;
        }
        enableRcSwitchReceive();
    }

    inSubMenu = true;
    subState = menuMain;
    menuIndex = 0;
    rfLedSet(RF_LED_MENU);
    std::vector<String> items = {"Read", "Read RAW", "Send", "Analyzer", "Jammer", "Brute"};
    displayShowMenu("Sub-GHz", items, 0);
}

// Global flag to request file picker from main loop – defined in main.cpp
extern int g_rfNextState;


//  Sub‑state event handler (defined here, after rfHandleMenuEvent)


//  Sub‑state event handler (complete)

static void rfHandleSubState(int evt) {
    // -----------------------------------------------------------------
    //  MAIN MENU (Sub-GHz top-level)
    // -----------------------------------------------------------------
    if (subState == menuMain) {
        std::vector<String> items = {"Read", "Read RAW", "Send", "Analyzer", "Jammer", "Brute"};
        const int count = items.size();
        if (evt == EVT_UP) {
            menuIndex = (menuIndex == 0) ? (count - 1) : menuIndex - 1;
            displayShowMenu("Sub-GHz", items, menuIndex);
        } else if (evt == EVT_DOWN) {
            menuIndex = (menuIndex == count - 1) ? 0 : menuIndex + 1;
            displayShowMenu("Sub-GHz", items, menuIndex);
        } else if (evt == EVT_OK) {
            switch (menuIndex) {
                case 0: // Read
                    if (!sdIsMounted()) {
                        displayShowMessage("Error", "SD card not mounted");
                        delay(800);
                        rfEnterMenu();
                        return;
                    }
                    subState = menuReceive;
                    if (!requireCC1101()) return;
                    disableRcSwitchReceive();
                    enableRcSwitchReceive();
                    validKeyReceived = false;
                    signals = 0;
                    memset(&keyData1, 0, sizeof(tpKeyData));
                    lastSavedKey = 0;
                    rfHopEnabled = false;
                    rfHopLastMs = millis();
                    rcswitch.resetAvailable();
                    rfLedSet(RF_LED_LISTENING);
                    drawWaitingSignal();
                    break;
                case 1: // Read RAW
                    if (!sdIsMounted()) {
                        displayShowMessage("Error", "SD card not mounted");
                        delay(800);
                        rfEnterMenu();
                        return;
                    }
                    subState = menuRawRecorder;
                    if (!requireCC1101()) return;
                    rawRecorderRunning = false;
                    rawRecorderStopped = false;
                    rawRecorderIgnoreOkRelease = true;
                    rawRecorderIgnoreBackRelease = false;
                    rawRecorderSavedCount = 0;
                    rawRecorderLastRssi = -127.0f;
                    rawRecorderLastFile = "";
                    rawRecorderSessionFile = "";
                    rawRecorderStoppedFile = "";
                    rawRecorderEdgeCount = 0;
                    rawRecorderPrevLevel = digitalRead(CC1101_GDO0);
                    rawRecorderPrevEdgeUs = micros();
                    rawRecorderLastEdgeUs = rawRecorderPrevEdgeUs;
                    rawRecorderLastDraw = 0;
                    resetRawRecorderSpectrum();
                    resetButtonStates();
                    rfLedSet(RF_LED_RAW_ARMED);
                    drawRawRecorderScreen();
                    break;
                case 2: // Send
                    if (!sdIsMounted()) {
                        displayShowMessage("Error", "SD card not mounted");
                        delay(800);
                        rfEnterMenu();
                        return;
                    }
                    transmitScanFiles();
                    if (transmitFileList.empty()) {
                        displayShowMessage("Send", "No .sub files on SD");
                        delay(900);
                        rfEnterMenu();
                        return;
                    }
                    subState = menuTransmit;
                    transmitBrowsing = true;
                    transmitFileIndex = 0;
                    resetButtonStates();
                    rfLedSet(RF_LED_TRANSMIT_BROWSE);
                    drawTransmitFileList();
                    break;
                case 3: // Analyzer
                    subState = menuAnalyzer;
                    if (!requireCC1101()) return;
                    disableRcSwitchReceive();
                    analyzerInit();
                    analyzerExitRequested = false;
                    resetButtonStates();
                    rfLedSet(RF_LED_ANALYZER);
                    drawAnalyzerScreen();
                    break;
                case 4: // Jammer
                    subState = menuJammer;
                    isJamming = false;
                    resetButtonStates();
                    rfLedSet(RF_LED_JAM_IDLE);
                    drawJammerScreen();
                    break;
                case 5: // Brute
                    subState = menuBruteforce;
                    bruteRunning = false;
                    resetButtonStates();
                    rfLedSet(RF_LED_BRUTE_IDLE);
                    drawBruteIntro();
                    break;
            }
        } else if (evt == EVT_BACK) {
            inSubMenu = false; // exit to main menu
        }
        return;
    }

    // -----------------------------------------------------------------
    //  RECEIVE (Read)
    // -----------------------------------------------------------------
    if (subState == menuReceive) {
        if (validKeyReceived) {
            // ... (keep original receive logic – unchanged) ...
            // I reproduce it here from the original file for completeness.
            if (evt == EVT_UP) {
                if (saveKeyToSD(&keyData1)) {
                    lastSavedKey = keyData1.keyID[0];
                    rfLedSet(RF_LED_SAVED);
                    displayShowMessage("Saved", "Key saved to SD");
                } else {
                    rfLedSet(RF_LED_ERROR);
                    displayShowMessage("Error", "Save failed");
                }
                delay(800);
                rfLedSet(RF_LED_CAPTURED);
                drawKeyScreen(&keyData1, "", false, true);
                return;
            } else if (evt == EVT_OK) {
                rfLedSet(RF_LED_SENDING);
                drawKeyScreen(&keyData1, "", true, false);
                bool sent = rfSendKey(&keyData1);
                restoreReceiveMode();
                if (!sent) rfLedSet(RF_LED_ERROR);
                displayShowMessage(sent ? "Sent" : "Error", sent ? "Signal sent" : "Send failed");
                delay(800);
                rfLedSet(RF_LED_CAPTURED);
                drawKeyScreen(&keyData1, "", false, true);
                return;
            } else if (evt == EVT_DOWN) {
                validKeyReceived = false;
                signals = 0;
                memset(&keyData1, 0, sizeof(tpKeyData));
                lastSavedKey = 0;
                rcswitch.resetAvailable();
                rfLedSet(rfHopEnabled ? RF_LED_HOPPING : RF_LED_LISTENING);
                drawWaitingSignal();
                return;
            } else if (evt == EVT_BACK) {
                subState = menuMain;
                resetButtonStates();
                rfEnterMenu();
                return;
            }
            // fall through to allow new signals
        }

        if (evt == EVT_UP) {
            rfHopEnabled = false;
            stepFrequency(1);
            keyData1.frequency = frequency;
            if (!requireCC1101()) return;
            rfLedSet(RF_LED_LISTENING);
            drawWaitingSignal();
        } else if (evt == EVT_DOWN) {
            rfHopEnabled = false;
            stepFrequency(-1);
            keyData1.frequency = frequency;
            if (!requireCC1101()) return;
            rfLedSet(RF_LED_LISTENING);
            drawWaitingSignal();
        } else if (evt == EVT_OK) {
            rfHopEnabled = !rfHopEnabled;
            rfHopLastMs = millis();
            rfLedSet(rfHopEnabled ? RF_LED_HOPPING : RF_LED_LISTENING);
            drawWaitingSignal();
        } else if (evt == EVT_BACK) {
            rfHopEnabled = false;
            subState = menuMain;
            resetButtonStates();
            rfEnterMenu();
            return;
        }

        if (rfHopEnabled && !validKeyReceived && millis() - rfHopLastMs >= RF_HOP_INTERVAL_MS) {
            stepFrequency(1);
            keyData1.frequency = frequency;
            if (!requireCC1101()) return;
            rcswitch.resetAvailable();
            rfHopLastMs = millis();
            drawWaitingSignal();
        }

        if (rcswitch.available()) {
            if (!readRAW) read_rcswitch(&keyData1);
            else read_raw(&keyData1);
            if (validKeyReceived) rfHopEnabled = false;
            if (validKeyReceived && autoSave && (lastSavedKey != keyData1.keyID[0] || keyData1.keyID[0] == 0)) {
                if (saveKeyToSD(&keyData1)) {
                    lastSavedKey = keyData1.keyID[0];
                    validKeyReceived = false;
                    signals = 0;
                    memset(&keyData1, 0, sizeof(tpKeyData));
                    rcswitch.resetAvailable();
                    rfLedSet(RF_LED_LISTENING);
                }
            }
        }

        static unsigned long waitingAnimLastDraw = 0;
        if (!validKeyReceived && millis() - waitingAnimLastDraw >= 100) {
            updateWaitingSignalAnimation();
            waitingAnimLastDraw = millis();
        }
        return;
    }

    // -----------------------------------------------------------------
    //  RAW RECORDER
    // -----------------------------------------------------------------
    if (subState == menuRawRecorder) {
        // ... (keep original raw recorder code unchanged) ...
        // I reproduce it here from the original file.
        if (rawRecorderIgnoreOkRelease) {
            if (evt == EVT_OK) rawRecorderIgnoreOkRelease = false;
        }
        if (rawRecorderIgnoreBackRelease) {
            if (evt == EVT_BACK) rawRecorderIgnoreBackRelease = false;
        }

        bool upClick = (evt == EVT_UP);
        bool downClick = (evt == EVT_DOWN);
        bool okClick = !rawRecorderIgnoreOkRelease && (evt == EVT_OK);
        bool backClick = !rawRecorderIgnoreBackRelease && (evt == EVT_BACK);

        if (rawRecorderStopped) {
            if (downClick) {
                if (rawRecorderStoppedFile.length() > 0 && SD.exists(rawRecorderStoppedFile)) {
                    SD.remove(rawRecorderStoppedFile);
                }
                rawRecorderStopped = false;
                rawRecorderStoppedFile = "";
                rawRecorderLastFile = "";
                rawRecorderSavedCount = 0;
                rawRecorderEdgeCount = 0;
                resetRawRecorderSpectrum();
                rfLedSet(RF_LED_RAW_ARMED);
                drawRawRecorderScreen();
            } else if (upClick) {
                rfLedSet(RF_LED_SAVED);
                displayShowMessage("Saved", "RAW file saved");
                delay(1000);
                rawRecorderStopped = false;
                rawRecorderStoppedFile = "";
                rawRecorderLastFile = "";
                rawRecorderSavedCount = 0;
                rawRecorderEdgeCount = 0;
                resetRawRecorderSpectrum();
                rfLedSet(RF_LED_RAW_ARMED);
                drawRawRecorderScreen();
            } else if (backClick) {
                subState = menuMain;
                resetButtonStates();
                rfEnterMenu();
                return;
            } else if (okClick) {
                playRawRecorderFile(rawRecorderStoppedFile);
                drawRawRecorderScreen();
                resetButtonStates();
            }
            return;
        }

        if (!rawRecorderRunning && upClick) {
            stepRawRecorderRssiThreshold();
            drawRawRecorderScreen();
        }
        if (!rawRecorderRunning && downClick) {
            stepFrequency(1);
            if (!requireCC1101()) return;
            rawRecorderPrevLevel = digitalRead(CC1101_GDO0);
            rawRecorderPrevEdgeUs = micros();
            rawRecorderLastEdgeUs = rawRecorderPrevEdgeUs;
            rawRecorderEdgeCount = 0;
            resetRawRecorderSpectrum();
            rfLedSet(RF_LED_RAW_ARMED);
            drawRawRecorderScreen();
        }
        if (backClick) {
            if (rawRecorderRunning) {
                stopRawRecorderSession(true, false);
                rawRecorderRunning = false;
                rawRecorderLastDraw = 0;
                rfLedSet(RF_LED_RAW_STOPPED);
                drawRawRecorderScreen();
            } else {
                subState = menuMain;
                resetButtonStates();
                rfEnterMenu();
                return;
            }
        } else if (okClick) {
            if (!rawRecorderRunning) {
                if (startRawRecorderSession()) {
                    rawRecorderRunning = true;
                    rawRecorderStopped = false;
                    resetRawRecorderSpectrum();
                    rfLedSet(RF_LED_RAW_RECORDING);
                } else {
                    rfLedSet(RF_LED_ERROR);
                    displayShowMessage("Error", "SD write fail");
                    delay(700);
                    rfLedSet(RF_LED_RAW_ARMED);
                }
            } else {
                String stoppedFile = rawRecorderSessionFile;
                stopRawRecorderSession(true, false);
                rawRecorderRunning = false;
                rawRecorderStopped = true;
                rawRecorderStoppedFile = stoppedFile;
                rfLedSet(RF_LED_RAW_STOPPED);
            }
            rawRecorderLastDraw = 0;
            drawRawRecorderScreen();
        } else if (rawRecorderRunning) {
            handleRawRecorderCapture();
            if (millis() - rawRecorderLastDraw >= 150) {
                updateRawRecorderAnimation();
                rawRecorderLastDraw = millis();
            }
        }
        return;
    }

    // -----------------------------------------------------------------
    //  TRANSMIT (Send file browser)
    // -----------------------------------------------------------------
    if (subState == menuTransmit) {
        // ... (keep original transmit code unchanged) ...
        if (transmitBrowsing) {
            int count = (int)transmitFileList.size();
            if (count == 0) {
                if (evt == EVT_BACK) {
                    subState = menuMain;
                    resetButtonStates();
                    rfEnterMenu();
                }
                return;
            }
            if (evt == EVT_UP) {
                transmitFileIndex = (transmitFileIndex == 0) ? count - 1 : transmitFileIndex - 1;
                drawTransmitFileList();
            } else if (evt == EVT_DOWN) {
                transmitFileIndex = (transmitFileIndex == count - 1) ? 0 : transmitFileIndex + 1;
                drawTransmitFileList();
            } else if (evt == EVT_OK) {
                String path = transmitFileList[transmitFileIndex];
                if (rfPrepareSignalList(path.c_str()) && rfGetSignalCount() > 0) {
                    transmitBrowsing = false;
                    rfLedSet(RF_LED_TRANSMIT_READY);
                    drawKeyScreen(&currentKeyData, rfBaseName(transmitFileList[transmitFileIndex]), false, false);
                    tft.setTextColor(TFT_WHITE);
                    tft.setTextSize(1);
                    tft.setCursor(6, tft.height() - 24);
                    tft.print("OK: Send");
                    tft.setCursor(6, tft.height() - 12);
                    tft.print("BACK: file list");
                } else {
                    rfLedSet(RF_LED_ERROR);
                    displayShowMessage("Error", "Failed to load file");
                    delay(700);
                    rfLedSet(RF_LED_TRANSMIT_BROWSE);
                    drawTransmitFileList();
                }
            } else if (evt == EVT_BACK) {
                subState = menuMain;
                resetButtonStates();
                rfEnterMenu();
                return;
            }
        } else {
            if (evt == EVT_OK) {
                rfLedSet(RF_LED_SENDING);
                drawKeyScreen(&currentKeyData, rfBaseName(transmitFileList[transmitFileIndex]), true, false);
                rfSendSelectedSignal(0);
                return;
            } else if (evt == EVT_BACK) {
                transmitBrowsing = true;
                rfLedSet(RF_LED_TRANSMIT_BROWSE);
                drawTransmitFileList();
            }
        }
        return;
    }

    // -----------------------------------------------------------------
    //  ANALYZER
    // -----------------------------------------------------------------
    if (subState == menuAnalyzer) {
        // ... (keep original analyzer code unchanged) ...
        if (analyzerExitRequested) {
            analyzerExitRequested = false;
            subState = menuMain;
            disableRcSwitchReceive();
            restoreReceiveMode();
            resetButtonStates();
            rfEnterMenu();
            return;
        }
        if (!analyzerHandleInput()) {
            if (analyzerExitRequested) return;
        }
        analyzerDoScan();
        if (analyzerExitRequested) return;
        if (millis() - analyzerLastDraw >= 150) {
            updateAnalyzerLive();
            analyzerLastDraw = millis();
        }
        return;
    }

    // -----------------------------------------------------------------
    //  JAMMER
    // -----------------------------------------------------------------
    if (subState == menuJammer) {
        // ... (keep original jammer code unchanged) ...
        if (evt == EVT_UP) {
            stepFrequency(1);
            if (isJamming) { stopJamming(); startJamming(); }
            drawJammerScreen();
        } else if (evt == EVT_DOWN) {
            stepFrequency(-1);
            if (isJamming) { stopJamming(); startJamming(); }
            drawJammerScreen();
        } else if (evt == EVT_OK) {
            if (!isJamming) {
                startJamming();
                isJamming = true;
                rfLedSet(RF_LED_JAMMING);
            } else {
                stopJamming();
                isJamming = false;
                rfLedSet(RF_LED_JAM_IDLE);
            }
            drawJammerScreen();
        } else if (evt == EVT_BACK) {
            if (isJamming) { stopJamming(); isJamming = false; }
            subState = menuMain;
            restoreReceiveMode();
            resetButtonStates();
            rfEnterMenu();
            return;
        }
        static unsigned long jammerAnimLastDraw = 0;
        if (isJamming && millis() - jammerAnimLastDraw >= 100) {
            drawJammerScreen();
            jammerAnimLastDraw = millis();
        }
        return;
    }

    // -----------------------------------------------------------------
    //  BRUTEFORCE INTRO
    // -----------------------------------------------------------------
    if (subState == menuBruteforce) {
        if (evt == EVT_OK) {
            subState = menuBruteConfig;
            resetButtonStates();
            rfLedSet(RF_LED_BRUTE_IDLE);
            drawBruteConfig();
        } else if (evt == EVT_BACK) {
            subState = menuMain;
            resetButtonStates();
            rfEnterMenu();
            return;
        }
        static unsigned long bruteIntroAnimLastDraw = 0;
        if (millis() - bruteIntroAnimLastDraw >= 150) {
            updateBruteIntroAnimation();
            bruteIntroAnimLastDraw = millis();
        }
        return;
    }

    // -----------------------------------------------------------------
    //  BRUTEFORCE CONFIG
    // -----------------------------------------------------------------
    if (subState == menuBruteConfig) {
        if (evt == EVT_UP) {
            bruteTypeIndex = (bruteTypeIndex + 1) % BRUTE_TYPE_COUNT;
            drawBruteConfig();
        } else if (evt == EVT_DOWN) {
            bruteTypeIndex = (bruteTypeIndex - 1 + BRUTE_TYPE_COUNT) % BRUTE_TYPE_COUNT;
            drawBruteConfig();
        } else if (evt == EVT_LEFT) {
            bruteFreqIndex = (bruteFreqIndex - 1 + BRUTE_FREQ_COUNT) % BRUTE_FREQ_COUNT;
            drawBruteConfig();
        } else if (evt == EVT_RIGHT) {
            bruteFreqIndex = (bruteFreqIndex + 1) % BRUTE_FREQ_COUNT;
            drawBruteConfig();
        } else if (evt == EVT_OK) {
            bruteProgress = 0;
            freqIndex = bruteFreqIndex;
            frequency = bruteFreqOptions[bruteFreqIndex];
            bruteLastStep = millis();
            if (bruteInitTx()) {
                bruteRunning = true;
                subState = menuBruteRun;
                rfLedSet(RF_LED_BRUTE_RUNNING);
                uint32_t total = 1UL << bruteBitsArray[bruteTypeIndex];
                drawBruteProgress(bruteProgress, total);
            } else {
                bruteRunning = false;
                rfLedSet(RF_LED_ERROR);
                drawError("Brute init fail", true);
                delay(800);
                rfLedSet(RF_LED_BRUTE_IDLE);
                subState = menuBruteConfig;
                drawBruteConfig();
            }
        } else if (evt == EVT_BACK) {
            subState = menuBruteforce;
            resetButtonStates();
            drawBruteIntro();
        }
        return;
    }

    // -----------------------------------------------------------------
    //  BRUTEFORCE RUN
    // -----------------------------------------------------------------
    if (subState == menuBruteRun) {
        if (evt == EVT_BACK) {
            bruteRunning = false;
            bruteStopTx();
            subState = menuMain;
            resetButtonStates();
            rfEnterMenu();
            return;
        } else if (evt == EVT_OK) {
            bruteRunning = false;
            bruteStopTx();
            subState = menuBruteforce;
            resetButtonStates();
            rfLedSet(RF_LED_BRUTE_IDLE);
            drawBruteIntro();
        } else if (bruteRunning && millis() - bruteLastStep >= 15) {
            bruteLastStep = millis();
            uint32_t total = 1UL << bruteBitsArray[bruteTypeIndex];
            if (bruteProgress < total) {
                bruteSendCode(bruteProgress);
                bruteProgress++;
                if (bruteProgress % 8 == 0 || bruteProgress == total) {
                    drawBruteProgress(bruteProgress, total);
                }
            }
            if (bruteProgress >= total) {
                bruteRunning = false;
                bruteStopTx();
                subState = menuBruteforce;
                resetButtonStates();
                rfLedSet(RF_LED_BRUTE_IDLE);
                drawBruteIntro();
            }
        }
        return;
    }

    // -----------------------------------------------------------------
    //  Fallback (should never reach here)
    // -----------------------------------------------------------------
    rfEnterMenu();
}


//  UI drawing functions (adapted for ILI9341 TFT)


// --------------------------------------------------------------------
//  Color palette - custom 16-bit RGB565 colors (not just the built-in
//  TFT_* primaries) shared by every screen below, so Read / Send /
//  Jammer / Analyzer all read as one deliberately-designed product
//  instead of each screen picking its own ad-hoc red/green/cyan.
// --------------------------------------------------------------------
constexpr uint16_t rfRgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

constexpr uint16_t UI_BG       = rfRgb565(8, 10, 18);     // app background
constexpr uint16_t UI_PANEL    = rfRgb565(18, 23, 36);    // card / panel fill
constexpr uint16_t UI_PANEL_LT = rfRgb565(27, 33, 50);    // raised / hover panel
constexpr uint16_t UI_BORDER   = rfRgb565(54, 64, 86);    // card borders, dividers
constexpr uint16_t UI_BLUE     = rfRgb565(66, 150, 255);  // primary accent
constexpr uint16_t UI_CYAN     = rfRgb565(72, 222, 255);  // info / idle / scanning
constexpr uint16_t UI_PURPLE   = rfRgb565(168, 120, 255); // secondary accent
constexpr uint16_t UI_GREEN    = rfRgb565(70, 226, 148);  // success / locked / go
constexpr uint16_t UI_AMBER    = rfRgb565(255, 176, 59);  // caution / hopping
constexpr uint16_t UI_RED      = rfRgb565(255, 87, 97);   // danger / transmit / jam
constexpr uint16_t UI_TEXT     = rfRgb565(235, 240, 250); // primary text
constexpr uint16_t UI_TEXT_DIM = rfRgb565(134, 144, 168); // secondary / muted text

// --------------------------------------------------------------------
//  Lightweight animation helpers (non-blocking, millis()-driven)
// --------------------------------------------------------------------
static unsigned long rfUiAnimEpochMs = 0;

// Monotonic phase driver: multiply by a "speed" to control cycle rate.
static inline float rfUiPhase(float speedPerMs) {
    if (rfUiAnimEpochMs == 0) rfUiAnimEpochMs = millis();
    return (float)(millis() - rfUiAnimEpochMs) * speedPerMs;
}

// Ease a displayed value toward a target - call every redraw for smooth
// motion (used for progress bars, RSSI bars, etc).
static void rfUiEaseTowards(float &shown, float target, float rate = 0.2f, float snap = 0.4f) {
    float delta = target - shown;
    shown += delta * rate;
    if (fabs(delta) < snap) shown = target;
}

// Simple on/off blink helper for indicator dots / warning text.
static bool rfUiBlinkOn(unsigned long periodMs = 500) {
    return (millis() / periodMs) % 2UL == 0UL;
}

// Consistent screen header: bold title left, small status text right,
// thin accent rule underneath. Every screen should start with this so
// the whole feature set reads as one coherent UI.
static void rfUiHeader(const char* title, const String& rightText = "", uint16_t accent = UI_BLUE) {
    int w = tft.width();

    // Panel band behind the whole header, instead of text floating
    // directly on the screen background.
    tft.fillRect(0, 0, w, 26, UI_PANEL);

    // Small accent tab beside the title - an instant colour cue for
    // which mode is active, echoed by the underline below.
    tft.fillRoundRect(6, 6, 4, 14, 2, accent);

    tft.setTextColor(UI_TEXT);
    tft.setTextSize(2);
    tft.setCursor(16, 4);
    tft.print(title);

    if (rightText.length() > 0) {
        // Status shown as a rounded "pill" badge rather than bare
        // colored text, matching the badges used elsewhere in the UI.
        tft.setTextSize(1);
        int16_t tw = tft.textWidth(rightText);
        int pillW = tw + 10;
        int pillX = w - pillW - 6;
        int pillY = 6;
        tft.fillRoundRect(pillX, pillY, pillW, 14, 7, accent);
        tft.setTextColor(UI_BG);
        tft.setCursor(pillX + 5, pillY + 3);
        tft.print(rightText);
    }

    tft.drawFastHLine(0, 26, w, accent);
    tft.drawFastHLine(0, 27, w, UI_BORDER);
}

// Expanding "radio wave" pulse rings - used for waiting-for-signal and
// active-transmit indicators. Draws `rings` circles at staggered phases
// that grow from a center dot and fade out (approximated by simply not
// drawing once a ring nears max radius, since the TFT has no alpha).
static void rfUiPulseRings(int cx, int cy, float phase01, uint16_t color, uint8_t rings = 3, int maxR = 40) {
    phase01 = phase01 - floorf(phase01); // wrap to [0,1)
    for (uint8_t i = 0; i < rings; i++) {
        float t = phase01 + (float)i / rings;
        t -= floorf(t);
        int r = 4 + (int)(t * maxR);
        if (t < 0.92f) tft.drawCircle(cx, cy, r, color);
    }
    tft.fillCircle(cx, cy, 4, color);
}

// Rotating tick-mark spinner (cheap substitute for an arc, since most
// TFT libs here don't expose a filled-arc primitive).
static void rfUiSpinner(int cx, int cy, int r, float phaseRad, uint16_t color) {
    const int ticks = 8;
    for (int i = 0; i < ticks; i++) {
        float frac = (float)i / ticks;
        float ang = phaseRad + frac * 2.0f * PI;
        if (frac < 0.2f) continue; // leaves a visible "gap" so rotation reads clearly
        int x0 = cx + (int)(cos(ang) * (r - 4));
        int y0 = cy + (int)(sin(ang) * (r - 4));
        int x1 = cx + (int)(cos(ang) * r);
        int y1 = cy + (int)(sin(ang) * r);
        tft.drawLine(x0, y0, x1, y1, color);
    }
}

// Rounded, filled progress bar. `pct` is 0-100 and should already be the
// eased/smoothed display value, not the raw target, for a fluid look.
static void rfUiProgressBar(int x, int y, int w, int h, float pct, uint16_t fg, uint16_t bg = UI_BORDER) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    tft.drawRoundRect(x, y, w, h, h / 2, bg);
    int innerW = w - 4;
    int fillW = (int)(innerW * (pct / 100.0f));
    if (fillW > 1 && h > 4) tft.fillRoundRect(x + 2, y + 2, fillW, h - 4, (h - 4) / 2, fg);
}

// Short, blocking "captured!" flash - a couple of border pulses. Only
// used for the rare one-shot moment a new signal is decoded, so a brief
// blocking flourish (~350ms) is an acceptable trade for simplicity.
static void rfUiFlashCapture() {
    for (int i = 0; i < 2; i++) {
        tft.drawRoundRect(1, 1, tft.width() - 2, tft.height() - 2, 8, UI_GREEN);
        delay(80);
        tft.drawRoundRect(1, 1, tft.width() - 2, tft.height() - 2, 8, UI_BG);
        delay(80);
    }
    tft.drawRoundRect(1, 1, tft.width() - 2, tft.height() - 2, 8, UI_GREEN);
}

// Two labeled action buttons across the bottom of a screen - used to give
// "Send" / "Save to SD" (and similar) explicit on-screen controls, Flipper
// style, instead of relying only on a text hint line.
static void rfUiDrawTwoButtons(const char* leftLabel, const char* rightLabel,
                                uint16_t leftColor = UI_PANEL_LT, uint16_t rightColor = UI_BLUE) {
    int w = tft.width();
    int h = tft.height();
    int btnW = (w - 30) / 2;
    int btnH = 26;
    int y = h - btnH - 4;
    int x1 = 10;
    int x2 = w - 10 - btnW;

    // Secondary action: outlined "ghost" button.
    tft.drawRoundRect(x1, y, btnW, btnH, 6, UI_BORDER);
    tft.setTextColor(UI_TEXT_DIM);
    tft.setTextSize(1);
    tft.setCursor(x1 + (btnW - tft.textWidth(leftLabel)) / 2, y + 9);
    tft.print(leftLabel);

    // Primary action: filled, accent-colored button.
    tft.fillRoundRect(x2, y, btnW, btnH, 6, rightColor);
    tft.setTextColor(UI_BG);
    tft.setCursor(x2 + (btnW - tft.textWidth(rightLabel)) / 2, y + 9);
    tft.print(rightLabel);
}

// Utility: draw centered text
static void drawCenteredText(const char* text, int y, uint16_t color = TFT_WHITE) {
    tft.setTextColor(color);
    tft.setTextSize(2);
    int16_t x = (tft.width() - tft.textWidth(text)) / 2;
    tft.setCursor(x, y);
    tft.print(text);
}

// Draw waiting signal screen (Read)
static void drawWaitingSignalStatic() {
    tft.fillScreen(UI_BG);

    int w = tft.width();
    int h = tft.height();
    uint16_t accent = rfHopEnabled ? UI_AMBER : UI_CYAN;

    String freqLabel = String(frequency, 2) + " MHz" + (rfHopEnabled ? "  HOP" : "");
    rfUiHeader("Sub-GHz Read", freqLabel, accent);

    // Frequency card
    int cardX = 10, cardY = 34, cardW = w - 20, cardH = 38;
    tft.fillRoundRect(cardX, cardY, cardW, cardH, 8, UI_PANEL);
    tft.drawRoundRect(cardX, cardY, cardW, cardH, 8, UI_BORDER);
    tft.setTextColor(accent);
    tft.setTextSize(3);
    tft.setCursor(cardX + 14, cardY + 7);
    tft.print(String(frequency, 2));
    tft.setTextSize(1);
    tft.setTextColor(UI_TEXT_DIM);
    tft.setCursor(cardX + cardW - 34, cardY + cardH / 2 - 3);
    tft.print("MHz");

    // Footer
    tft.drawFastHLine(0, h - 28, w, UI_BORDER);
    tft.setTextColor(UI_TEXT_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, h - 22);
    tft.print("UP/DN: freq   OK: " + String(rfHopEnabled ? "stop hop" : "hop scan"));
    tft.setCursor(6, h - 11);
    tft.print("BACK: exit");
}

// Call repeatedly to animate. Only touches the middle region.
static void updateWaitingSignalAnimation() {
    int w = tft.width();
    uint16_t accent = rfHopEnabled ? UI_AMBER : UI_CYAN;

    // Animation region — 90px tall, centered
    const int animY = 90;
    const int animH = 90;

    // Clear only the animation area (not the whole screen)
    tft.fillRect(0, animY, w, animH, UI_BG);

    // Pulse rings
    float phase = rfUiPhase(rfHopEnabled ? 0.0009f : 0.00035f);
    rfUiPulseRings(w / 2, animY + animH / 2 - 8, phase, accent, 3, 34);

    // "Listening..." / "Hopping..." text
    tft.setTextColor(UI_TEXT);
    tft.setTextSize(1);
    int dots = (millis() / 400) % 4;
    String waitTxt = rfHopEnabled ? "Hopping" : "Listening";
    for (int i = 0; i < dots; i++) waitTxt += ".";
    int16_t tw = tft.textWidth("Listening...");
    tft.setCursor((w - tw) / 2, animY + animH - 14);
    tft.print(waitTxt);
}

// Backward-compatible wrapper — call this if you only want to redraw
// everything (e.g. on entry or when the frequency changes).
static void drawWaitingSignal() {
    drawWaitingSignalStatic();
    updateWaitingSignalAnimation();
}

// Draw RAW Recorder screen
static void drawRawRecorderStatic() {
    tft.fillScreen(TFT_BLACK);
    String status = rawRecorderStopped ? "Stopped" : (rawRecorderRunning ? "REC" : "Ready");
    rfUiHeader("RAW Capture", String(frequency, 2) + " MHz",
               rawRecorderRunning ? TFT_RED : TFT_CYAN);
    tft.drawRoundRect(5, 30, tft.width() - 10, 110, 6, TFT_WHITE);

    // Static footer text
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(6, tft.height() - 12);
    tft.print("Thr " + String((int)rawRecorderRssiThreshold) + "dB  " + status);
}

static void updateRawRecorderAnimation() {
    // Only the spectrum animates
    tft.fillRect(8, 33, tft.width() - 16, 104, TFT_BLACK);
    drawRawRecorderSpectrum(8, 33, tft.width() - 16, 104);

    // REC blink dot
    static bool lastBlink = false;
    bool nowBlink = rawRecorderRunning && rfUiBlinkOn(400);
    if (nowBlink != lastBlink) {
        lastBlink = nowBlink;
        int dotX = tft.width() / 2 - 54;
        int dotY = tft.height() - 26;
        tft.fillCircle(dotX, dotY, 5, nowBlink ? TFT_RED : TFT_BLACK);
    }
}

static void drawRawRecorderScreen() {
    drawRawRecorderStatic();
    updateRawRecorderAnimation();
    drawRawRecorderButton();
}

static void drawRawRecorderSpectrum(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    const uint8_t barCount = min((uint8_t)sizeof(rawRecorderSpectrumVals), (uint8_t)(w / 3));
    if (rawRecorderRunning) {
        for (uint8_t i = 0; i < barCount - 1; i++) {
            rawRecorderSpectrumVals[i] = rawRecorderSpectrumVals[i + 1];
        }
        float rssi = ELECHOUSE_cc1101.getRssi();
        if (rssi < -100) rssi = -100;
        if (rssi > -30) rssi = -30;
        rawRecorderSpectrumVals[barCount - 1] = 1 + (uint8_t)(((rssi + 100) * (h - 1)) / 70);
    }
    for (uint8_t i = 0; i < barCount; i++) {
        uint8_t bar = rawRecorderSpectrumVals[i];
        if (bar > 0) {
            uint8_t px = x + i * 3;
            tft.drawFastVLine(px, y + h - bar, bar, TFT_WHITE);
            tft.drawFastVLine(px + 1, y + h - bar, bar, TFT_WHITE);
        }
    }
}

static void drawRawRecorderButton() {
    const char* label = rawRecorderStopped ? "Stopped" : (rawRecorderRunning ? "Stop" : "REC");
    int btnX = tft.width() / 2 - 40;
    int btnY = tft.height() - 40;
    tft.fillRoundRect(btnX, btnY, 80, 30, 6, TFT_WHITE);
    tft.setTextColor(TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(btnX + 20, btnY + 6);
    tft.print(label);
    // Blinking record dot to the left of the button while actively capturing.
    if (rawRecorderRunning && rfUiBlinkOn(400)) {
        tft.fillCircle(btnX - 14, btnY + 15, 5, TFT_RED);
    }
}

static void resetRawRecorderSpectrum() {
    for (uint8_t i = 0; i < sizeof(rawRecorderSpectrumVals); i++) {
        rawRecorderSpectrumVals[i] = 0;
    }
}

static void stepRawRecorderRssiThreshold() {
    rawRecorderRssiThreshold += RAW_REC_RSSI_STEP;
    if (rawRecorderRssiThreshold > RAW_REC_MAX_RSSI) {
        rawRecorderRssiThreshold = RAW_REC_MIN_RSSI;
    }
}

// Key display screen
static void drawKeyScreen(tpKeyData* kd, String fileName, bool isSending, bool showActions) {
    tft.fillScreen(UI_BG);
    int w = tft.width();
    uint16_t accent = fileName.length() > 0 ? UI_CYAN : UI_GREEN;
    rfUiHeader(fileName.length() > 0 ? "Signal File" : "Signal Captured", "", accent);

    // Everything below the header lives on one bordered info card, with
    // dim labels and bright values, instead of a flat list of text lines.
    int cardX = 8, cardY = 32, cardW = w - 16;
    int y = cardY + 8;
    tft.setTextSize(1);

    if (fileName.length() > 0) {
        tft.setTextColor(UI_TEXT_DIM);
        tft.setCursor(cardX + 6, y);
        tft.print("File:");
        tft.setTextColor(UI_TEXT);
        printFileName(fileName, cardX + 6 + tft.textWidth("File: "), y);
        y += 18;
    } else {
        y += 4;
    }

    String st = "";
    if (kd->type == kLINEAR) {
        st = "Unknown";
    } else {
        bool leadingZero = true;
        for (int i = 0; i < 8; i++) {
            if (kd->keyID[i] != 0 || !leadingZero || i == 7) {
                leadingZero = false;
                if (kd->keyID[i] < 0x10) st += "0";
                st += String(kd->keyID[i], HEX);
                if (i < 7) st += ":";
            }
        }
    }

    // Code gets its own emphasized row in the accent color - it's the
    // one field that actually identifies the signal.
    tft.setTextColor(UI_TEXT_DIM);
    tft.setCursor(cardX + 6, y);
    tft.print("Code");
    y += 12;
    tft.setTextColor(accent);
    tft.setTextSize(2);
    tft.setCursor(cardX + 6, y);
    tft.print(st);
    tft.setTextSize(1);
    y += 22;

    tft.drawFastHLine(cardX + 4, y, cardW - 8, UI_BORDER);
    y += 8;

    tft.setTextColor(UI_TEXT_DIM);
    tft.setCursor(cardX + 6, y);
    tft.print("Type");
    tft.setTextColor(UI_TEXT);
    tft.setCursor(cardX + 60, y);
    tft.print(getTypeName((emKeys)kd->type));
    y += 16;

    tft.setTextColor(UI_TEXT_DIM);
    tft.setCursor(cardX + 6, y);
    tft.print("Freq");
    tft.setTextColor(UI_TEXT);
    tft.setCursor(cardX + 60, y);
    tft.print(String(kd->frequency, 2) + " MHz");
    y += 16;

    if (kd->bitLength > 0) {
        tft.setTextColor(UI_TEXT_DIM);
        tft.setCursor(cardX + 6, y);
        tft.print("Bits");
        tft.setTextColor(UI_TEXT);
        tft.setCursor(cardX + 60, y);
        tft.print(String(kd->bitLength));
        y += 16;
    }

    tft.drawRoundRect(cardX, cardY, cardW, y - cardY + 6, 8, UI_BORDER);

    if (isSending) {
        tft.setTextColor(rfUiBlinkOn(350) ? UI_RED : UI_TEXT);
        tft.setTextSize(2);
        String txt = "Sending...";
        tft.setCursor((w - tft.textWidth(txt)) / 2, tft.height() - 28);
        tft.print(txt);
    } else if (showActions) {
        // Explicit Send / Save to SD buttons for a freshly captured signal,
        // Flipper-style, plus a small hint for the discard action.
        rfUiDrawTwoButtons("Save (UP)", "Send (OK)", UI_PANEL_LT, UI_GREEN);
        tft.setTextColor(UI_TEXT_DIM);
        tft.setTextSize(1);
        tft.setCursor(6, tft.height() - 34);
        tft.print("DOWN: discard   BACK: exit");
    }
}

static void printFileName(const String& fileName, int16_t x, int16_t y) {
    tft.setCursor(x, y);
    if (fileName.length() > 20) {
        tft.print(fileName.substring(0, 18) + "..");
    } else {
        tft.print(fileName);
    }
}

static bool fileNameNeedsScroll(const String& fileName) {
    return fileName.length() > 20;
}

// Error screen
static void drawError(String st, bool err) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 30);
    tft.print(err ? "Error!" : "OK");
    tft.setTextSize(1);
    tft.setCursor(10, 60);
    tft.print(st);
}

static void drawCC1101InitError() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setCursor(10, 30);
    tft.print("CC1101 not found");
    tft.setTextSize(1);
    tft.setCursor(10, 60);
    tft.print("Check wiring / module seating");
    tft.setCursor(10, 76);
    tft.print("Press BACK to return");
}

static void waitBackFromCC1101InitError() {
    while (true) {
        int evt = buttonsPoll();
        if (evt == EVT_BACK) break;
        delay(10);
    }
}


//  SD card operations (adapted from subghz.cpp)


static void ensureSubDir() {
    // We'll use /subghz by default
}

static int signalNumberFromName(String name) {
    int lastSlash = name.lastIndexOf('/');
    if (lastSlash >= 0) name = name.substring(lastSlash + 1);
    if (!(name.startsWith("Signal_") || name.startsWith("signal_")) || !name.endsWith(".sub")) return 0;
    String numStr = name.substring(7, name.length() - 4);
    if (numStr.length() == 0) return 0;
    for (uint16_t i = 0; i < numStr.length(); i++) {
        if (!isDigit(numStr[i])) return 0;
    }
    return numStr.toInt();
}

// BUGFIX (freeze): the previous version recursively walked the WHOLE
// card several folders deep. That has two real ways to hang the device:
// (1) some SD/FatFs cores hand back "." and ".." as real directory
// entries - recursing into "." reopens the same directory and sees "."
// again, so it never returns; (2) even capped, a real SD card can
// easily have thousands of files across many folders (assets, other
// apps' data, OS junk), and depth-first recursion could burn the whole
// entry budget on unrelated folders before ever reaching /subghz - so
// on top of the freeze risk, it could silently fail to find your own
// saved signals. Deep recursion just isn't a safe operation to do
// synchronously on an SPI SD card.
//
// This replaces it with two single-level (non-recursive) directory
// listings only: the card's root, and /subghz. Both are flat reads with
// no recursion, so total work is bounded by "however many files are
// directly in these two folders" - it cannot blow up no matter how big
// or deep the rest of the card's folder tree is. This covers the two
// places .sub files realistically end up (dropped in root, or saved by
// this device into /subghz) without the open-ended walk that hung.
static const int TRANSMIT_SCAN_MAX_FILES = 200; // sanity cap even for a single folder

static String rfBaseName(const String& path) {
    int slash = path.lastIndexOf('/');
    return (slash >= 0) ? path.substring(slash + 1) : path;
}

static void transmitScanOneDir(const String& dirPath) {
    File dir = SD.open(dirPath);
    if (!dir) return;
    int visited = 0;
    while ((int)transmitFileList.size() < TRANSMIT_SCAN_MAX_FILES) {
        File entry = dir.openNextFile();
        if (!entry) break;
        visited++;
        if ((visited % 8) == 0) yield(); // keep the watchdog fed / stay cooperative
        if (!entry.isDirectory()) {
            String leaf = String(entry.name());
            int slash = leaf.lastIndexOf('/');
            if (slash >= 0) leaf = leaf.substring(slash + 1);
            if (leaf != "." && leaf != ".." && leaf.endsWith(".sub")) {
                String full = (dirPath == "/") ? ("/" + leaf) : (dirPath + "/" + leaf);
                transmitFileList.push_back(full);
            }
        }
        entry.close();
    }
    dir.close();
}

static void transmitScanFiles() {
    transmitFileList.clear();
    // /subghz first - this device's own saved signals should always show
    // up even if root happens to be large.
    if (SD.exists("/subghz")) transmitScanOneDir("/subghz");
    transmitScanOneDir("/");
}

static void drawTransmitFileList() {
    if (transmitFileIndex >= (int)transmitFileList.size()) transmitFileIndex = 0;
    std::vector<String> displayNames;
    displayNames.reserve(transmitFileList.size());
    for (const String& p : transmitFileList) displayNames.push_back(rfBaseName(p));
    displayShowMenu("Send: pick file", displayNames, transmitFileIndex);
}

static void syncNextSignalIndexFromFiles() {
    ensureSubDir();
    int maxIndex = 0;
    if (!SD.exists("/subghz")) return;
    File dir = SD.open("/subghz");
    if (dir) {
        while (true) {
            File entry = dir.openNextFile();
            if (!entry) break;
            if (!entry.isDirectory() && String(entry.name()).endsWith(".sub")) {
                int num = signalNumberFromName(entry.name());
                if (num > maxIndex) maxIndex = num;
            }
            entry.close();
        }
        dir.close();
    }
    nextSignalIndex = maxIndex + 1;
}

static String allocateNextSignalFileName() {
    ensureSubDir();
    syncNextSignalIndexFromFiles();
    int fileNum = nextSignalIndex > 0 ? nextSignalIndex : 1;
    String fileName;
    while (fileNum <= 999) {
        fileName = "/subghz/Signal_" + String(fileNum) + ".sub";
        if (!SD.exists(fileName)) break;
        fileNum++;
    }
    if (fileNum > 999) return "";
    nextSignalIndex = fileNum + 1;
    return fileName;
}

static bool saveKeyToSD(tpKeyData* kd) {
    if (!kd || kd->codeLenth == 0 || kd->frequency == 0.0) return false;
    String fileName = allocateNextSignalFileName();
    if (fileName.length() == 0) return false;
    if (!SD.exists("/subghz")) SD.mkdir("/subghz");
    File file = SD.open(fileName, FILE_WRITE);
    if (!file) return false;

    // BUGFIX: a Read RAW capture that read_raw() couldn't match to any
    // known protocol has kd->type == kUnknown, but the exact captured
    // pulse train is still sitting in kd->rawData. The same is true for
    // the coarse "roughly CAME-length" transition-count guess, which sets
    // kd->type == kCAME with an all-zero keyID because no real bits were
    // ever decoded (see read_raw() / rfKeyIdIsAllZero()). This function
    // used to ignore kd->rawData entirely and always write a "Key File" -
    // for either case that meant a file with no real signal in it (either
    // "Protocol: Unknown" with nothing to send, or a fabricated all-zero
    // CAME code that can never open anything). The capture was silently
    // thrown away, with no way to ever send it again once saved. Write it
    // as a proper "RAW File" (same format the RAW Recorder feature
    // already uses) instead, so the actual waveform survives to be
    // replayed later.
    bool noRealDecode = (kd->type == kUnknown) ||
                         (kd->type == kCAME && rfKeyIdIsAllZero(kd));
    if (noRealDecode && kd->rawData[0] != '\0') {
        file.println("Filetype: Flipper SubGhz RAW File");
        file.println("Version: 1");
        file.print("Frequency: ");
        file.println((unsigned long)(kd->frequency * 1000000));
        file.println("Preset: FuriHalSubGhzPresetOok650Async");
        file.println("Protocol: RAW");
        file.print("RAW_Data: ");
        file.println(kd->rawData);
        file.close();
        return true;
    }

    file.println("Filetype: Flipper SubGhz Key File");
    file.println("Version: 1");
    file.print("Frequency: ");
    file.println((unsigned long)(kd->frequency * 1000000));
    file.print("Protocol: ");
    file.println(getTypeName((emKeys)kd->type));
    file.print("Bit: ");
    file.println(kd->bitLength);
    file.print("Key: ");
    for (int i = 0; i < 8; i++) {
        if (kd->keyID[i] < 0x10) file.print("0");
        file.print(kd->keyID[i], HEX);
        if (i < 7) file.print(" ");
    }
    file.println();
    file.print("TE: ");
    file.println(kd->te);
    file.print("RCProto: ");
    file.println(kd->rcProtocolNum);
    file.close();
    return true;
}

// Real Flipper firmware protocol name strings don't always match what this
// codebase originally checked for verbatim - e.g. it writes "Holtek" not
// "HOLTEK", "Star Line" (with a space) not "StarLine", "Nice FloR-S" not
// "NICE", and "LinearDelta3" (no spaces/hyphen) not "Linear Delta-3". A
// straight String::equals() against the wrong casing/spacing silently
// fell through to kUnknown and got refused as "not supported" even though
// the protocol itself was already implemented. Normalize both sides
// (uppercase, strip spaces/hyphens/underscores) before comparing so a
// real Flipper-written .sub file matches regardless of formatting.
static String rfNormalizeProtoName(String s) {
    String out;
    out.reserve(s.length());
    for (unsigned int i = 0; i < s.length(); i++) {
        char c = s.charAt(i);
        if (c == ' ' || c == '-' || c == '_') continue;
        if (c >= 'a' && c <= 'z') c -= 32; // toupper
        out += c;
    }
    return out;
}

static bool rfProtoNameIs(const String& normalized, const char* candidate) {
    return normalized == rfNormalizeProtoName(String(candidate));
}

static emKeys rfProtocolTypeFromName(const String& rawName) {
    String n = rfNormalizeProtoName(rawName);
    if (rfProtoNameIs(n, "Princeton")) return kPrinceton;
    if (rfProtoNameIs(n, "RcSwitch")) return kRcSwitch;
    if (rfProtoNameIs(n, "CAME")) return kCAME;
    if (rfProtoNameIs(n, "NICE") || rfProtoNameIs(n, "Nice Flo")) return kNICE;
    if (rfProtoNameIs(n, "Nice FloR-S")) return kNiceFlorS;
    if (rfProtoNameIs(n, "Holtek")) return kHOLTEK;
    if (rfProtoNameIs(n, "KeeLoq")) return kKeeLoq;
    if (rfProtoNameIs(n, "Star Line") || rfProtoNameIs(n, "StarLine")) return kStarLine;
    if (rfProtoNameIs(n, "Chamberlain")) return kChamberlain;
    if (rfProtoNameIs(n, "Ansonic")) return kAnsonic;
    if (rfProtoNameIs(n, "LinearDelta3") || rfProtoNameIs(n, "Linear Delta-3")) return kLinearDelta3;
    if (rfProtoNameIs(n, "Somfy Keytis") || rfProtoNameIs(n, "Somfy Telis") ||
        rfProtoNameIs(n, "Somfy RTS")) return kSomfyRTS;
    if (rfProtoNameIs(n, "Security+1.0") || rfProtoNameIs(n, "SecPlus v1") ||
        rfProtoNameIs(n, "SecPlus_v1")) return kSecPlusV1;
    if (rfProtoNameIs(n, "Security+2.0") || rfProtoNameIs(n, "SecPlus v2") ||
        rfProtoNameIs(n, "SecPlus_v2")) return kSecPlusV2;
    if (rfProtoNameIs(n, "FAAC SLH")) return kFaacSLH;
    if (rfProtoNameIs(n, "BFT Mitto") || rfProtoNameIs(n, "Mitto")) return kBftMitto;
    if (rfProtoNameIs(n, "CAME Atomo")) return kCameAtomo;
    if (rfProtoNameIs(n, "AN-Motors") || rfProtoNameIs(n, "AN Motors") ||
        rfProtoNameIs(n, "AnMotors")) return kAnMotors;
    if (rfProtoNameIs(n, "HCS101")) return kHcs101;
    if (rfProtoNameIs(n, "RAW")) return kLINEAR;
    if (rfProtoNameIs(n, "Gate TX")) return kGateTX;
    if (rfProtoNameIs(n, "SMC5326")) return kSMC5326;
    if (rfProtoNameIs(n, "MegaCode")) return kMegaCode;
    if (rfProtoNameIs(n, "UNILARM")) return kUNILARM;
    if (rfProtoNameIs(n, "Alutech AT-4N") || rfProtoNameIs(n, "AlutechAT4N")) return kAlutechAT4N;
    if (rfProtoNameIs(n, "DoorHan")) return kDoorHan;
    if (rfProtoNameIs(n, "Hormann") || rfProtoNameIs(n, "Hörmann")) return kHormann;
    if (rfProtoNameIs(n, "Marantec")) return kMarantec;
    if (rfProtoNameIs(n, "Kia/Hyundai") || rfProtoNameIs(n, "KiaHyundai") || rfProtoNameIs(n, "Hyundai")) return kKiaHyundai;
    if (rfProtoNameIs(n, "Aprimatic")) return kAprimatic;
    if (rfProtoNameIs(n, "IronLogic")) return kIronLogic;
    if (rfProtoNameIs(n, "Sommer")) return kSommer;
    if (rfProtoNameIs(n, "Mutancode")) return kMutancode;
    if (rfProtoNameIs(n, "MHouse")) return kMHouse;
    if (rfProtoNameIs(n, "iDo")) return kiDo;
    return kUnknown;
}

static bool loadKeyFromSD(String fileName, tpKeyData* kd) {
    if (fileName.length() == 0) return false;
    // Send's file browser now passes full absolute paths (see
    // transmitScanFiles) since files can live anywhere on the card, not
    // just /subghz. Only bare names still get the /subghz/ prefix.
    String fullPath = fileName.startsWith("/") ? fileName : ("/subghz/" + fileName);
    if (!SD.exists(fullPath)) fullPath = fileName; // last-resort fallback
    File file = SD.open(fullPath, FILE_READ);
    if (!file) return false;
    memset(kd, 0, sizeof(tpKeyData));
    String line;
    while (file.available()) {
        line = file.readStringUntil('\n');
        line.trim();
        if (line.startsWith("Filetype:")) {
            bool isKey = line.equals("Filetype: Flipper SubGhz Key File");
            bool isRaw = line.equals("Filetype: Flipper SubGhz RAW File");
            if (!isKey && !isRaw) { file.close(); return false; }
        } else if (line.startsWith("Frequency:")) {
            kd->frequency = line.substring(10).toFloat() / 1000000.0;
        } else if (line.startsWith("Protocol:")) {
            String prot = line.substring(9);
            prot.trim();
            kd->type = rfProtocolTypeFromName(prot);
        } else if (line.startsWith("Bit:")) {
            kd->bitLength = line.substring(4).toInt();
            kd->codeLenth = kd->bitLength;
        } else if (line.startsWith("Key:")) {
            String keyStr = line.substring(4);
            keyStr.trim();
            keyStr.replace(" ", "");
            for (int i = 0; i < keyStr.length() / 2 && i < 8; i++) {
                String byteStr = keyStr.substring(i * 2, i * 2 + 2);
                kd->keyID[i] = strtol(byteStr.c_str(), NULL, 16);
            }
        } else if (line.startsWith("TE:")) {
            kd->te = line.substring(3).toInt();
        } else if (line.startsWith("RCProto:")) {
            kd->rcProtocolNum = (uint8_t)line.substring(8).toInt();
        } else if (line.startsWith("RAW_Data:")) {
            // For RAW files we store raw data, but we handle them separately.
            kd->type = kLINEAR;
            kd->codeLenth = 1;
            strncpy(kd->rawData, line.substring(9).c_str(), sizeof(kd->rawData) - 1);
        }
    }
    file.close();
    return true;
}


//  RAW Recorder sessions

static bool startRawRecorderSession() {
    String fileName = allocateNextSignalFileName();
    if (fileName.length() == 0) return false;
    if (!SD.exists("/subghz")) SD.mkdir("/subghz");
    File file = SD.open(fileName, FILE_WRITE);
    if (!file) return false;
    file.println("Filetype: Flipper SubGhz RAW File");
    file.println("Version: 1");
    file.print("Frequency: ");
    file.println((unsigned long)(frequency * 1000000));
    file.println("Preset: FuriHalSubGhzPresetOok650Async");
    file.println("Protocol: RAW");
    file.close();
    rawRecorderSessionFile = fileName;
    int lastSlash = fileName.lastIndexOf('/');
    rawRecorderLastFile = (lastSlash >= 0) ? fileName.substring(lastSlash + 1) : fileName;
    rawRecorderEdgeCount = 0;
    pinMode(CC1101_GDO0, INPUT);
    rawRecorderPrevLevel = digitalRead(CC1101_GDO0);
    rawRecorderPrevEdgeUs = micros();
    rawRecorderLastEdgeUs = rawRecorderPrevEdgeUs;
    return true;
}

static bool saveRawFrameToSession(const String& rawData, float rssi) {
    (void)rssi;
    if (rawRecorderSessionFile.length() == 0 || rawData.length() == 0) return false;
    File file = SD.open(rawRecorderSessionFile, FILE_APPEND);
    if (!file) return false;
    file.print("RAW_Data: ");
    file.println(rawData);
    file.close();
    return true;
}

static void flushRawRecorderFrame() {
    if (rawRecorderEdgeCount < RAW_REC_MIN_EDGES) {
        rawRecorderEdgeCount = 0;
        return;
    }
    rawRecorderLastRssi = ELECHOUSE_cc1101.getRssi();
    if (rawRecorderLastRssi < rawRecorderRssiThreshold) {
        rawRecorderEdgeCount = 0;
        return;
    }
    String data = "";
    for (int i = 0; i < rawRecorderEdgeCount; i++) {
        if (i > 0) data += " ";
        data += String(rawRecorderEdges[i]);
    }
    if (saveRawFrameToSession(data, rawRecorderLastRssi)) {
        rawRecorderSavedCount++;
    }
    rawRecorderEdgeCount = 0;
}

static void stopRawRecorderSession(bool flushPending, bool discardFile) {
    String fileToDiscard = rawRecorderSessionFile;
    if (flushPending) flushRawRecorderFrame();
    else rawRecorderEdgeCount = 0;
    rawRecorderSessionFile = "";
    if (discardFile && fileToDiscard.length() > 0 && SD.exists(fileToDiscard)) {
        SD.remove(fileToDiscard);
    }
}

static void handleRawRecorderCapture() {
    unsigned long nowUs = micros();
    int level = digitalRead(CC1101_GDO0);
    if (level != rawRecorderPrevLevel) {
        unsigned long durUs = nowUs - rawRecorderPrevEdgeUs;
        rawRecorderPrevEdgeUs = nowUs;
        rawRecorderLastEdgeUs = nowUs;
        if (durUs >= 40 && durUs <= 60000 && rawRecorderEdgeCount < RAW_REC_MAX_EDGES) {
            int signedDur = rawRecorderPrevLevel ? (int)durUs : -(int)durUs;
            rawRecorderEdges[rawRecorderEdgeCount++] = signedDur;
        } else if (durUs > 60000 && rawRecorderEdgeCount >= RAW_REC_MIN_EDGES) {
            flushRawRecorderFrame();
        } else if (rawRecorderEdgeCount >= RAW_REC_MAX_EDGES) {
            flushRawRecorderFrame();
        }
        rawRecorderPrevLevel = level;
    }
    if (rawRecorderEdgeCount >= RAW_REC_MIN_EDGES && (nowUs - rawRecorderLastEdgeUs) > RAW_REC_FRAME_GAP_US) {
        flushRawRecorderFrame();
        rawRecorderPrevEdgeUs = nowUs;
        rawRecorderLastEdgeUs = nowUs;
    }
}


//  RCSwitch read functions

static void read_rcswitch(tpKeyData* kd) {
    uint32_t decoded = rcswitch.getReceivedValue();
    if (decoded) {
        signals++;
        kd->frequency = frequency;
        int numBytes = (kd->bitLength + 7) / 8;
        if (numBytes > 4) numBytes = 4;
        for (int i = 0; i < 8; i++) {
            kd->keyID[i] = (i < 8 - numBytes) ? 0 : (decoded >> ((numBytes - 1 - (i - (8 - numBytes))) * 8)) & 0xFF;
        }
        kd->type = (rcswitch.getReceivedProtocol() == 1 && rcswitch.getReceivedBitlength() == 24) ? kPrinceton : kRcSwitch;
        if (rcswitch.getReceivedBitlength() <= 40 && rcswitch.getReceivedProtocol() == 11) {
            kd->type = kCAME;
        }
        kd->rcProtocolNum = (uint8_t)rcswitch.getReceivedProtocol();
        kd->te = rcswitch.getReceivedDelay();
        kd->bitLength = rcswitch.getReceivedBitlength();
        kd->codeLenth = kd->bitLength;
        strncpy(kd->preset, "0", sizeof(kd->preset) - 1);
        validKeyReceived = true;
        rfLedSet(RF_LED_CAPTURED);
        drawKeyScreen(kd, "", false, true);
        rfUiFlashCapture();
    }
    rcswitch.resetAvailable();
}

static void read_raw(tpKeyData* kd) {
    unsigned int* raw = rcswitch.getReceivedRawdata();
    uint32_t decoded = rcswitch.getReceivedValue();
    String data = "";
    int transitions = 0;
    for (transitions = 0; transitions < MAX_DATA_LOG && raw[transitions] != 0; transitions++) {
        if (transitions > 0) data += " ";
        int sign = (transitions % 2 == 0) ? 1 : -1;
        data += String(sign * (int)raw[transitions]);
    }
    if (transitions > 20) {
        signals++;
        kd->frequency = frequency;
        if (data.length() >= sizeof(kd->rawData)) data = data.substring(0, sizeof(kd->rawData) - 1);
        strncpy(kd->rawData, data.c_str(), sizeof(kd->rawData) - 1);
        kd->type = kUnknown;
        kd->te = 0;
        kd->bitLength = 0;
        strncpy(kd->preset, "0", sizeof(kd->preset) - 1);
        kd->codeLenth = transitions;

        if (decoded) {
            int numBytes = (rcswitch.getReceivedBitlength() + 7) / 8;
            if (numBytes > 4) numBytes = 4;
            for (int i = 0; i < 8; i++) {
                kd->keyID[i] = (i < 8 - numBytes) ? 0 : (decoded >> ((numBytes - 1 - (i - (8 - numBytes))) * 8)) & 0xFF;
            }
            kd->type = (rcswitch.getReceivedProtocol() == 1 && rcswitch.getReceivedBitlength() == 24) ? kPrinceton : kRcSwitch;
            if (rcswitch.getReceivedBitlength() <= 40 && rcswitch.getReceivedProtocol() == 11) {
                kd->type = kCAME;
            }
            kd->rcProtocolNum = (uint8_t)rcswitch.getReceivedProtocol();
            kd->te = rcswitch.getReceivedDelay();
            kd->bitLength = rcswitch.getReceivedBitlength();
            kd->codeLenth = kd->bitLength;
        } else {
            // RCSwitch's own decoder didn't recognize this signal, but we
            // still have the raw edge buffer - try matching it against the
            // same fixed-code pulse tables the brute-forcer uses, so a
            // CAME/NICE/HOLTEK remote gets identified (and its actual bits
            // decoded, so it can be sent again) instead of only guessed
            // from how many edges it happened to have.
            uint64_t matchedKey = 0;
            int matchedBits = 0;
            struct { emKeys type; const BruteProtocol* proto; } fixedCandidates[] = {
                { kCAME,        &protoCame },
                { kNICE,        &protoNice },
                { kHOLTEK,      &protoHoltek },
                { kChamberlain, &protoChamber },
                { kAnsonic,     &protoAnsonic },
                { kLinearDelta3,&protoLinearDelta3 },
                // Add new static protocols to this list as well
                { kGateTX,      &protoGateTX },
                { kSMC5326,     &protoSMC5326 },
                { kMegaCode,    &protoMegaCode },
                { kUNILARM,     &protoUNILARM },
            };
            bool matched = false;
            for (size_t c = 0; c < sizeof(fixedCandidates) / sizeof(fixedCandidates[0]); c++) {
                matchedBits = decodeFixedCodeProtocol(raw, transitions, fixedCandidates[c].proto, &matchedKey);
                if (matchedBits > 0) {
                    kd->type = fixedCandidates[c].type;
                    kd->bitLength = matchedBits;
                    kd->codeLenth = matchedBits;
                    packKeyBits(matchedKey, matchedBits, kd->keyID);
                    matched = true;
                    break;
                }
            }

            // ---- If still not matched, try edge-count detection for new protocols ----
            if (!matched) {
                // Define a table: { protocol type, min edges, max edges, rolling? }
                static const struct {
                    emKeys type;
                    int minEdges;
                    int maxEdges;
                    bool rolling;
                } edgeMap[] = {
                    { kAlutechAT4N,  160, 190, true },
                    { kGateTX,       48,  56, false },
                    { kDoorHan,      120, 150, true },
                    { kSMC5326,      50,  58, false },
                    { kHormann,      130, 170, true },
                    { kMarantec,     100, 130, true },
                    { kKiaHyundai,   110, 140, true },
                    { kMegaCode,     40,  50, false },
                    { kAprimatic,    140, 170, true },
                    { kIronLogic,    140, 170, true },
                    { kSommer,       120, 160, true },
                    { kMutancode,    130, 170, true },
                    { kMHouse,       140, 180, true },
                    { kUNILARM,      30,  40, false },
                    { kiDo,          100, 140, true },
                    { kHOLTEK,       48,  56, false },
                };

                for (size_t i = 0; i < sizeof(edgeMap)/sizeof(edgeMap[0]); i++) {
                    if (transitions >= edgeMap[i].minEdges && transitions <= edgeMap[i].maxEdges) {
                        kd->type = edgeMap[i].type;
                        // For static protocols, try to decode bits using the specific brute protocol
                        if (!edgeMap[i].rolling) {
                            const BruteProtocol* proto = nullptr;
                            switch (edgeMap[i].type) {
                                case kGateTX:   proto = &protoGateTX; break;
                                case kSMC5326:  proto = &protoSMC5326; break;
                                case kMegaCode: proto = &protoMegaCode; break;
                                case kUNILARM:  proto = &protoUNILARM; break;
                                case kHOLTEK:   proto = &protoHoltek; break;
                                default: break;
                            }
                            if (proto) {
                                uint64_t key = 0;
                                int bits = decodeFixedCodeProtocol(raw, transitions, proto, &key);
                                if (bits > 0) {
                                    kd->bitLength = bits;
                                    kd->codeLenth = bits;
                                    packKeyBits(key, bits, kd->keyID);
                                }
                            }
                        }
                        matched = true;
                        break;
                    }
                }
            }

            // If still not matched, fall back to the coarse transition-count guess
            if (!matched) {
                if (transitions >= 129 && transitions <= 137) kd->type = kStarLine;
                else if (transitions >= 133 && transitions <= 137) kd->type = kKeeLoq;
                else if (transitions >= 40 && transitions <= 60) kd->type = kCAME;
            }
        }

        validKeyReceived = true;
        rfLedSet(RF_LED_CAPTURED);
        drawKeyScreen(kd, "", false, true);
        rfUiFlashCapture();
    }
    rcswitch.resetAvailable();
}


//  Frequency stepping

static void stepFrequency(int step) {
    freqIndex = (freqIndex + step + numFrequencies) % numFrequencies;
    frequency = frequencies[freqIndex];
}


//  RCSwitch sending (from subghz.cpp)

static void RCSwitch_send(uint64_t data, unsigned int bits, int pulse, int protocol, int repeat) {
    RCSwitch mySwitch = RCSwitch();
    int txPin = CC1101_GDO0;
    mySwitch.enableTransmit(txPin);
    mySwitch.setProtocol(protocol);
    if (pulse) mySwitch.setPulseLength(pulse);
    mySwitch.setRepeatTransmit(repeat > 6 ? 6 : repeat);
    mySwitch.send(data, bits);
    mySwitch.disableTransmit();
}

static void RCSwitch_RAW_send(int *ptrtransmittimings) {
    int nTransmitterPin = CC1101_GDO0;
    if (!ptrtransmittimings) return;
    for (int nRepeat = 0; nRepeat < 1; nRepeat++) {
        unsigned int currenttiming = 0;
        bool level = true;
        while (ptrtransmittimings[currenttiming]) {
            int dur = ptrtransmittimings[currenttiming];
            bool lvl = (dur >= 0);
            unsigned int t = abs(dur);
            digitalWrite(nTransmitterPin, lvl ? HIGH : LOW);
            rawPlaybackDelayMicroseconds(t);
            currenttiming++;
        }
        digitalWrite(nTransmitterPin, LOW);
        rawPlaybackDelayMicroseconds(8000);
    }
}


//  Fixed-code protocol TX/RX helpers (CAME/NICE/HOLTEK-style)

// These reuse the same BruteProtocol pulse tables the brute-forcer already
// transmits with (protoCame/protoNice/protoHoltek), so the timings are the
// same ones already exercised on real hardware there - this just drives
// them from a captured key instead of a counter, and adds the matching
// decode side so Read/Read RAW can recognize these protocols by their
// actual pulse timing rather than guessing from transition count alone.

// Packs the low `bits` bits of `value` into keyID[8], MSB-first, matching
// the exact layout rfSendKey()/read_rcswitch() already use elsewhere
// (keyID[7] = LSB byte, working backwards) so all code paths stay
// interchangeable.
static void packKeyBits(uint64_t value, int bits, uint8_t keyID[8]) {
    memset(keyID, 0, 8);
    int numBytes = (bits + 7) / 8;
    if (numBytes > 8) numBytes = 8;
    for (int i = 0; i < numBytes; i++) {
        keyID[7 - i] = (uint8_t)((value >> (i * 8)) & 0xFF);
    }
}

static void sendTimingSymbol(const int* seq, size_t len) {
    if (!seq || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        int duration = seq[i];
        digitalWrite(CC1101_GDO0, duration > 0 ? HIGH : LOW);
        delayMicroseconds((unsigned int)abs(duration));
    }
}

// Bit-bangs `key` (bitLength bits, MSB first - same order bruteSendCode()
// already uses) through a fixed-code protocol's pilot/zero/one/stop
// pulse table.
static bool sendFixedCodeProtocol(const BruteProtocol* p, uint64_t key, int bitLength, int repeat) {
    if (!p || bitLength <= 0 || bitLength > 64) return false;
    pinMode(CC1101_GDO0, OUTPUT);
    if (repeat < 1) repeat = 1;
    if (repeat > 10) repeat = 10;
    for (int r = 0; r < repeat; r++) {
        sendTimingSymbol(p->pilot, p->pilotLen);
        for (int bit = bitLength - 1; bit >= 0; --bit) {
            bool set = (key >> bit) & 0x1;
            sendTimingSymbol(set ? p->one : p->zero, set ? p->oneLen : p->zeroLen);
        }
        sendTimingSymbol(p->stop, p->stopLen);
    }
    digitalWrite(CC1101_GDO0, LOW);
    return true;
}

// Tries to match a captured raw edge buffer against a fixed-code protocol
// template within tolerance, and if it fits, decodes the actual data bits
// (not just a transition-count guess). Returns the decoded bit count, or 0
// on no match. NOTE: this is a best-effort timing match against the same
// pulse tables the brute-forcer uses on hardware - real remotes vary
// slightly in timing, so treat a match as "likely this protocol", and
// confirm against a real receiver/remote before relying on it.
static int decodeFixedCodeProtocol(const unsigned int* raw, int transitions, const BruteProtocol* p, uint64_t* outKey) {
    if (!raw || !p || !outKey || transitions < 8) return 0;

    auto durMatches = [](unsigned int measured, int expected) -> bool {
        int e = abs(expected);
        int tol = e / 4 + 60; // ~25% tolerance plus a fixed floor for jitter
        int diff = abs((int)measured - e);
        return diff <= tol;
    };

    int idx = 0;
    if (p->pilotLen > 0) {
        if (transitions < (int)p->pilotLen) return 0;
        for (size_t i = 0; i < p->pilotLen; i++) {
            if (!durMatches(raw[idx + i], p->pilot[i])) return 0;
        }
        idx += (int)p->pilotLen;
    }

    uint64_t key = 0;
    int bits = 0;
    while (bits < 64) {
        bool isZero = (idx + (int)p->zeroLen <= transitions);
        for (size_t i = 0; isZero && i < p->zeroLen; i++) {
            if (!durMatches(raw[idx + i], p->zero[i])) isZero = false;
        }
        if (isZero) { key = (key << 1) | 0; idx += (int)p->zeroLen; bits++; continue; }

        bool isOne = (idx + (int)p->oneLen <= transitions);
        for (size_t i = 0; isOne && i < p->oneLen; i++) {
            if (!durMatches(raw[idx + i], p->one[i])) isOne = false;
        }
        if (isOne) { key = (key << 1) | 1; idx += (int)p->oneLen; bits++; continue; }

        break; // hit the stop symbol, trailing noise, or end of buffer
    }

    // A real fixed-code remote is at least a byte's worth of data; anything
    // shorter is almost certainly a coincidental partial match, not a hit.
    if (bits < 8) return 0;
    *outKey = key;
    return bits;
}


//  Send key (synth)

// BUGFIX (history): this used to fall through to a generic Princeton-style
// RCSwitch_send() for every protocol it didn't specifically handle, then
// returned true regardless - so the UI reported "Sent" even when the
// timing on air didn't match the source device and the remote ignored it.
// Below, every protocol is either encoded with timings that actually match
// it, or explicitly refused - never silently faked.
//
// Coverage:
//  - Princeton, CAME: encoded via the RCSwitch library's own protocol
//    table (unchanged from before - already correct).
//  - NICE, HOLTEK, Chamberlain, Ansonic, Linear Delta-3: fixed-code, all
//    natively encoded via sendFixedCodeProtocol() using pulse tables
//    (protoNice/protoHoltek/protoChamber/protoAnsonic/protoLinearDelta3).
//    Chamberlain/Ansonic tables were already present for the brute-forcer
//    but previously unused for read/send. Linear Delta-3's table is new
//    and, unlike the others, has not been validated against real
//    hardware - see the comment by protoLinearDelta3's definition.
//  - RcSwitch (generic capture): now replayed with the *exact* RCSwitch
//    protocol number captured at Read time (kd->rcProtocolNum), instead of
//    always guessing protocol 1. Older files saved before this field
//    existed have rcProtocolNum == 0 and fall back to the previous
//    best-effort behavior.
//  - KeeLoq, StarLine, Somfy RTS (Telis/Keytis), Nice FloR-S, Security+1.0,
//    Security+2.0, FAAC SLH, BFT Mitto, CAME Atomo, AN-Motors, HCS101: all
//    refused via rfIsRollingCode(). These are rolling-code protocols - a
//    real transmitter must advance an encrypted counter each time, so a
//    static saved file can never be validly "replayed" on any device, not
//    just this one. There is no encoding fix for that; re-capturing the
//    file doesn't change it. All of these are recognized only when
//    loading a .sub file that already carries that Protocol: name (e.g.
//    captured by a real Flipper) - this codebase has no live RAW-capture
//    heuristic for any of them, unlike the fixed-code protocols above.
//
// BUGFIX: protocol name matching against the Protocol: line in .sub files
// is now case/spacing-normalized (see rfProtocolTypeFromName()) instead of
// exact-string. Real Flipper firmware writes "Holtek", "Star Line" (with a
// space), "Nice Flo"/"Nice FloR-S", and "LinearDelta3" - none of which
// matched this codebase's original literal comparisons ("HOLTEK",
// "StarLine", "NICE", "Linear Delta-3"), so files using those already-
// supported protocols were falling through to kUnknown and getting
// refused as "not supported" even though the send path for them worked
// fine once correctly identified.
static bool rfIsRollingCode(emKeys type) {
    switch (type) {
        case kKeeLoq:
        case kStarLine:
        case kSomfyRTS:
        case kNiceFlorS:
        case kSecPlusV1:
        case kSecPlusV2:
        case kFaacSLH:
        case kBftMitto:
        case kCameAtomo:
        case kAnMotors:
        case kHcs101:
        // --- NEW rolling protocols ---
        case kAlutechAT4N:
        case kDoorHan:
        case kHormann:
        case kMarantec:
        case kKiaHyundai:
        case kAprimatic:
        case kIronLogic:
        case kSommer:
        case kMutancode:
        case kMHouse:
        case kiDo:
            return true;
        default:
            return false;
    }
}

static bool rfKeyIdIsAllZero(const tpKeyData* kd) {
    for (int i = 0; i < 8; i++) if (kd->keyID[i] != 0) return false;
    return true;
}

bool rfSendKey(tpKeyData* kd) {
    String protocol = getTypeName((emKeys)kd->type);

    if (protocol == "RAW") return false;

    if (rfIsRollingCode((emKeys)kd->type)) {
        drawError(protocol + ": rolling code", true);
        tft.setCursor(10, 76);
        tft.print("Cannot be replayed from a file");
        delay(1400);
        return false;
    }

    // If it's unknown or a zero-key CAME with raw data, replay raw
    if ((kd->type == kUnknown || (kd->type == kCAME && rfKeyIdIsAllZero(kd))) && kd->rawData[0] != '\0') {
        if (!initRfModule("tx", kd->frequency)) {
            drawError("CC1101 TX init fail", true);
            delay(800);
            return false;
        }
        bool ok = sendRawBlock(String(kd->rawData));
        deinitRfModule();
        return ok;
    }

    // --- Allow all static protocols ---
    if (kd->type != kPrinceton && kd->type != kCAME &&
        kd->type != kNICE && kd->type != kHOLTEK && kd->type != kRcSwitch &&
        kd->type != kChamberlain && kd->type != kAnsonic && kd->type != kLinearDelta3 &&
        kd->type != kGateTX && kd->type != kSMC5326 && kd->type != kMegaCode && kd->type != kUNILARM) {
        drawError(protocol + ": not supported", true);
        tft.setCursor(10, 76);
        tft.print("Re-capture as RAW to send this");
        delay(1400);
        return false;
    }

    if (!initRfModule("tx", kd->frequency)) {
        drawError("CC1101 TX init fail", true);
        delay(800);
        return false;
    }

    uint64_t key = 0;
    for (int i = 0; i < 8; i++) {
        key |= ((uint64_t)kd->keyID[i] << ((7 - i) * 8));
    }

    bool ok = false;
    switch (kd->type) {
        case kPrinceton:
            RCSwitch_send(key, kd->bitLength, 350, 1, 6);
            ok = true;
            break;
        case kCAME:
            RCSwitch_send(key, kd->bitLength, 270, 11, 6);
            ok = true;
            break;
        case kNICE:
            ok = sendFixedCodeProtocol(&protoNice, key, kd->bitLength, 6);
            break;
        case kHOLTEK:
            ok = sendFixedCodeProtocol(&protoHoltek, key, kd->bitLength, 6);
            break;
        case kChamberlain:
            ok = sendFixedCodeProtocol(&protoChamber, key, kd->bitLength, 6);
            break;
        case kAnsonic:
            ok = sendFixedCodeProtocol(&protoAnsonic, key, kd->bitLength, 6);
            break;
        case kLinearDelta3:
            ok = sendFixedCodeProtocol(&protoLinearDelta3, key, kd->bitLength, 10);
            break;
        case kGateTX:
            ok = sendFixedCodeProtocol(&protoGateTX, key, kd->bitLength, 6);
            break;
        case kSMC5326:
            ok = sendFixedCodeProtocol(&protoSMC5326, key, kd->bitLength, 6);
            break;
        case kMegaCode:
            ok = sendFixedCodeProtocol(&protoMegaCode, key, kd->bitLength, 6);
            break;
        case kUNILARM:
            ok = sendFixedCodeProtocol(&protoUNILARM, key, kd->bitLength, 6);
            break;
        case kRcSwitch: {
            int pulse = kd->te > 0 ? kd->te : 350;
            int protocolNum = kd->rcProtocolNum > 0 ? kd->rcProtocolNum : 1;
            RCSwitch_send(key, kd->bitLength, pulse, protocolNum, 6);
            ok = true;
            break;
        }
        default:
            ok = false;
            break;
    }

    deinitRfModule();
    return ok;
}

static void sendSynthKey(tpKeyData* kd) {
    rfSendKey(kd);
}


//  RAW playback (from subghz.cpp)

static bool playRawRecorderFile(const String& fileName) {
    rawPlaybackActive = false;
    rawPlaybackStopRequestedFlag = false;
    rawPlaybackOkWasPressed = false;
    rawPlaybackBackWasPressed = false;
    rawPlaybackBackPressedAt = 0;
    rawPlaybackIgnoreOkRelease = false;
    rawPlaybackIgnoreBackRelease = false;
    rawPlaybackShownPct = 0.0f;
    rawPlaybackTargetPct = 0.0f;
    rawPlaybackStartMs = millis();
    rawPlaybackLastDrawMs = 0;

    if (fileName.length() == 0 || !SD.exists(fileName)) {
        drawError("No RAW file", true);
        delay(600);
        return false;
    }

    disableRcSwitchReceive();
    rawPlaybackActive = true;
    rawPlaybackIgnoreOkRelease = false;
    rawPlaybackIgnoreBackRelease = false;
    drawRawPlaybackWave(0, 0);
    startRawPlaybackAnimationTask();

    if (!initRfModule("tx", frequency)) {
        stopRawPlaybackAnimationTask();
        rawPlaybackActive = false;
        drawError("TX init fail", true);
        delay(800);
        restoreReceiveMode();
        return false;
    }

    ELECHOUSE_cc1101.setModulation(2);
    ELECHOUSE_cc1101.setRxBW(270.0);
    ELECHOUSE_cc1101.setDeviation(0);
    ELECHOUSE_cc1101.setDRate(10);
    ELECHOUSE_cc1101.setPA(12);
    ELECHOUSE_cc1101.SetTx();
    pinMode(CC1101_GDO0, OUTPUT);

    File rawFile = SD.open(fileName, FILE_READ);
    if (!rawFile) {
        stopRawPlaybackAnimationTask();
        rawPlaybackActive = false;
        deinitRfModule();
        restoreReceiveMode();
        drawError("Open failed", true);
        delay(600);
        return false;
    }

    int sentBlocks = 0;
    const uint32_t rawFileSize = rawFile.size();
    bool foundRawBlock = false;

    while (rawFile.available()) {
        if (rawPlaybackStopRequested()) break;
        String line = rawFile.readStringUntil('\n');
        line.trim();
        if (!line.startsWith("RAW_Data:")) continue;
        String block = line.substring(9);
        block.trim();
        foundRawBlock = true;
        float targetPct = rawFileSize > 0 ? ((float)rawFile.position() * 100.0f / rawFileSize) : 100.0f;
        if (targetPct < 1.0f) targetPct = 1.0f;
        rawPlaybackTargetPct = targetPct;
        // BUGFIX: pumpRawPlaybackAnimation() existed but nothing ever
        // called it - the "Transmitting" screen was drawn once at 0%
        // before this loop and then never redrawn until everything had
        // already finished, so the wave/progress bar just sat frozen for
        // the whole transmission. Redraw between blocks (not mid-block,
        // so the RF bit-banging timing in RCSwitch_RAW_send isn't
        // disturbed by display SPI traffic).
        pumpRawPlaybackAnimation(true);
        if (sendRawBlock(block)) sentBlocks++;
    }
    rawFile.close();

    if (!rawPlaybackStopRequestedFlag && foundRawBlock) {
        rawPlaybackTargetPct = 100.0f;
        finishRawPlaybackAnimation(180);
    }

    stopRawPlaybackAnimationTask();
    armRawPlaybackReleaseGuards();
    deinitRfModule();
    restoreReceiveMode();

    return sentBlocks > 0;
}

static bool sendRawBlock(const String& block) {
    // BUGFIX: this used to strtok() directly on block.c_str(), which
    // mutates the String's internal buffer through a const pointer -
    // undefined behavior that could corrupt the String or crash mid-
    // transmit. It was also capped at 512 timings while the recorder can
    // capture up to RAW_REC_MAX_EDGES (900) edges per frame, silently
    // truncating (and thus never fully sending) longer captures.
    size_t len = block.length();
    if (len == 0) return false;
    std::vector<char> mutableBlock(len + 1);
    memcpy(mutableBlock.data(), block.c_str(), len + 1);

    const int maxTimings = RAW_REC_MAX_EDGES + 1;
    std::vector<int> transmittimings(maxTimings, 0);
    int count = 0;
    char* p = strtok(mutableBlock.data(), " ");
    while (p && count < maxTimings - 1) {
        transmittimings[count++] = atoi(p);
        p = strtok(nullptr, " ");
    }
    if (count <= 0) return false;
    transmittimings[count] = 0;
    RCSwitch_RAW_send(transmittimings.data());
    return true;
}

static void drawRawPlaybackWave(float phase, uint8_t progressPct) {
    tft.fillScreen(UI_BG);
    int w = tft.width();
    int h = tft.height();
    rfUiHeader("Transmitting", String(frequency, 2) + " MHz", UI_RED);

    tft.fillRoundRect(5, 30, w - 10, 90, 8, UI_PANEL);
    tft.drawRoundRect(5, 30, w - 10, 90, 8, UI_BORDER);
    for (int i = 0; i < w - 20; i++) {
        int y = 75 + 28 * sin(phase + i * 0.12);
        tft.drawPixel(10 + i, y, UI_CYAN);
    }

    rfUiProgressBar(10, 128, w - 20, 12, progressPct, UI_RED, UI_BORDER);
    tft.setTextColor(UI_TEXT_DIM);
    tft.setTextSize(1);
    String pctTxt = String(progressPct) + "%";
    tft.setCursor((w - tft.textWidth(pctTxt)) / 2, 144);
    tft.print(pctTxt);

    int btnW = 70, btnH = 24;
    int btnX = w / 2 - btnW / 2, btnY = h - btnH - 6;
    tft.fillRoundRect(btnX, btnY, btnW, btnH, 6, UI_RED);
    tft.setTextColor(UI_BG);
    tft.setCursor(btnX + (btnW - tft.textWidth("Stop")) / 2, btnY + 7);
    tft.print("Stop");
}

static bool rawPlaybackStopRequested() {
    int evt = buttonsPoll();
    if (evt == EVT_BACK || evt == EVT_OK) {
        rawPlaybackStopRequestedFlag = true;
    }
    return rawPlaybackStopRequestedFlag;
}

static void rawPlaybackDelayMicroseconds(unsigned int durationUs) {
    while (durationUs > 0) {
        if (rawPlaybackActive && rawPlaybackStopRequested()) return;
        const unsigned int slice = durationUs > 500 ? 500 : durationUs;
        delayMicroseconds(slice);
        durationUs -= slice;
    }
}

static void startRawPlaybackAnimationTask() {
    // For simplicity, we'll just pump in the main loop.
}

static void stopRawPlaybackAnimationTask() {
    rawPlaybackActive = false;
}

static void pumpRawPlaybackAnimation(bool force) {
    if (!rawPlaybackActive) return;
    unsigned long now = millis();
    if (!force && now - rawPlaybackLastDrawMs < 18) return;
    rawPlaybackLastDrawMs = now;
    float delta = rawPlaybackTargetPct - rawPlaybackShownPct;
    rawPlaybackShownPct += delta * 0.16f;
    if (fabs(delta) < 0.35f) rawPlaybackShownPct = rawPlaybackTargetPct;
    float phase = (float)(now - rawPlaybackStartMs) * 0.1f;
    drawRawPlaybackWave(phase, (uint8_t)(rawPlaybackShownPct + 0.5f));
}

static void armRawPlaybackReleaseGuards() {
    // Not needed
}

static void finishRawPlaybackAnimation(uint16_t durationMs) {
    // BUGFIX: this used to just delay() without drawing anything, so the
    // "settle to 100%" tail sat on the same frozen frame as everything
    // else. Now it actually eases rawPlaybackShownPct up to 100 and
    // redraws each tick like the rest of the animation does.
    unsigned long start = millis();
    while (!rawPlaybackStopRequested() && millis() - start < durationMs) {
        pumpRawPlaybackAnimation(true);
        delay(18);
    }
    pumpRawPlaybackAnimation(true); // final frame, guaranteed to show 100%
}


//  Brute force (from subghz.cpp)

static void drawBruteIntroStatic() {
    tft.fillScreen(TFT_BLACK);
    rfUiHeader("Bruteforce");
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 50);
    tft.print("Press OK to configure");
    tft.setCursor(10, 70);
    tft.print("Then OK to start attack");
    tft.setCursor(10, 90);
    tft.print("BACK to exit");
}

static void updateBruteIntroAnimation() {
    // Only the spinner at the top-right corner animates
    const int cx = tft.width() - 26;
    const int cy = 12;
    const int R  = 12;

    tft.fillRect(cx - R, cy - R, R * 2, R * 2, UI_BG);

    float phase = rfUiPhase(0.003f);
    rfUiSpinner(cx, cy, 11, phase, TFT_CYAN);
}

static void drawBruteIntro() {
    drawBruteIntroStatic();
    updateBruteIntroAnimation();
}

static void drawBruteConfig() {
    tft.fillScreen(TFT_BLACK);
    rfUiHeader("Brute Config");
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.setCursor(10, 40);
    tft.print("Type: ");
    tft.setTextColor(TFT_YELLOW);
    tft.print(bruteTypes[bruteTypeIndex]);
    tft.setTextColor(TFT_WHITE);
    tft.print("-");
    tft.print(String(bruteBitsArray[bruteTypeIndex]) + "bit");
    tft.setCursor(10, 60);
    tft.print("Freq: ");
    tft.setTextColor(TFT_CYAN);
    tft.print(bruteFreqLabels[bruteFreqIndex]);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(10, 90);
    tft.print("UP/DN: Type   L/R: Freq");
    tft.setCursor(10, 105);
    tft.print("OK: Start   BACK: Cancel");
}

static void drawBruteProgressStatic(uint16_t total) {
    tft.fillScreen(TFT_BLACK);
    rfUiHeader("Bruteforce", bruteFreqLabels[bruteFreqIndex], TFT_RED);
    // Draw the progress bar outline once
    rfUiProgressBar(20, 75, tft.width() - 40, 16, 0, TFT_GREEN);
}

static void updateBruteProgress(uint16_t progress, uint16_t total) {
    // Only clear and redraw the number, bar fill, and TX blink

    // Clear the counter text region
    tft.fillRect(0, 40, tft.width(), 30, TFT_BLACK);
    char buf[32];
    snprintf(buf, sizeof(buf), "%u/%u", progress, total);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setCursor(tft.width()/2 - tft.textWidth(buf)/2, 40);
    tft.print(buf);

    // Redraw the progress bar fill
    uint16_t percent = (total == 0) ? 0 : (progress * 100) / total;
    static float shownPct = 0.0f;
    if (progress == 0) shownPct = 0.0f;
    rfUiEaseTowards(shownPct, (float)percent, 0.35f, 0.5f);
    rfUiProgressBar(20, 75, tft.width() - 40, 16, shownPct, TFT_GREEN);

    // Percentage text
    tft.fillRect(0, 96, tft.width(), 16, TFT_BLACK);
    tft.setTextSize(1);
    String pctTxt = String(percent) + "%";
    tft.setCursor((tft.width() - tft.textWidth(pctTxt)) / 2, 96);
    tft.print(pctTxt);

    // TX blink
    tft.fillRect(0, 116, tft.width(), 16, TFT_BLACK);
    if (bruteRunning && rfUiBlinkOn(250)) {
        tft.setTextColor(TFT_RED);
        String txt = "TX";
        tft.setCursor((tft.width() - tft.textWidth(txt)) / 2, 120);
        tft.print(txt);
    }
}

// Backward-compatible entry
static void drawBruteProgress(uint16_t progress, uint16_t total) {
    if (progress == 0) drawBruteProgressStatic(total);
    updateBruteProgress(progress, total);
}

static const BruteProtocol* getBruteProtocolByIndex(int idx) {
    switch (idx) {
        case 0: return &protoCame;
        case 1: return &protoNice;
        case 2: return &protoAnsonic;
        case 3: return &protoHoltek;
        case 4: return &protoChamber;
        case 5: return &protoGateTX;
        case 6: return &protoSMC5326;
        case 7: return &protoMegaCode;
        case 8: return &protoUNILARM;
        default: return nullptr;
    }
}

static bool bruteInitTx() {
    currentBruteProto = getBruteProtocolByIndex(bruteTypeIndex);
    if (!currentBruteProto) return false;
    frequency = bruteFreqOptions[bruteFreqIndex];
    bruteTxPin = CC1101_GDO0;
    if (!initRfModule("tx", frequency)) return false;
    pinMode(bruteTxPin, OUTPUT);
    digitalWrite(bruteTxPin, LOW);
    bruteRfActive = true;
    return true;
}

static void bruteStopTx() {
    if (!bruteRfActive) return;
    digitalWrite(bruteTxPin, LOW);
    deinitRfModule();
    bruteRfActive = false;
}

static void bruteSendSequence(const int* seq, size_t len) {
    if (!seq || len == 0) return;
    for (size_t i = 0; i < len; i++) {
        int duration = seq[i];
        bool levelHigh = duration > 0;
        unsigned int delayVal = abs(duration);
        digitalWrite(bruteTxPin, levelHigh ? HIGH : LOW);
        delayMicroseconds(delayVal);
    }
}

static void bruteSendCode(uint16_t code) {
    if (!bruteRfActive || !currentBruteProto) return;
    const BruteProtocol* p = currentBruteProto;
    bruteSendSequence(p->pilot, p->pilotLen);
    for (int bit = bruteBits - 1; bit >= 0; --bit) {
        bool set = (code >> bit) & 0x1;
        bruteSendSequence(set ? p->one : p->zero, set ? p->oneLen : p->zeroLen);
    }
    bruteSendSequence(p->stop, p->stopLen);
    digitalWrite(bruteTxPin, LOW);
}


//  Analyzer (from subghz.cpp)

static void analyzerInit() {
    for (uint8_t i = 0; i < ANALYZER_HIST_CNT; i++) {
        analyzerState.hist_freq[i] = 0;
        analyzerState.hist_count[i] = 0;
        analyzerState.hist_peak_rssi[i] = ANALYZER_RSSI_LOW;
    }
    analyzerState.curr_freq = 0;
    analyzerState.saved_freq = 0;
    analyzerState.rssi_now = 0.0f;
    analyzerState.last_rssi = 0.0f;
    analyzerState.has_signal = false;
    analyzerState.threshold = ANALYZER_DEFAULT_TRIG;
    analyzerState.noise_floor = ANALYZER_RSSI_LOW;
    analyzerFilterVal = 0.0f;
    analyzerTrigLevel = ANALYZER_DEFAULT_TRIG;
    analyzerHoldCount = 0;
    analyzerLocked = false;
    analyzerLastDraw = 0;
    analyzerSaveFlashUntil = 0;
    ELECHOUSE_cc1101.SpiStrobe(0x36);
    delayMicroseconds(200);
    ELECHOUSE_cc1101.setRxBW(812.0);
    ELECHOUSE_cc1101.SetRx();

    // Auto-calibrate the squelch/trigger threshold to the ambient RF
    // noise floor instead of relying on one fixed value for every
    // environment. A quiet room can safely use a much lower trigger
    // (catches weaker signals); a noisy urban environment needs a
    // higher one to avoid constantly false-locking onto background
    // noise. Runs a quick sparse sample (a handful of frequencies, not
    // the whole table) so it doesn't add noticeable delay on entry.
    float floor = analyzerCalibrateNoiseFloor();
    analyzerState.noise_floor = floor;
    analyzerTrigLevel = floor + ANALYZER_LOCK_MARGIN_DB;
    if (analyzerTrigLevel < ANALYZER_RSSI_LOW) analyzerTrigLevel = ANALYZER_RSSI_LOW;
    if (analyzerTrigLevel > ANALYZER_RSSI_HIGH) analyzerTrigLevel = ANALYZER_RSSI_HIGH;
}

static uint32_t analyzerSmoothAvg(uint32_t newVal) {
    float newFloat = (float)newVal;
    float mix = (fabsf(newFloat - analyzerFilterVal) > 500000.0f) ? 0.9f : 0.03f;
    analyzerFilterVal += (newFloat - analyzerFilterVal) * mix;
    return (uint32_t)analyzerFilterVal;
}

static uint32_t analyzerNearestFreq(uint32_t input) {
    uint32_t prev = 0, out = 0;
    for (int i = 0; i < ANALYZER_FREQ_COUNT; i++) {
        uint32_t cur = (uint32_t)(analyzerFreqs[i] * 1000000.0f);
        if (cur == 0) continue;
        if (cur == input) return cur;
        if (cur > input && prev < input) {
            out = (cur - input < input - prev) ? cur : prev;
            break;
        }
        prev = cur;
    }
    if (!out && prev) out = prev;
    return out;
}

static void analyzerAddHistory(uint32_t freq, float rssi) {
    uint32_t normFreq = analyzerNearestFreq(freq);
    if (!normFreq) return;
    bool found = false;
    for (uint8_t i = 0; i < ANALYZER_HIST_CNT; i++) {
        if (analyzerState.hist_freq[i] != normFreq) continue;
        found = true;
        if (analyzerState.hist_count[i] == 0) analyzerState.hist_count[i] = 1;
        else analyzerState.hist_count[i]++;
        if (rssi > analyzerState.hist_peak_rssi[i]) analyzerState.hist_peak_rssi[i] = rssi;
        if (i > 0) {
            uint32_t f = analyzerState.hist_freq[i];
            uint8_t c = analyzerState.hist_count[i];
            float pk = analyzerState.hist_peak_rssi[i];
            for (int j = i; j > 0; j--) {
                analyzerState.hist_freq[j] = analyzerState.hist_freq[j - 1];
                analyzerState.hist_count[j] = analyzerState.hist_count[j - 1];
                analyzerState.hist_peak_rssi[j] = analyzerState.hist_peak_rssi[j - 1];
            }
            analyzerState.hist_freq[0] = f;
            analyzerState.hist_count[0] = c;
            analyzerState.hist_peak_rssi[0] = pk;
        }
        break;
    }
    if (!found) {
        for (int i = ANALYZER_HIST_CNT - 1; i > 0; i--) {
            analyzerState.hist_freq[i] = analyzerState.hist_freq[i - 1];
            analyzerState.hist_count[i] = analyzerState.hist_count[i - 1];
            analyzerState.hist_peak_rssi[i] = analyzerState.hist_peak_rssi[i - 1];
        }
        analyzerState.hist_freq[0] = normFreq;
        analyzerState.hist_count[0] = 1;
        analyzerState.hist_peak_rssi[0] = rssi;
        // Only log genuinely new frequencies (not repeat hits climbing
        // back to the top of the list) to keep the CSV meaningful instead
        // of spamming a line every scan cycle.
        analyzerLogFrequency(normFreq, rssi);
    }
}

static bool analyzerSaveFreq() {
    if (!analyzerState.curr_freq) return false;
    uint32_t saveFreq = analyzerNearestFreq(analyzerState.curr_freq);
    if (!saveFreq) return false;
    analyzerState.saved_freq = saveFreq;
    return true;
}

// Returns true = keep scanning, false = exit analyzer (BACK pressed).
// NOTE: previously this returned false whenever no button event was
// present (the common case), which made analyzerDoScan() bail out on
// its very first loop iteration almost every call - the analyzer never
// actually scanned. Only BACK should stop the scan loop now.
static bool analyzerHandleInput() {
    int evt = buttonsPoll();
    if (evt == EVT_BACK) {
        analyzerExitRequested = true;
        return false;
    }
    if (evt == EVT_DOWN) {
        analyzerTrigLevel -= ANALYZER_TRIG_STEP;
        if (analyzerTrigLevel < ANALYZER_RSSI_LOW) analyzerTrigLevel = ANALYZER_RSSI_LOW;
    } else if (evt == EVT_UP) {
        if (analyzerTrigLevel == ANALYZER_RSSI_LOW) analyzerTrigLevel = -95.0f;
        else analyzerTrigLevel += ANALYZER_TRIG_STEP;
        if (analyzerTrigLevel > ANALYZER_RSSI_HIGH) analyzerTrigLevel = ANALYZER_RSSI_HIGH;
    } else if (evt == EVT_OK) {
        if (analyzerSaveFreq()) {
            analyzerSaveFlashUntil = millis() + 700;
        }
    }
    return true;
}

static bool analyzerIsNoiseFreq(uint32_t freqHz) {
    for (int i = 0; i < ANALYZER_NOISE_BAND_COUNT; i++) {
        if (freqHz >= analyzerNoiseBands[i].loHz && freqHz <= analyzerNoiseBands[i].hiHz) {
            return true;
        }
    }
    return false;
}

// Samples RSSI at a sparse subset of the monitored frequencies (skipping
// known noise bands) and averages them to estimate the ambient RF noise
// floor in the current environment. Used by analyzerInit() to set a
// sensible default trigger threshold. Deliberately samples only ~8 points
// rather than the full table so it stays fast enough to run on every
// analyzer entry without a visible pause.
static float analyzerCalibrateNoiseFloor() {
    ELECHOUSE_cc1101.SpiStrobe(0x36);
    ELECHOUSE_cc1101.setRxBW(812.0);
    const int step = (ANALYZER_FREQ_COUNT > 8) ? (ANALYZER_FREQ_COUNT / 8) : 1;
    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < ANALYZER_FREQ_COUNT; i += step) {
        uint32_t freqHz = (uint32_t)(analyzerFreqs[i] * 1000000.0f);
        if (analyzerIsNoiseFreq(freqHz)) continue;
        ELECHOUSE_cc1101.setMHZ(analyzerFreqs[i]);
        ELECHOUSE_cc1101.SetRx();
        delayMicroseconds(ANALYZER_STEP_DELAY_US);
        sum += ELECHOUSE_cc1101.getRssi();
        count++;
    }
    return (count > 0) ? (sum / count) : ANALYZER_DEFAULT_TRIG;
}

// Appends a newly-discovered frequency to a persistent CSV log on SD, so
// the on-screen 4-slot recent-frequency list isn't the only record of
// what the analyzer has found. Uses millis() as a lightweight timestamp
// since this project has no RTC; swap for a real timestamp if one is
// ever added. Failures are silent - logging is a convenience, not
// something that should ever block or error out the analyzer itself.
static void analyzerLogFrequency(uint32_t freqHz, float rssi) {
    if (!SD.exists("/subghz")) SD.mkdir("/subghz");
    bool isNewFile = !SD.exists("/subghz/analyzer_log.csv");
    File f = SD.open("/subghz/analyzer_log.csv", FILE_APPEND);
    if (!f) return;
    if (isNewFile) f.println("uptime_ms,freq_hz,rssi_dbm");
    f.print(millis());
    f.print(",");
    f.print(freqHz);
    f.print(",");
    f.println(rssi, 1);
    f.close();
}

static void analyzerDoScan() {
    bool hadSignal = analyzerState.has_signal;
    uint32_t prevFreq = analyzerState.curr_freq;
    analyzerScanResult.rough_rssi = -127.0f;
    analyzerScanResult.fine_rssi = -127.0f;

    ELECHOUSE_cc1101.SpiStrobe(0x36);
    ELECHOUSE_cc1101.setRxBW(812.0);

    for (int i = 0; i < ANALYZER_FREQ_COUNT; i++) {
        if (!analyzerHandleInput()) return;
        uint32_t freqHz = (uint32_t)(analyzerFreqs[i] * 1000000.0f);
        if (freqHz == 462750000 || freqHz == 467750000 || freqHz == 464000000 || freqHz > 920000000) continue;
        if (analyzerIsNoiseFreq(freqHz)) continue;
        ELECHOUSE_cc1101.setMHZ(analyzerFreqs[i]);
        ELECHOUSE_cc1101.SetRx();
        delayMicroseconds(ANALYZER_STEP_DELAY_US);
        float rssi = ELECHOUSE_cc1101.getRssi();
        if (analyzerScanResult.rough_rssi < rssi) {
            analyzerScanResult.rough_rssi = rssi;
            analyzerScanResult.rough_freq = freqHz;
        }
    }

    if (analyzerScanResult.rough_rssi > analyzerTrigLevel && analyzerScanResult.rough_freq > 300000) {
        ELECHOUSE_cc1101.SpiStrobe(0x36);
        ELECHOUSE_cc1101.setRxBW(58.0);
        for (uint32_t freqHz = analyzerScanResult.rough_freq - 300000;
             freqHz < analyzerScanResult.rough_freq + 300000;
             freqHz += 20000) {
            if (!analyzerHandleInput()) return;
            if (analyzerIsNoiseFreq(freqHz)) continue;
            float freqMHz = freqHz / 1000000.0f;
            ELECHOUSE_cc1101.setMHZ(freqMHz);
            ELECHOUSE_cc1101.SetRx();
            delayMicroseconds(ANALYZER_STEP_DELAY_US);
            float rssi = ELECHOUSE_cc1101.getRssi();
            if (analyzerScanResult.fine_rssi < rssi) {
                analyzerScanResult.fine_rssi = rssi;
                analyzerScanResult.fine_freq = freqHz;
            }
        }
    }

    if (analyzerScanResult.fine_rssi > analyzerTrigLevel + ANALYZER_LOCK_MARGIN_DB &&
        !analyzerIsNoiseFreq(analyzerScanResult.fine_freq)) {
        analyzerHoldCount = 20;
        if (analyzerFilterVal != 0.0f) {
            analyzerScanResult.fine_freq = analyzerSmoothAvg(analyzerScanResult.fine_freq);
        }
        analyzerLocked = true;
        analyzerState.curr_freq = analyzerScanResult.fine_freq;
        analyzerState.rssi_now = analyzerScanResult.fine_rssi;
        analyzerState.last_rssi = analyzerScanResult.fine_rssi;
        analyzerState.has_signal = true;
    } else if (analyzerScanResult.rough_rssi > analyzerTrigLevel + ANALYZER_LOCK_MARGIN_DB &&
               analyzerHoldCount < 10 &&
               !analyzerIsNoiseFreq(analyzerScanResult.rough_freq)) {
        analyzerHoldCount = 20;
        if (analyzerFilterVal != 0.0f) {
            analyzerScanResult.rough_freq = analyzerSmoothAvg(analyzerScanResult.rough_freq);
        }
        analyzerLocked = true;
        analyzerState.curr_freq = analyzerScanResult.rough_freq;
        analyzerState.rssi_now = analyzerScanResult.rough_rssi;
        analyzerState.last_rssi = analyzerScanResult.rough_rssi;
        analyzerState.has_signal = true;
    } else {
        analyzerLocked = false;
        analyzerState.has_signal = false;
        analyzerState.curr_freq = 0;
        analyzerState.rssi_now = 0.0f;
        analyzerState.last_rssi = 0.0f;
        analyzerFilterVal = 0.0f;
        if (analyzerHoldCount > 0) analyzerHoldCount--;
    }

    // BUGFIX: this used to be recomputed as (rssi_now > analyzerTrigLevel).
    // With no signal, rssi_now resets to 0.0 while analyzerTrigLevel is a
    // negative dBm value (e.g. -75), so 0 > -75 was always true - the
    // analyzer reported "locked" even when nothing was found. has_signal
    // above is already the correct lock state; just carry it forward.
    analyzerLocked = analyzerState.has_signal;
    analyzerState.threshold = analyzerTrigLevel;
    if (analyzerState.has_signal) {
        if (!hadSignal) {
            analyzerAddHistory(analyzerState.curr_freq, analyzerState.rssi_now);
        } else {
            uint32_t diff = (analyzerState.curr_freq > prevFreq) ? (analyzerState.curr_freq - prevFreq) : (prevFreq - analyzerState.curr_freq);
            if (diff >= 300000UL) analyzerAddHistory(analyzerState.curr_freq, analyzerState.rssi_now);
        }
    }
}

// Maps an RSSI value (dBm) to a 0.0-1.0 fraction of the display range,
// clamped to the analyzer's configured low/high bounds. Shared by the
// meter fill and its trigger/floor tick marks so they all agree on the
// same scale.
static float analyzerRssiToFrac(float rssi) {
    if (rssi > ANALYZER_RSSI_HIGH) rssi = ANALYZER_RSSI_HIGH;
    if (rssi < ANALYZER_RSSI_LOW) rssi = ANALYZER_RSSI_LOW;
    return (rssi - ANALYZER_RSSI_LOW) / (ANALYZER_RSSI_HIGH - ANALYZER_RSSI_LOW);
}

// Horizontal RSSI meter: a filled bar for the live signal strength, plus
// a yellow tick for the current trigger threshold and a red tick for the
// auto-calibrated noise floor - so trigger headroom is something you can
// see at a glance instead of only reading as two separate numbers.
static void analyzerDrawMeter(int x, int y, int w, int h) {
    tft.fillRoundRect(x, y, w, h, h / 2, UI_PANEL);
    tft.drawRoundRect(x, y, w, h, h / 2, UI_BORDER);

    static float shownFrac = 0.0f;
    float targetFrac = (analyzerState.rssi_now != 0.0f) ? analyzerRssiToFrac(analyzerState.rssi_now) : 0.0f;
    rfUiEaseTowards(shownFrac, targetFrac, 0.25f, 0.01f);

    int innerW = w - 4;
    int fillW = (int)(innerW * shownFrac);
    uint16_t fillColor = analyzerState.has_signal ? UI_GREEN : UI_CYAN;
    if (fillW > 1 && h > 4) {
        tft.fillRoundRect(x + 2, y + 2, fillW, h - 4, (h - 4) / 2, fillColor);
    }

    int trigX = x + 2 + (int)(innerW * analyzerRssiToFrac(analyzerTrigLevel));
    tft.drawFastVLine(trigX, y - 3, h + 6, UI_AMBER);

    int floorX = x + 2 + (int)(innerW * analyzerRssiToFrac(analyzerState.noise_floor));
    tft.drawFastVLine(floorX, y - 3, h + 6, UI_RED);
}

static void updateAnalyzerLive() {
    // We only need to redraw the frequency value and the meter fill.
    // The cards, borders, and history rows are static between scans.

    int cardX = 10, cardY = 30;             // matches drawAnalyzerScreen layout
    int cardW = tft.width() - 20;
    int cardH = 56;

    // Clear just the inside of the frequency card (not the border)
    tft.fillRect(cardX + 2, cardY + 2, cardW - 4, cardH - 4, UI_PANEL);

    // Redraw the frequency value
    tft.setTextSize(3);
    char buf[16];
    if (analyzerState.has_signal && analyzerState.curr_freq > 0) {
        tft.setTextColor(UI_GREEN);
        snprintf(buf, sizeof(buf), "%03lu.%03lu",
                 analyzerState.curr_freq / 1000000UL % 1000UL,
                 analyzerState.curr_freq / 1000UL % 1000UL);
    } else {
        tft.setTextColor(UI_TEXT_DIM);
        snprintf(buf, sizeof(buf), "---.---");
    }
    tft.setCursor(cardX + 14, cardY + (cardH - 24) / 2);
    tft.print(buf);

    // Meter area — only repaint the fill portion
    int meterX = 10, meterY = cardY + cardH + 8;
    int meterW = tft.width() - 20, meterH = 14;
    analyzerDrawMeter(meterX, meterY, meterW, meterH);
}

static void drawAnalyzerScreen() {
    tft.fillScreen(UI_BG);
    int w = tft.width();
    int h = tft.height();
    char buf[32];

    // Saved frequency (set by pressing OK) is shown in the header subtitle
    // so it persists on screen.
    String savedLabel = "";
    if (analyzerState.saved_freq > 0) {
        char sbuf[24];
        snprintf(sbuf, sizeof(sbuf), "Saved %03lu.%03lu",
                 analyzerState.saved_freq / 1000000UL % 1000UL,
                 analyzerState.saved_freq / 1000UL % 1000UL);
        savedLabel = String(sbuf);
    }
    rfUiHeader("Analyzer", savedLabel, analyzerState.has_signal ? UI_GREEN : UI_CYAN);

    // Layout is computed from the screen's actual dimensions (rather than
    // hardcoded pixel positions) so it holds together across different
    // panel sizes, and shrinks the history row height automatically if
    // the screen is too short to fit everything at full size.
    int top = 30;
    int bottom = h - 24; // reserve two lines for the button-hint footer
    int avail = bottom - top;
    int gap = 8;
    int cardH = min(56, (avail * 30) / 100);
    int meterH = 14;
    int panelHeaderH = 16;
    int panelRowH = 16;
    int panelH = panelHeaderH + panelRowH * ANALYZER_HIST_CNT + 6;
    int neededH = cardH + gap + meterH + gap + panelH;
    if (neededH > avail) {
        int overBy = neededH - avail;
        int shrinkPerRow = (overBy / ANALYZER_HIST_CNT) + 1;
        panelRowH = max(10, panelRowH - shrinkPerRow);
        panelH = panelHeaderH + panelRowH * ANALYZER_HIST_CNT + 6;
    }

    int cardX = 10, cardY = top, cardW = w - 20;
    int meterX = 10, meterY = cardY + cardH + gap, meterW = w - 20;
    int panelX = 10, panelY = meterY + meterH + gap, panelW = w - 20;

    // ---- Frequency card ----
    uint16_t cardAccent = analyzerState.has_signal ? UI_GREEN : UI_BORDER;
    tft.fillRoundRect(cardX, cardY, cardW, cardH, 8, UI_PANEL);
    tft.drawRoundRect(cardX, cardY, cardW, cardH, 8, cardAccent);
    if (analyzerState.has_signal && rfUiBlinkOn(300)) {
        tft.drawRoundRect(cardX + 1, cardY + 1, cardW - 2, cardH - 2, 7, cardAccent);
    }

    tft.setTextSize(3);
    if (analyzerState.has_signal && analyzerState.curr_freq > 0) {
        tft.setTextColor(UI_GREEN);
        snprintf(buf, sizeof(buf), "%03lu.%03lu",
                 analyzerState.curr_freq / 1000000UL % 1000UL,
                 analyzerState.curr_freq / 1000UL % 1000UL);
    } else {
        tft.setTextColor(UI_TEXT_DIM);
        snprintf(buf, sizeof(buf), "---.---");
    }
    tft.setCursor(cardX + 14, cardY + (cardH - 24) / 2);
    tft.print(buf);
    tft.setTextSize(1);
    tft.setTextColor(UI_TEXT);
    tft.setCursor(cardX + cardW - 34, cardY + cardH / 2 - 3);
    tft.print("MHz");

    // Status pill, top-right corner of the card.
    String statusTxt = analyzerState.has_signal ? "LOCKED" : "SCAN";
    uint16_t pillColor = analyzerState.has_signal ? UI_GREEN : UI_CYAN;
    int pillW = tft.textWidth(statusTxt) + 12;
    int pillX = cardX + cardW - pillW - 8;
    int pillY = cardY + 6;
    tft.fillRoundRect(pillX, pillY, pillW, 14, 7, pillColor);
    tft.setTextColor(UI_BG);
    tft.setCursor(pillX + 6, pillY + 3);
    tft.print(statusTxt);

    // Scanning sweep marker along the bottom of the card while hunting.
    if (!analyzerState.has_signal) {
        float t = fmodf(rfUiPhase(0.0004f), 2.0f);
        float back = (t > 1.0f) ? (2.0f - t) : t;
        int sweepX = cardX + 6 + (int)(back * (cardW - 12));
        tft.drawFastVLine(sweepX, cardY + cardH - 6, 6, UI_CYAN);
    }

    // ---- RSSI meter ----
    analyzerDrawMeter(meterX, meterY, meterW, meterH);

    // ---- Recent frequencies panel ----
    tft.fillRoundRect(panelX, panelY, panelW, panelH, 6, UI_PANEL);
    tft.drawRoundRect(panelX, panelY, panelW, panelH, 6, UI_BORDER);
    tft.setTextColor(UI_CYAN);
    tft.setTextSize(1);
    tft.setCursor(panelX + 8, panelY + 4);
    tft.print("Recent");
    tft.setCursor(panelX + panelW - 92, panelY + 4);
    tft.print("Hits  Peak");
    tft.drawFastHLine(panelX + 4, panelY + panelHeaderH, panelW - 8, UI_BORDER);

    for (uint8_t i = 0; i < ANALYZER_HIST_CNT; i++) {
        int rowY = panelY + panelHeaderH + 2 + i * panelRowH;
        bool active = analyzerState.hist_freq[i] > 0;
        tft.setTextColor(active ? UI_TEXT : UI_TEXT_DIM);
        tft.setCursor(panelX + 8, rowY);
        if (active) {
            snprintf(buf, sizeof(buf), "%03lu.%03lu",
                     analyzerState.hist_freq[i] / 1000000UL % 1000UL,
                     analyzerState.hist_freq[i] / 1000UL % 1000UL);
        } else {
            snprintf(buf, sizeof(buf), "---.---");
        }
        tft.print(buf);

        tft.setCursor(panelX + panelW - 92, rowY);
        if (active && analyzerState.hist_count[i] > 0) {
            snprintf(buf, sizeof(buf), "x%-3u", analyzerState.hist_count[i]);
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        tft.print(buf);

        tft.setCursor(panelX + panelW - 48, rowY);
        if (active && analyzerState.hist_count[i] > 0) {
            snprintf(buf, sizeof(buf), "%ddB", (int)analyzerState.hist_peak_rssi[i]);
        } else {
            snprintf(buf, sizeof(buf), "-");
        }
        tft.print(buf);
    }

    // ---- Footer readout: trigger / floor as text (meter ticks above are
    // the at-a-glance version; this is the precise numeric one) ----
    int footY = panelY + panelH + 6;
    tft.setTextColor(UI_AMBER);
    tft.setTextSize(1);
    tft.setCursor(10, footY);
    tft.print("Trig " + String((int)analyzerTrigLevel) + "dB");
    tft.setTextColor(UI_RED);
    tft.setCursor(10 + tft.textWidth("Trig -75dB") + 10, footY);
    tft.print("Floor " + String((int)analyzerState.noise_floor) + "dB");

    if (analyzerSaveFlashUntil > millis()) {
        tft.setTextColor(UI_GREEN);
        String flashTxt = "SAVED!";
        tft.setCursor(w - tft.textWidth(flashTxt) - 10, footY);
        tft.print(flashTxt);
    }

    tft.setTextColor(UI_TEXT_DIM);
    tft.setTextSize(1);
    tft.setCursor(4, h - 12);
    tft.print("UP/DN:Trig  OK:Save  BACK:Exit");
}


static void drawJammerStatic() {
    tft.fillScreen(UI_BG);
    int w = tft.width();
    uint16_t accent = isJamming ? UI_RED : UI_CYAN;
    rfUiHeader("Jammer", String(frequency, 2) + " MHz", accent);

    if (isJamming) {
        tft.setTextColor(UI_RED);
        tft.setTextSize(2);
        String txt = "TRANSMITTING";
        tft.setCursor((w - tft.textWidth(txt)) / 2, 150);
        tft.print(txt);
    } else {
        int cardX = 10, cardY = 50, cardW = w - 20, cardH = 70;
        tft.fillRoundRect(cardX, cardY, cardW, cardH, 8, UI_PANEL);
        tft.drawRoundRect(cardX, cardY, cardW, cardH, 8, UI_BORDER);
        tft.setTextColor(UI_TEXT_DIM);
        tft.setTextSize(1);
        String hint = "Press OK to start jamming";
        tft.setCursor(cardX + (cardW - tft.textWidth(hint)) / 2, cardY + cardH / 2 - 4);
        tft.print(hint);
    }

    tft.drawFastHLine(0, tft.height() - 16, w, UI_BORDER);
    tft.setTextColor(UI_TEXT_DIM);
    tft.setTextSize(1);
    tft.setCursor(6, tft.height() - 12);
    tft.print("UP/DN freq   OK toggle   BACK exit");
}

// Call repeatedly when jamming — animates only the pulse rings.
static void updateJammerAnimation() {
    int w = tft.width();
    const int cx = w / 2;
    const int cy = 90;
    const int R  = 55;

    // Clear only the ring region
    tft.fillRect(cx - R - 4, cy - R - 4, (R + 4) * 2, (R + 4) * 2, UI_BG);

    float phase = rfUiPhase(0.0006f);
    rfUiPulseRings(cx, cy, phase, UI_RED, 3, R);
}

// Backward-compatible entry.
static void drawJammerScreen() {
    drawJammerStatic();
    if (isJamming) updateJammerAnimation();
}

static void startJamming() {
    if (!ensureCC1101Initialized()) return;

    // Put chip in IDLE
    ELECHOUSE_cc1101.SpiStrobe(0x36);
    delayMicroseconds(100);

    // Set frequency
    ELECHOUSE_cc1101.setMHZ(frequency);

    // Disable sync, set packet format to 0 (no packet)
    ELECHOUSE_cc1101.setSyncMode(0);        // no sync word
    ELECHOUSE_cc1101.setPktFormat(0);       // no packet handling
    ELECHOUSE_cc1101.setCrc(0);             // no CRC

    // Set ASK/OOK with 0 deviation (carrier)
    ELECHOUSE_cc1101.setModulation(0);
    ELECHOUSE_cc1101.setDeviation(0);

    // Set PA table to max (0xFF)
    ELECHOUSE_cc1101.setPA(12);             // or set PATABLE via library if available

    // Put in TX mode – this will emit a continuous carrier
    // because there is no packet structure, the chip keeps transmitting
    ELECHOUSE_cc1101.SetTx();

    // (Optional) force carrier by setting test register
    // ELECHOUSE_cc1101.SpiWriteReg(0x35, 0x00); // may not be needed
}

static void stopJamming() {
    ELECHOUSE_cc1101.SpiWriteReg(0x35, 0x00);
    ELECHOUSE_cc1101.SetRx();
    restoreReceiveMode();
}

static String getTypeName(emKeys tp) {
    switch (tp) {
        case kUnknown: return "Unknown";
        case kPrinceton: return "Princeton";
        case kRcSwitch: return "RcSwitch";
        case kCAME: return "CAME";
        case kNICE: return "Nice Flo";
        case kHOLTEK: return "Holtek";
        case kKeeLoq: return "KeeLoq";
        case kStarLine: return "Star Line";
        case kLINEAR: return "RAW";
        case kChamberlain: return "Chamberlain";
        case kAnsonic: return "Ansonic";
        case kLinearDelta3: return "Linear Delta-3";
        case kSomfyRTS: return "Somfy RTS";
        case kNiceFlorS: return "Nice FloR-S";
        case kSecPlusV1: return "Security+1.0";
        case kSecPlusV2: return "Security+2.0";
        case kFaacSLH: return "FAAC SLH";
        case kBftMitto: return "BFT Mitto";
        case kCameAtomo: return "CAME Atomo";
        case kAnMotors: return "AN-Motors";
        case kHcs101: return "HCS101";
        case kAlutechAT4N: return "Alutech AT-4N";
        case kGateTX: return "Gate TX";
        case kDoorHan: return "DoorHan";
        case kSMC5326: return "SMC5326";
        case kHormann: return "Hörmann";
        case kMarantec: return "Marantec";
        case kKiaHyundai: return "Kia/Hyundai";
        case kMegaCode: return "MegaCode";
        case kAprimatic: return "Aprimatic";
        case kIronLogic: return "IronLogic";
        case kSommer: return "Sommer";
        case kMutancode: return "Mutancode";
        case kMHouse: return "MHouse";
        case kUNILARM: return "UNILARM";
        case kiDo: return "iDo";
        default: return "Unknown";
    }
}

static void resetButtonStates() {
    // not needed
}


bool rfPrepareSignalList(const char* path) {
    currentSubFilePath = "";
    currentSignalNames.clear();
    isRawFile = false;
    memset(&currentKeyData, 0, sizeof(tpKeyData));

    if (!loadKeyFromSD(path, &currentKeyData)) return false;
    if (currentKeyData.type != kLINEAR) {
        currentSignalNames.push_back(getTypeName((emKeys)currentKeyData.type));
    } else {
        currentSignalNames.push_back("RAW");
        isRawFile = true;
    }
    currentSubFilePath = path;
    return currentSignalNames.size() > 0;
}

int rfGetSignalCount() {
    return currentSignalNames.size();
}

String rfGetSignalName(int index) {
    if (index < 0 || index >= (int)currentSignalNames.size()) return "";
    return currentSignalNames[index];
}

bool rfSendSelectedSignal(int index) {
    if (currentSubFilePath.length() == 0) return false;
    if (index < 0 || index >= (int)currentSignalNames.size()) return false;

    // BUGFIX: both branches below transmit at the CC1101's *current*
    // frequency, not the frequency stored in the .sub file that was just
    // loaded. That mismatch (e.g. a 315MHz file replayed while the radio
    // was still set to 433.92MHz) is why "Send" looked broken - the file
    // was being sent, just on the wrong frequency. Sync the active
    // frequency from the loaded file before transmitting, like Flipper does.
    if (currentKeyData.frequency > 0.0f) {
        frequency = currentKeyData.frequency;
        for (int i = 0; i < numFrequencies; i++) {
            if (fabsf(frequencies[i] - frequency) < 0.01f) { freqIndex = i; break; }
        }
    }

    bool ok = isRawFile ? playRawRecorderFile(currentSubFilePath)
                         : rfSendKey(&currentKeyData);

    // BUGFIX: this is reached from the SD file picker with inSubMenu
    // already set false (see case 2 "Send" in rfHandleMenuEvent), and the
    // file picker itself has no notion of the Sub-GHz submenu. Without
    // this, finishing (or failing) a send dropped straight back to the
    // device's top-level main menu instead of back to the Sub-GHz
    // (Read/Read RAW/Send/Analyzer/...) submenu the user came from.
    subState = menuMain;
    inSubMenu = true;
    resetButtonStates();
    rfEnterMenu();

    return ok;
}

bool rfReplayFromSD(const char* path) {
    if (rfPrepareSignalList(path)) {
        if (rfGetSignalCount() > 0) {
            return rfSendSelectedSignal(0);
        }
    }
    return false;
}

void rfDeinit() {
    if (cc1101Initialized) {
        // Put the CC1101 into SLEEP state (register value 0x39)
        // instead of just IDLE. This drops consumption from ~1.7mA
        // (IDLE) to ~400nA (SLEEP) on the chip itself.
        ELECHOUSE_cc1101.SpiStrobe(0x36);  // IDLE first (required before SLEEP)
        delayMicroseconds(100);
        ELECHOUSE_cc1101.SpiStrobe(0x39);  // SLEEP
        cc1101Initialized = false;         // force re-init on next use
        cc1101PinsReady = false;           // force SPI re-arming too
    }
}

void rfEnterSaved() {}
void rfHandleSavedEvent(int evt) {}
void rfEnterSavedAction() {}
void rfHandleSavedActionEvent(int evt) {}

void rfJammerStart() {
    if (!isJamming) {
        startJamming();
        isJamming = true;
        rfLedSet(RF_LED_JAMMING);
    }
}
void rfJammerStop() {
    if (isJamming) {
        stopJamming();
        isJamming = false;
        rfLedSet(RF_LED_JAM_IDLE);
    }
}
bool rfJammerIsRunning() { return isJamming; }

static void drawBruteConfig(int previousSelection) {
    (void)previousSelection;
    // TODO: implement brute‑force configuration UI later
    // For now, this stub prevents the linker error.
}
void rfHandleMenuEvent(int evt) {
    if (!inSubMenu) {
        rfEnterMenu();
        return;
    }
    // Delegate to sub‑state handler
    rfHandleSubState(evt);
}