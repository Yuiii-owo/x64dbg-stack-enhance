#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>

#include "bridgemain.h"
#include "_plugins.h"
#include "_dbgfunctions.h"

#define PLUGIN_NAME_UTF8 "Stack Enhance（栈注释增强）"
#define PLUGIN_VERSION   100
#define PLUG_SECTION     "StackEnhance"

#ifdef _WIN64
#define SE_ARCH          "x64"
#define SE_SP            "rsp"
#define SE_BP            "rbp"
#define SE_IP            "rip"
constexpr int SE_PTR_SIZE = 8;
constexpr int SE_ADDR_HEX = 16;
#else
#define SE_ARCH          "x86"
#define SE_SP            "esp"
#define SE_BP            "ebp"
#define SE_IP            "eip"
constexpr int SE_PTR_SIZE = 4;
constexpr int SE_ADDR_HEX = 8;
#endif

#define PLUG_EXPORT extern "C" __declspec(dllexport)

struct Settings
{
    bool enabled      = true;
    bool markers      = true;
    bool showStrings  = true;
    bool showDisasm   = true;
    bool colorize     = true;
    int  depth        = 3;
    int  maxComment   = 480;
    int  textRowsAbove = 64;
    int  textRowsBelow = 256;
    int  cacheTtlMs   = 300;
};

extern Settings             g_settings;
extern HINSTANCE            g_hInst;
extern int                  g_pluginHandle;
extern const DBGFUNCTIONS*  g_DbgF;
extern std::atomic<bool>    g_paused;

void ConfigLoad();
void ConfigSave();
