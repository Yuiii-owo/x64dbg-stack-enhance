#include "util.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace util
{
std::string WideToUtf8(const std::wstring& w)
{
    if(w.empty())
        return std::string();
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if(len <= 0)
        return std::string();
    std::string out((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s)
{
    if(s.empty())
        return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if(len <= 0)
        return std::wstring();
    std::wstring out((size_t)len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], len);
    return out;
}

std::string Format(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = _vscprintf(fmt, ap);
    va_end(ap);
    if(needed < 0)
    {
        va_end(ap2);
        return std::string();
    }
    std::string out((size_t)needed + 1, '\0');
    vsnprintf(&out[0], out.size(), fmt, ap2);
    va_end(ap2);
    out.resize((size_t)needed);
    return out;
}

std::string Trim(const std::string& s)
{
    size_t b = 0;
    size_t e = s.size();
    while(b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        b++;
    while(e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        e--;
    return s.substr(b, e - b);
}

std::string ToLower(std::string s)
{
    for(char& c : s)
    {
        if(c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    }
    return s;
}

std::string HexPtr(duint v)
{
    return Format(sizeof(duint) == 8 ? "0x%016llx" : "0x%08llx", (unsigned long long)v);
}

std::string HexValue(duint v)
{
    return Format("0x%llx", (unsigned long long)v);
}

std::string HexOffset(dsint off)
{
    unsigned long long a = off < 0 ? (unsigned long long)(-(long long)off) : (unsigned long long)off;
    char num[32];
    snprintf(num, sizeof(num), "%llX", a);
    std::string s = off < 0 ? "-" : "+";
    int pad = 3 - (int)strlen(num);
    if(pad > 0)
        s.append((size_t)pad, '0');
    s += num;
    return s;
}

std::string TruncateUtf8(const std::string& s, size_t maxLen)
{
    if(s.size() <= maxLen)
        return s;
    if(maxLen <= 3)
        return std::string();
    size_t n = maxLen - 3;
    while(n > 0 && (((unsigned char)s[n]) & 0xC0) == 0x80)
        n--;
    return s.substr(0, n) + "...";
}

bool AppendBounded(std::string& s, const std::string& part, size_t maxLen)
{
    if(s.size() + part.size() > maxLen)
    {
        if(s.size() + 3 <= maxLen)
            s += "...";
        return false;
    }
    s += part;
    return true;
}

bool ReadPtr(duint addr, duint& value)
{
    duint v = 0;
    if(!DbgMemRead(addr, &v, sizeof(duint)))
        return false;
    value = v;
    return true;
}

bool WriteUtf8File(const std::wstring& path, const std::string& data, std::string& error)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if(h == INVALID_HANDLE_VALUE)
    {
        error = Format("CreateFile 失败: %lu", (unsigned long)GetLastError());
        return false;
    }
    size_t written = 0;
    while(written < data.size())
    {
        DWORD chunk = (DWORD)((data.size() - written) > 0x10000000 ? 0x10000000 : (data.size() - written));
        DWORD got = 0;
        if(!WriteFile(h, data.data() + written, chunk, &got, nullptr) || got == 0)
        {
            error = Format("WriteFile 失败: %lu", (unsigned long)GetLastError());
            CloseHandle(h);
            return false;
        }
        written += got;
    }
    CloseHandle(h);
    return true;
}

bool CopyUtf8ToClipboard(const std::string& text, std::string& error)
{
    std::wstring w = Utf8ToWide(text);
    if(!OpenClipboard(nullptr))
    {
        error = Format("OpenClipboard 失败: %lu", (unsigned long)GetLastError());
        return false;
    }
    EmptyClipboard();
    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if(!h)
    {
        CloseClipboard();
        error = "GlobalAlloc 失败";
        return false;
    }
    void* p = GlobalLock(h);
    if(!p)
    {
        GlobalFree(h);
        CloseClipboard();
        error = "GlobalLock 失败";
        return false;
    }
    memcpy(p, w.c_str(), bytes);
    GlobalUnlock(h);
    if(!SetClipboardData(CF_UNICODETEXT, h))
    {
        GlobalFree(h);
        CloseClipboard();
        error = Format("SetClipboardData 失败: %lu", (unsigned long)GetLastError());
        return false;
    }
    CloseClipboard();
    return true;
}
}
