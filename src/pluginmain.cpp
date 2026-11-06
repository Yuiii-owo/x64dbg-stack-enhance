#include "plugin.h"
#include "hook.h"
#include "stackcomment.h"
#include "util.h"

#include <algorithm>
#include <cstring>

Settings            g_settings;
HINSTANCE           g_hInst = nullptr;
int                 g_pluginHandle = 0;
const DBGFUNCTIONS* g_DbgF = nullptr;
std::atomic<bool>   g_paused { true };

namespace
{
enum MenuId
{
    MENU_ENABLE = 1,
    MENU_CLEAR = 2,
    MENU_COPY = 3,
    MENU_HELP = 4,
};

uint64_t g_loadId = 0;
bool g_hookFailureLogged = false;

bool SettingBool(const char* key, bool def)
{
    duint v = 0;
    if(!BridgeSettingGetUint(PLUG_SECTION, key, &v))
        return def;
    return v != 0;
}

int SettingInt(const char* key, int def)
{
    duint v = 0;
    if(!BridgeSettingGetUint(PLUG_SECTION, key, &v))
        return def;
    return (int)v;
}

void SaveBool(const char* key, bool value)
{
    BridgeSettingSetUint(PLUG_SECTION, key, value ? 1 : 0);
}

void SaveInt(const char* key, int value)
{
    BridgeSettingSetUint(PLUG_SECTION, key, (duint)value);
}

void RefreshViews()
{
    stackcomment::InvalidateCache();
    GuiUpdateAllViews();
}

void UpdateMenuCheck()
{
    if(g_pluginHandle)
        _plugin_menuentrysetchecked(g_pluginHandle, MENU_ENABLE, g_settings.enabled);
}

void LogHelp()
{
    _plugin_logputs("[StackEnhance] 命令: stackenhance on|off|toggle | markers on|off | "
                    "depth <1..6> | strings on|off | disasm on|off | color on|off | "
                    "clearcache | status [path] | text <path> | copy | help");
    _plugin_logputs("[StackEnhance] 增强输出在自带 Stack 视图的 Comments 列；"
                    "Address/Data 列保持原生。");
}

void SplitSub(const std::string& rest, std::string& sub, std::string& args)
{
    size_t p = rest.find_first_of(" \t");
    if(p == std::string::npos)
    {
        sub = util::ToLower(rest);
        args.clear();
    }
    else
    {
        sub = util::ToLower(rest.substr(0, p));
        args = util::Trim(rest.substr(p + 1));
    }
}

std::string StripQuotes(const std::string& s)
{
    if(s.size() >= 2)
    {
        char a = s.front();
        char b = s.back();
        if((a == '"' && b == '"') || (a == '\'' && b == '\''))
            return s.substr(1, s.size() - 2);
    }
    return s;
}

bool ParseOnOff(const std::string& arg, bool& value)
{
    std::string a = util::ToLower(util::Trim(arg));
    if(a == "on" || a == "1" || a == "true" || a == "yes")
    {
        value = true;
        return true;
    }
    if(a == "off" || a == "0" || a == "false" || a == "no")
    {
        value = false;
        return true;
    }
    return false;
}

void ApplyEnabled(bool enabled, const char* source)
{
    g_settings.enabled = enabled;
    hook::SetEnabled(enabled);
    SaveBool("Enabled", enabled);
    UpdateMenuCheck();
    RefreshViews();
    _plugin_logprintf("[StackEnhance] 增强已%s (%s)\n", enabled ? "启用" : "禁用", source);
    GuiAddStatusBarMessage(enabled ? "Stack Enhance: 已启用" : "Stack Enhance: 已禁用");
}

bool WriteStatus(const std::string& path)
{
    std::string text = hook::StatusText();
    if(path.empty())
    {
        _plugin_logprint(text.c_str());
        GuiAddStatusBarMessage("Stack Enhance: 状态已写入日志");
        return true;
    }
    std::string err;
    if(!util::WriteUtf8File(util::Utf8ToWide(StripQuotes(path)), text, err))
    {
        _plugin_logprintf("[StackEnhance] 写状态失败: %s\n", err.c_str());
        return false;
    }
    _plugin_logprintf("[StackEnhance] 状态已写入 %s\n", StripQuotes(path).c_str());
    GuiAddStatusBarMessage("Stack Enhance: 状态已写入文件");
    return true;
}

bool WriteText(const std::string& path)
{
    std::string clean = StripQuotes(path);
    if(clean.empty())
    {
        _plugin_logputs("[StackEnhance] 用法: stackenhance text <path>");
        return false;
    }
    std::string text = stackcomment::BuildText();
    std::string err;
    if(!util::WriteUtf8File(util::Utf8ToWide(clean), text, err))
    {
        _plugin_logprintf("[StackEnhance] 写文本失败: %s\n", err.c_str());
        return false;
    }
    _plugin_logprintf("[StackEnhance] 栈注释文本已写入 %s\n", clean.c_str());
    GuiAddStatusBarMessage("Stack Enhance: 栈注释文本已写入文件");
    return true;
}

void Toggle()
{
    ApplyEnabled(!g_settings.enabled, "menu/command");
}

void CbMenuEntry(CBTYPE cbType, void* callbackInfo)
{
    (void)cbType;
    auto* info = (PLUG_CB_MENUENTRY*)callbackInfo;
    if(!info)
        return;
    switch(info->hEntry)
    {
    case MENU_ENABLE:
        Toggle();
        break;
    case MENU_CLEAR:
        stackcomment::InvalidateCache();
        GuiUpdateAllViews();
        _plugin_logputs("[StackEnhance] 注释缓存已清空");
        GuiAddStatusBarMessage("Stack Enhance: 缓存已清空");
        break;
    case MENU_COPY:
    {
        std::string err;
        if(!stackcomment::CopyText(err))
            _plugin_logprintf("[StackEnhance] 复制失败: %s\n", err.c_str());
        else
            GuiAddStatusBarMessage("Stack Enhance: 栈注释已复制到剪贴板");
        break;
    }
    case MENU_HELP:
        LogHelp();
        GuiAddStatusBarMessage("Stack Enhance: 帮助已写入日志");
        break;
    default:
        break;
    }
}

void CbDebug(CBTYPE cbType, void* callbackInfo)
{
    (void)callbackInfo;
    switch(cbType)
    {
    case CB_INITDEBUG:
    case CB_PAUSEDEBUG:
    case CB_BREAKPOINT:
    case CB_STEPPED:
        stackcomment::SetPaused(true);
        stackcomment::OnDebugStateChanged();
        if(g_settings.enabled && !hook::IsInstalled())
        {
            if(!hook::Install() && !g_hookFailureLogged)
            {
                g_hookFailureLogged = true;
                _plugin_logputs("[StackEnhance] IAT hook 安装失败；使用 stackenhance status 查看原因");
            }
        }
        break;
    case CB_RESUMEDEBUG:
    case CB_STOPDEBUG:
        stackcomment::SetPaused(false);
        stackcomment::OnDebugStateChanged();
        break;
    default:
        break;
    }
}

bool CmdStackEnhance(int argc, char** argv)
{
    std::string raw;
    if(argc >= 1 && argv && argv[0])
        raw = argv[0];
    raw = util::Trim(raw);

    size_t sp = raw.find_first_of(" \t");
    std::string rest;
    if(sp != std::string::npos)
        rest = util::Trim(raw.substr(sp + 1));
    else if(argc > 1)
    {
        for(int i = 1; i < argc; ++i)
        {
            if(i > 1)
                rest += " ";
            if(argv[i])
                rest += argv[i];
        }
        rest = util::Trim(rest);
    }

    std::string sub;
    std::string args;
    SplitSub(rest, sub, args);

    if(sub.empty() || sub == "help")
    {
        LogHelp();
        return true;
    }
    if(sub == "on" || sub == "off")
    {
        ApplyEnabled(sub == "on", "command");
        return true;
    }
    if(sub == "toggle")
    {
        Toggle();
        return true;
    }
    if(sub == "markers" || sub == "strings" || sub == "disasm" || sub == "color")
    {
        bool value = false;
        if(!ParseOnOff(args, value))
        {
            _plugin_logprintf("[StackEnhance] 用法: stackenhance %s on|off\n", sub.c_str());
            return false;
        }
        if(sub == "markers")
        {
            g_settings.markers = value;
            SaveBool("Markers", value);
        }
        else if(sub == "strings")
        {
            g_settings.showStrings = value;
            SaveBool("ShowStrings", value);
        }
        else if(sub == "disasm")
        {
            g_settings.showDisasm = value;
            SaveBool("ShowDisasm", value);
        }
        else
        {
            g_settings.colorize = value;
            SaveBool("Colorize", value);
        }
        RefreshViews();
        _plugin_logprintf("[StackEnhance] %s=%d\n", sub.c_str(), value ? 1 : 0);
        return true;
    }
    if(sub == "depth")
    {
        int v = atoi(args.c_str());
        if(v < 1 || v > 8)
        {
            _plugin_logputs("[StackEnhance] 用法: stackenhance depth <1..8>");
            return false;
        }
        g_settings.depth = v;
        SaveInt("Depth", v);
        RefreshViews();
        _plugin_logprintf("[StackEnhance] depth=%d\n", v);
        return true;
    }
    if(sub == "clearcache")
    {
        stackcomment::InvalidateCache();
        GuiUpdateAllViews();
        _plugin_logputs("[StackEnhance] 注释缓存已清空");
        return true;
    }
    if(sub == "status")
        return WriteStatus(args);
    if(sub == "text")
        return WriteText(args);
    if(sub == "copy")
    {
        std::string err;
        if(!stackcomment::CopyText(err))
        {
            _plugin_logprintf("[StackEnhance] 复制失败: %s\n", err.c_str());
            return false;
        }
        _plugin_logputs("[StackEnhance] 栈注释已复制到剪贴板");
        GuiAddStatusBarMessage("Stack Enhance: 栈注释已复制到剪贴板");
        return true;
    }

    _plugin_logprintf("[StackEnhance] 未知子命令: %s\n", sub.c_str());
    LogHelp();
    return false;
}
}

