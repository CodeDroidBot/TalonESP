#pragma once
#include <Arduino.h>
#include <vector>

bool sdInit();
bool sdIsMounted();
std::vector<String> sdListDir(const char* path);
bool sdReadFile(const char* path, String& outContent);
bool sdWriteFile(const char* path, const String& content);
bool sdAppendLine(const char* path, const String& line);
bool sdDeleteFile(const char* path);
bool sdWriteFile(const char* path, const uint8_t* data, size_t len);
bool sdAppendFile(const char* path, const uint8_t* data, size_t len);