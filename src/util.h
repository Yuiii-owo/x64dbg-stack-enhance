#pragma once

#include "plugin.h"

namespace util
{
std::string  WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::string  Format(const char* fmt, ...);
std::string  Trim(const std::string& s);
std::string  ToLower(std::string s);
std::string  HexPtr(duint v);
std::string  HexValue(duint v);
std::string  HexOffset(dsint off);
std::string  TruncateUtf8(const std::string& s, size_t maxLen);
bool         AppendBounded(std::string& s, const std::string& part, size_t maxLen);
bool         ReadPtr(duint addr, duint& value);
bool         WriteUtf8File(const std::wstring& path, const std::string& data, std::string& error);
bool         CopyUtf8ToClipboard(const std::string& text, std::string& error);
}