void ConfigLoad()
{
    g_settings.enabled = SettingBool("Enabled", true);
    g_settings.markers = SettingBool("Markers", true);
    g_settings.showStrings = SettingBool("ShowStrings", true);
    g_settings.showDisasm = SettingBool("ShowDisasm", true);
    g_settings.colorize = SettingBool("Colorize", true);
    g_settings.depth = std::clamp(SettingInt("Depth", 3), 1, 8);
    g_settings.maxComment = std::clamp(SettingInt("MaxComment", 480), 64, 500);
    g_settings.textRowsAbove = std::clamp(SettingInt("TextRowsAbove", 64), 0, 512);
    g_settings.textRowsBelow = std::clamp(SettingInt("TextRowsBelow", 256), 0, 1024);
    g_settings.cacheTtlMs = std::clamp(SettingInt("CacheTtlMs", 300), 0, 5000);
}

void ConfigSave()
{
    SaveBool("Enabled", g_settings.enabled);
    SaveBool("Markers", g_settings.markers);
    SaveBool("ShowStrings", g_settings.showStrings);
    SaveBool("ShowDisasm", g_settings.showDisasm);
    SaveBool("Colorize", g_settings.colorize);
    SaveInt("Depth", g_settings.depth);
    SaveInt("MaxComment", g_settings.maxComment);
    SaveInt("TextRowsAbove", g_settings.textRowsAbove);
    SaveInt("TextRowsBelow", g_settings.textRowsBelow);
    SaveInt("CacheTtlMs", g_settings.cacheTtlMs);
}

