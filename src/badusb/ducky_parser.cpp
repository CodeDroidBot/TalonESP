// ducky_parser.cpp
#include "hid_module.h"
#include <SD.h>
#include <vector>
#include <map>

// ============================================================
//  Global-scope hook into the HID module's non-blocking delay
//  (defined in hid_module.cpp). Declaring it here at global scope
//  makes the linker find the global symbol, which was the actual
//  problem with the previous version — declaring it inside a
//  function inside namespace Ducky would have created
//  Ducky::duckySetDelayUntil, not ::duckySetDelayUntil.
// ============================================================
void duckySetDelayUntil(unsigned long ms);

namespace Ducky {

// ============================================================
//  LoopFrame is at namespace scope (not nested inside ParserState)
//  so executeLine can reference it without qualification.
// ============================================================
struct LoopFrame {
    int start;      // line index of the matching WHILE
    int end;        // line index of the matching END_WHILE (-1 until resolved)
    int count;      // reserved: iterations, if you later add a count
    int iter;       // current iteration
};

struct ParserState {
    std::map<String, int> labels;        // label name -> line index
    std::map<String, int> functions;     // function name -> line index
    std::map<String, int> variables;     // $var -> value
    std::map<String, int> defines;       // #define -> value

    std::vector<String> lines;           // source lines
    int  currentLine   = 0;              // 0-based index into lines
    int  defaultDelay  = 0;              // ms
    bool running       = false;
    bool stopRequested = false;
    String lastError   = "";
    String status      = "Idle";

    std::vector<LoopFrame> loopStack;
    std::vector<int>       callStack;    // line index to return to
};

static ParserState g;

// ============================================================
//  Helpers
// ============================================================
static String trim(const String& s) {
    String out = s;
    out.trim();
    return out;
}

static String toUpper(const String& s) {
    String out = s;
    out.toUpperCase();
    return out;
}

// Strip REM comments: everything after " REM " is ignored. Also handles
// the DuckyScript "//" style as a courtesy.
static String stripComment(const String& line) {
    int rem = line.indexOf(" REM ");
    if (rem >= 0) return line.substring(0, rem);
    if (line.startsWith("REM ")) return "";

    int slashes = line.indexOf("//");
    if (slashes >= 0) return line.substring(0, slashes);

    return line;
}

// ============================================================
//  HID action dispatch
// ============================================================
static bool typeChar(char c) {
    KeyboardLayout layout = hidGetLayout();
    const uint8_t* keys = hidLayoutTable(layout);
    uint8_t ascii = (uint8_t)c;
    if (ascii >= 128) return false;
    uint8_t key = keys[ascii];
    if (key == 0) return false;
    uint8_t mod = hidLayoutModifierFor(layout, ascii);
    if (mod) {
        hidPressModifierKey(mod, key);
    } else {
        hidPressKey(key);
        hidReleaseKey(key);
    }
    return true;
}

static void typeString(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        typeChar(s[i]);
        if (g.defaultDelay > 0) delay(g.defaultDelay);
    }
}

// ============================================================
//  Modifier parsing
// ============================================================
static uint8_t parseModifierToken(const String& tok, bool& ok) {
    ok = true;
    if (tok == "CTRL"   || tok == "CONTROL") return 0x01;
    if (tok == "SHIFT")                       return 0x02;
    if (tok == "ALT")                         return 0x04;
    if (tok == "GUI"    || tok == "WINDOWS" || tok == "META") return 0x08;
    if (tok == "CTRL-R" || tok == "RIGHT_CTRL")  return 0x10;
    if (tok == "SHIFT-R"|| tok == "RIGHT_SHIFT") return 0x20;
    if (tok == "ALT-R"  || tok == "RIGHT_ALT")   return 0x40;
    if (tok == "GUI-R"  || tok == "RIGHT_GUI")   return 0x80;
    ok = false;
    return 0;
}

