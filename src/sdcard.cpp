#include "sdcard.h"
#include "config.h"
#include <SPI.h>
#include <SD.h>

static bool mounted = false;

bool sdInit() {
  // Reuses the SPI bus already initialized by displayInit() (TFT_eSPI),
  // just with its own CS pin.
  mounted = SD.begin(PIN_SD_CS);
  return mounted;
}

bool sdIsMounted() { return mounted; }

std::vector<String> sdListDir(const char* path) {
  std::vector<String> out;
  if (!mounted) return out;

  File dir = SD.open(path);
  if (!dir || !dir.isDirectory()) return out;

  File entry = dir.openNextFile();
  while (entry) {
    String name = entry.name();
    if (entry.isDirectory()) name += "/";
    out.push_back(name);
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();
  return out;
}

bool sdReadFile(const char* path, String& outContent) {
  if (!mounted) return false;
  File f = SD.open(path, FILE_READ);
  if (!f) return false;

  size_t size = f.size();
  outContent = "";
  if (!outContent.reserve(size)) {  // one allocation up front, not thousands
    f.close();
    return false;
  }

  const size_t BUF_SZ = 512;
  uint8_t buf[BUF_SZ];
  while (f.available()) {
    size_t n = f.read(buf, BUF_SZ);
    if (n == 0) break;
    outContent.concat((const char*)buf, n);
  }
  f.close();
  return true;
}

bool sdWriteFile(const char* path, const String& content) {
  if (!mounted) return false;
  File f = SD.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();
  return true;
}

bool sdAppendLine(const char* path, const String& line) {
  if (!mounted) return false;
  File f = SD.open(path, FILE_APPEND);
  if (!f) return false;
  f.println(line);
  f.close();
  return true;
}

bool sdDeleteFile(const char* path) {
  if (!mounted) return false;
  return SD.remove(path);
}

bool sdWriteFile(const char* path, const uint8_t* data, size_t len) {
  File file = SD.open(path, FILE_WRITE);
  if (!file) return false;
  if (data && len > 0) {
    size_t written = file.write(data, len);
    file.close();
    return written == len;
  } else {
    // Create an empty file (just close it)
    file.close();
    return true;
  }
}

bool sdAppendFile(const char* path, const uint8_t* data, size_t len) {
  File file = SD.open(path, FILE_APPEND);
  if (!file) return false;
  size_t written = file.write(data, len);
  file.close();
  return written == len;
}