// keyboard_layouts.cpp
#include "hid_module.h"

// ============================================================
//  Keyboard layout tables
//
//  Format: 128-entry uint8_t array, indexed by ASCII code (0-127).
//  Each entry is:
//     0x00                     = unmappable character
//     [modifier << 8] | scancode  (packed as two bytes in a uint16_t)
//
//  Actually we use a compact two-array format:
//     keys[128]   = HID usage code
//     mods[128]   = modifier byte (0, MOD_SHIFT, MOD_ALTGR, ...)
//
//  Modifier bits (matching USB HID spec):
//     0x02 = Left Shift
//     0x40 = Right Alt (AltGr)
// ============================================================

#define MOD_NONE   0x00
#define MOD_SHIFT  0x02
#define MOD_ALTGR  0x40

// ---- en_US ----
static const uint8_t en_US_keys[128] = {
    0,0,0,0,0,0,0,0, 0,0x2B,0x28,0,0,0,0,0,        // 0x00-0x0F
    0,0,0,0,0,0,0,0, 0,0,0,0x29,0,0,0,0,           // 0x10-0x1F
    0x2C,0x1E,0x34,0x20,0x21,0x22,0x24,0x34,       //  ! " # $ % & '
    0x26,0x27,0x25,0x2E,0x36,0x2D,0x37,0x38,       // ( ) * + , - . /
    0x27,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,       // 0 1 2 3 4 5 6 7
    0x25,0x26,0x33,0x33,0x36,0x2E,0x37,0x38,       // 8 9 : ; < = > ?
    0x1F,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,       // @ A B C D E F G
    0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,       // H I J K L M N O
    0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,       // P Q R S T U V W
    0x1B,0x1C,0x1D,0x2F,0x31,0x30,0x35,0x2D,       // X Y Z [ \ ] ^ _
    0x35,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,       // ` a b c d e f g
    0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,       // h i j k l m n o
    0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,       // p q r s t u v w
    0x1B,0x1C,0x1D,0x2F,0x31,0x30,0x35,0x00        // x y z { | } ~ DEL
};

static const uint8_t en_US_mods[128] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_NONE,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_NONE, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_NONE, MOD_NONE, MOD_NONE, MOD_SHIFT, MOD_SHIFT,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, 0
};

// ---- de_DE (abbreviated — same format as en_US) ----
// The full table is 128 entries; here are the key differences from en_US
// so you can see the pattern. Populate the rest from the arduino-esp32
// KeyboardLayout_de_DE.h file (MIT-licensed) for a real build.
static const uint8_t de_DE_keys[128] = {
    0,0,0,0,0,0,0,0, 0,0x2B,0x28,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0x29,0,0,0,0,
    0x2C,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,  // ! " § $ % & /
    0x25,0x26,0x27,0x2E,0x36,0x2D,0x37,0x38,  // ( ) * + , - . /
    0x27,0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,
    0x25,0x26,0x33,0x33,0x36,0x2E,0x37,0x38,
    0x14,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,  // @ A B C D E F G
    0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,
    0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,
    0x1B,0x1C,0x1D,0x2F,0x31,0x30,0x35,0x2D,
    0x35,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,
    0x0B,0x0C,0x0D,0x0E,0x0F,0x10,0x11,0x12,
    0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1A,
    0x1B,0x1C,0x1D,0x2F,0x31,0x30,0x35,0x00
};
static const uint8_t de_DE_mods[128] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0, MOD_SHIFT, MOD_ALTGR, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_SHIFT,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_SHIFT, MOD_SHIFT, MOD_NONE, MOD_SHIFT, MOD_NONE, MOD_SHIFT,
    MOD_ALTGR, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_SHIFT, MOD_SHIFT, MOD_ALTGR, MOD_ALTGR, MOD_ALTGR, MOD_SHIFT, MOD_SHIFT,
    MOD_SHIFT, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE, MOD_NONE,
    MOD_NONE, MOD_NONE, MOD_NONE, MOD_ALTGR, MOD_ALTGR, MOD_ALTGR, MOD_SHIFT, 0
};

// ---- Other layouts follow the same pattern. ----
// For brevity the full tables for FR_FR, ES_ES, IT_IT, PT_BR, PT_PT,
// SV_SE, DA_DK, HU_HU are omitted here. Copy them verbatim from
// arduino-esp32/libraries/USB/src/keyboardLayout/*.h — they are MIT
// licensed and designed for exactly this use case. Each is two arrays
// (keys + mods) of 128 entries.

// ============================================================
//  Layout registry
// ============================================================
struct LayoutEntry {
    KeyboardLayout id;
    const char*    name;
    const uint8_t* keys;
    const uint8_t* mods;
};

// Populate this table with every layout you support. Entries pointing
// at nullptr fall back to en_US at runtime.
static const LayoutEntry LAYOUTS[] = {
    { KeyboardLayout::EN_US, "en_US", en_US_keys, en_US_mods },
    { KeyboardLayout::DE_DE, "de_DE", de_DE_keys, de_DE_mods },
    // { KeyboardLayout::FR_FR, "fr_FR", fr_FR_keys, fr_FR_mods },
    // ... add the rest as you copy their tables in
};

static const int LAYOUT_COUNT = sizeof(LAYOUTS) / sizeof(LAYOUTS[0]);

const char* hidLayoutName(KeyboardLayout layout) {
    for (int i = 0; i < LAYOUT_COUNT; i++)
        if (LAYOUTS[i].id == layout) return LAYOUTS[i].name;
    return "en_US";
}

KeyboardLayout hidLayoutFromString(const String& name) {
    for (int i = 0; i < LAYOUT_COUNT; i++)
        if (name.equalsIgnoreCase(LAYOUTS[i].name)) return LAYOUTS[i].id;
    return KeyboardLayout::EN_US;
}

const uint8_t* hidLayoutTable(KeyboardLayout layout) {
    for (int i = 0; i < LAYOUT_COUNT; i++)
        if (LAYOUTS[i].id == layout) return LAYOUTS[i].keys;
    return en_US_keys;
}

uint8_t hidLayoutModifierFor(KeyboardLayout layout, uint8_t ascii) {
    if (ascii >= 128) return 0;
    for (int i = 0; i < LAYOUT_COUNT; i++) {
        if (LAYOUTS[i].id == layout) return LAYOUTS[i].mods[ascii];
    }
    // Layout not in the registry (e.g. one of the abbreviated ones) —
    // fall back to en_US so we always return something usable.
    return en_US_mods[ascii];
}