// ============================================================
//  Special key mapping (DuckyScript token -> HID usage code)
// ============================================================
static uint8_t specialKeyCode(const String& tok) {
    if (tok == "ENTER"      || tok == "RETURN")    return 0x28;
    if (tok == "ESC"        || tok == "ESCAPE")    return 0x29;
    if (tok == "BACKSPACE")                         return 0x2A;
    if (tok == "TAB")                               return 0x2B;
    if (tok == "SPACE")                             return 0x2C;
    if (tok == "CAPSLOCK")                          return 0x39;
    if (tok == "DELETE"     || tok == "DEL")        return 0x4C;
    if (tok == "INSERT")                            return 0x49;
    if (tok == "HOME")                              return 0x4A;
    if (tok == "END")                               return 0x4D;
    if (tok == "PAGEUP")                            return 0x4B;
    if (tok == "PAGEDOWN")                          return 0x4E;
    if (tok == "UP")                                return 0x52;
    if (tok == "DOWN")                              return 0x51;
    if (tok == "LEFT")                              return 0x50;
    if (tok == "RIGHT")                             return 0x4F;
    if (tok == "PRINTSCREEN")                       return 0x46;
    if (tok == "PAUSE")                             return 0x48;
    if (tok == "MENU"       || tok == "APP")        return 0x65;
    if (tok == "NUMLOCK")                           return 0x53;
    if (tok == "SCROLLLOCK")                        return 0x47;
    if (tok.startsWith("F") && tok.length() <= 3) {
        int n = tok.substring(1).toInt();
        if (n >= 1 && n <= 12) return 0x3A + (n - 1);
    }
    return 0;
}

// ============================================================
//  Numeric expression evaluator (for IF/WHILE/VAR)
//
//  The nested "P" parser struct has an `ok` member pointer instead of
//  trying to touch the enclosing function's parameter directly —
//  a member function of a local class cannot reference locals of the
//  containing function in C++, which was the cause of the original
//  "use of parameter from containing function" error.
// ============================================================
static int evalExpr(const String& expr, bool& ok) {
    ok = true;
    String e = expr;

    // Expand variables and defines (strip sigils first)
    String tmp = e;
    tmp.replace("$", " ");
    tmp.replace("#", " ");
    tmp.replace(" ", "");

    // Substitute $var and #define values. We walk the maps and do a
    // straight literal replace; script authors are expected to use
    // short, non-overlapping names.
    for (auto& kv : g.variables) {
        tmp.replace(kv.first, String(kv.second));
    }
    for (auto& kv : g.defines) {
        tmp.replace(kv.first, String(kv.second));
    }
    e = tmp;

    // Comparison operators — evaluate as 0/1. These recurse via the
    // outer evalExpr, which is fine because it's a free function.
    int eqPos = e.indexOf("==");
    if (eqPos >= 0) {
        bool okA = true, okB = true;
        int a = evalExpr(e.substring(0, eqPos), okA);
        int b = evalExpr(e.substring(eqPos + 2), okB);
        if (!okA || !okB) { ok = false; return 0; }
        return (a == b) ? 1 : 0;
    }
    int nePos = e.indexOf("!=");
    if (nePos >= 0) {
        bool okA = true, okB = true;
        int a = evalExpr(e.substring(0, nePos), okA);
        int b = evalExpr(e.substring(nePos + 2), okB);
        if (!okA || !okB) { ok = false; return 0; }
        return (a != b) ? 1 : 0;
    }

    struct P {
        const char* s;
        int  pos;
        bool* okOut;

        int parseExpr() {
            int v = parseTerm();
            while (s[pos] == '+' || s[pos] == '-') {
                char op = s[pos++];
                int r = parseTerm();
                v = (op == '+') ? v + r : v - r;
            }
            return v;
        }
        int parseTerm() {
            int v = parseFactor();
            while (s[pos] == '*' || s[pos] == '/') {
                char op = s[pos++];
                int r = parseFactor();
                if (op == '*') v *= r; else if (r != 0) v /= r;
            }
            return v;
        }
        int parseFactor() {
            if (s[pos] == '(') {
                pos++;
                int v = parseExpr();
                if (s[pos] == ')') pos++;
                return v;
            }
            if (s[pos] == '-') {
                pos++;
                return -parseFactor();
            }
            int start = pos;
            while (s[pos] >= '0' && s[pos] <= '9') pos++;
            if (start == pos) {
                if (okOut) *okOut = false;
                return 0;
            }
            // Build a small String from the digit run. This is the only
            // heap allocation in the evaluator, and it's tiny.
            String digits;
            digits.reserve(pos - start);
            for (int i = start; i < pos; i++) digits += s[i];
            return digits.toInt();
        }
    };

    bool localOk = true;
    P p{ e.c_str(), 0, &localOk };
    int result = p.parseExpr();
    if (!localOk) ok = false;
    return result;
}