PLUG_EXPORT bool pluginit(PLUG_INITSTRUCT* initStruct)
{
    if(!initStruct)
        return false;

    initStruct->sdkVersion = PLUG_SDKVERSION;
    initStruct->pluginVersion = PLUGIN_VERSION;
    strcpy_s(initStruct->pluginName, sizeof(initStruct->pluginName), PLUGIN_NAME_UTF8);

    g_pluginHandle = initStruct->pluginHandle;
    g_DbgF = DbgFunctions();
    ConfigLoad();

    g_loadId = GetTickCount64() ^ ((uint64_t)(uintptr_t)&g_pluginHandle << 32);
    hook::SetLoadId(g_loadId);
    hook::SetEnabled(g_settings.enabled);
    stackcomment::SetPaused(true);

    _plugin_registercallback(g_pluginHandle, CB_MENUENTRY, CbMenuEntry);
    _plugin_registercallback(g_pluginHandle, CB_INITDEBUG, CbDebug);
    _plugin_registercallback(g_pluginHandle, CB_PAUSEDEBUG, CbDebug);
    _plugin_registercallback(g_pluginHandle, CB_BREAKPOINT, CbDebug);
    _plugin_registercallback(g_pluginHandle, CB_STEPPED, CbDebug);
    _plugin_registercallback(g_pluginHandle, CB_RESUMEDEBUG, CbDebug);
    _plugin_registercallback(g_pluginHandle, CB_STOPDEBUG, CbDebug);

    bool cmd = _plugin_registercommand(g_pluginHandle, "stackenhance", CmdStackEnhance, false);
    _plugin_logprintf("[StackEnhance] v%d.%02d (%s) 已加载，命令%s\n",
                      PLUGIN_VERSION / 100, PLUGIN_VERSION % 100, SE_ARCH,
                      cmd ? "注册成功" : "注册失败");
    return true;
}

PLUG_EXPORT void plugsetup(PLUG_SETUPSTRUCT* setupStruct)
{
    if(!setupStruct)
        return;

    _plugin_menuaddentry(setupStruct->hMenu, MENU_ENABLE, "启用栈注释增强");
    _plugin_menuaddentry(setupStruct->hMenu, MENU_CLEAR, "刷新注释缓存");
    _plugin_menuaddentry(setupStruct->hMenu, MENU_COPY, "复制当前栈注释");
    _plugin_menuaddentry(setupStruct->hMenu, MENU_HELP, "帮助/状态");
    _plugin_menuentrysetchecked(g_pluginHandle, MENU_ENABLE, g_settings.enabled);
    _plugin_menuentrysethotkey(g_pluginHandle, MENU_ENABLE, "Ctrl+Alt+S");

    if(g_settings.enabled)
    {
        if(hook::Install())
        {
            _plugin_logputs("[StackEnhance] 已启用原生 Stack 视图 Comments 列增强");
        }
        else
        {
            _plugin_logputs("[StackEnhance] 启用失败：未找到可挂钩的 GUI 导入项");
            g_hookFailureLogged = true;
        }
    }
    GuiUpdateAllViews();
}

PLUG_EXPORT bool plugstop()
{
    ConfigSave();
    _plugin_unregistercommand(g_pluginHandle, "stackenhance");
    bool safe = hook::Shutdown();
    _plugin_logprintf("[StackEnhance] 卸载: safe=%d\n", safe ? 1 : 0);
    return safe;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if(reason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}