// ============================================================
//  Main line executor
// ============================================================
int executeLine(const String& rawLine, String& outError) {
    outError = "";
    String line = stripComment(rawLine);
    line.trim();
    if (line.length() == 0) return 0;

    // Split into tokens
    std::vector<String> tokens;
    int start = 0;
    while (start < (int)line.length()) {
        int sp = line.indexOf(' ', start);
        if (sp < 0) { tokens.push_back(line.substring(start)); break; }
        tokens.push_back(line.substring(start, sp));
        start = sp + 1;
        while (start < (int)line.length() && line[start] == ' ') start++;
    }
    if (tokens.empty()) return 0;

    String cmd = toUpper(tokens[0]);

    // ---- REM_BLOCK / END_REM ----
    if (cmd == "REM_BLOCK") {
        while (g.currentLine < (int)g.lines.size()) {
            g.currentLine++;
            if (g.currentLine >= (int)g.lines.size()) break;
            String l = trim(g.lines[g.currentLine]);
            if (toUpper(l) == "END_REM") break;
        }
        return 0;
    }

    // ---- DEFAULT_DELAY ----
    if (cmd == "DEFAULT_DELAY" || cmd == "DEFAULTDELAY") {
        if (tokens.size() >= 2) g.defaultDelay = tokens[1].toInt();
        return 0;
    }

    // ---- DELAY ----
    if (cmd == "DELAY") {
        if (tokens.size() >= 2) {
            int ms = tokens[1].toInt();
            ::duckySetDelayUntil(millis() + ms);
        }
        return 0;
    }

    // ---- STRING / STRINGLN ----
    if (cmd == "STRING" || cmd == "STRINGLN") {
        int sp = line.indexOf(' ');
        String text = (sp >= 0) ? line.substring(sp + 1) : "";
        typeString(text);
        if (cmd == "STRINGLN") {
            hidPressKey(0x28);
            hidReleaseKey(0x28);
        }
        return 0;
    }

    // ---- LAYOUT ----
    if (cmd == "LAYOUT") {
        if (tokens.size() >= 2) {
            KeyboardLayout l = hidLayoutFromString(tokens[1]);
            hidSetLayout(l);
        }
        return 0;
    }

    // ---- VAR ----
    if (cmd == "VAR") {
        int eq = line.indexOf('=');
        if (eq < 0) { outError = "VAR missing '='"; return 2; }
        String lhs = trim(line.substring(4, eq));
        String rhs = trim(line.substring(eq + 1));
        bool ok = true;
        int v = evalExpr(rhs, ok);
        if (!ok) { outError = "VAR bad expression: " + rhs; return 2; }
        String name = lhs;
        name.replace("$", "");
        g.variables[name] = v;
        return 0;
    }

    // ---- DEFINE ----
    if (cmd == "DEFINE") {
        if (tokens.size() < 3) { outError = "DEFINE needs name and value"; return 2; }
        String name = tokens[1];
        name.replace("#", "");
        g.defines[name] = tokens[2].toInt();
        return 0;
    }

    // ---- REPEAT ----
    if (cmd == "REPEAT") {
        if (tokens.size() < 2) { outError = "REPEAT needs a count"; return 2; }
        int count = tokens[1].toInt();
        int prev = g.currentLine - 1;
        while (prev >= 0 && trim(stripComment(g.lines[prev])).length() == 0) prev--;
        if (prev < 0) { outError = "REPEAT has nothing to repeat"; return 2; }
        for (int i = 0; i < count; i++) {
            String e;
            int rc = executeLine(g.lines[prev], e);
            if (rc == 2) { outError = e; return 2; }
        }
        return 0;
    }

    // ---- IF (single-line form:  IF <expr> THEN <command>) ----
    if (cmd == "IF") {
        int thenPos = line.indexOf("THEN");
        if (thenPos < 0) { outError = "IF missing THEN"; return 2; }
        String cond = trim(line.substring(2, thenPos));
        String thenCmd = trim(line.substring(thenPos + 4));
        bool ok = true;
        int v = evalExpr(cond, ok);
        if (!ok) { outError = "IF bad condition"; return 2; }
        if (v) {
            String e;
            int rc = executeLine(thenCmd, e);
            if (rc == 2) { outError = e; return 2; }
        }
        return 0;
    }

    // ---- WHILE / END_WHILE ----
    if (cmd == "WHILE") {
        bool ok = true;
        int v = evalExpr(line.substring(6), ok);
        if (!ok) { outError = "WHILE bad condition"; return 2; }
        if (!v) {
            int depth = 1;
            while (g.currentLine < (int)g.lines.size() && depth > 0) {
                g.currentLine++;
                if (g.currentLine >= (int)g.lines.size()) break;
                String l = toUpper(trim(stripComment(g.lines[g.currentLine])));
                if (l.startsWith("WHILE")) depth++;
                if (l.startsWith("END_WHILE")) depth--;
            }
        } else {
            LoopFrame f{ g.currentLine, -1, 0, 0 };
            g.loopStack.push_back(f);
        }
        return 0;
    }

    if (cmd == "END_WHILE") {
        if (g.loopStack.empty()) { outError = "END_WHILE without WHILE"; return 2; }
        LoopFrame& f = g.loopStack.back();
        g.currentLine = f.start;
        return 0;
    }

    // ---- FUNCTION / CALL ----
    if (cmd == "FUNCTION") {
        if (tokens.size() >= 2) g.functions[tokens[1]] = g.currentLine;
        return 0;
    }
    if (cmd == "END_FUNCTION") {
        if (!g.callStack.empty()) {
            g.currentLine = g.callStack.back();
            g.callStack.pop_back();
        }
        return 0;
    }
    if (cmd == "CALL") {
        if (tokens.size() < 2) { outError = "CALL needs a function name"; return 2; }
        auto it = g.functions.find(tokens[1]);
        if (it == g.functions.end()) { outError = "Unknown function: " + tokens[1]; return 2; }
        g.callStack.push_back(g.currentLine);
        g.currentLine = it->second;
        return 0;
    }

    // ---- LED commands ----
    if (cmd == "LED_R" || cmd == "LED_G" || cmd == "LED_B" || cmd == "LED_OFF") {
        return 0;
    }

    // ---- Modifier + key combos ----
    uint8_t mod = 0;
    uint8_t key = 0;

    for (size_t i = 0; i < tokens.size(); i++) {
        String tok = tokens[i];
        int dash = tok.indexOf('-');
        if (dash > 0) {
            String a = tok.substring(0, dash);
            String b = tok.substring(dash + 1);
            bool okA = false, okB = false;
            uint8_t ma = parseModifierToken(toUpper(a), okA);
            uint8_t mb = parseModifierToken(toUpper(b), okB);
            if (okA && okB) { mod |= ma | mb; continue; }
        }
        bool ok = false;
        uint8_t m = parseModifierToken(toUpper(tok), ok);
        if (ok) { mod |= m; continue; }
        uint8_t k = specialKeyCode(toUpper(tok));
        if (k) { key = k; continue; }
    }

    if (mod && key) {
        hidPressModifierKey(mod, key);
        return 0;
    }
    if (mod) {
        hidPressModifierKey(mod, 0);
        return 0;
    }
    if (key) {
        hidPressKey(key);
        hidReleaseKey(key);
        return 0;
    }

    outError = "Unknown command: " + line;
    return 2;
}

// ============================================================
//  Public API
// ============================================================
void reset() {
    g = ParserState();
}

void setLines(const std::vector<String>& lines) {
    g.lines = lines;
    g.currentLine = 0;
    g.loopStack.clear();
    g.callStack.clear();
    g.variables.clear();
    g.defines.clear();
}

bool isRunning() { return g.running; }
int  lineNumber() { return g.currentLine; }
int  totalLines() { return (int)g.lines.size(); }

} // namespace Ducky