#include "hook.h"
#include "stackcomment.h"
#include "util.h"

#include <tlhelp32.h>

#include <atomic>
#include <cstring>
#include <cwctype>
#include <vector>

namespace hook
{
namespace
{
struct Slot
{
    void**      slot = nullptr;
    void*       original = nullptr;
    std::string dll;
};

std::atomic<uint64_t> g_calls { 0 };
std::atomic<int>      g_inHook { 0 };
std::atomic<bool>     g_installed { false };
std::atomic<bool>     g_enabled { true };
StackCommentFn        g_original = nullptr;
std::vector<Slot>     g_slots;
std::wstring          g_guiModule;
std::string           g_lastError;
uint64_t              g_loadId = 0;

bool __cdecl StackCommentHook(duint addr, STACK_COMMENT* comment);

#ifdef _WIN64
constexpr const wchar_t* kGuiModule = L"x64gui.dll";
constexpr const wchar_t* kBridgeModule = L"x64bridge.dll";
#else
constexpr const wchar_t* kGuiModule = L"x32gui.dll";
constexpr const wchar_t* kBridgeModule = L"x32bridge.dll";
#endif

std::string ModuleName(HMODULE mod)
{
    wchar_t path[MAX_PATH] = {};
    if(!GetModuleFileNameW(mod, path, MAX_PATH))
        return std::string();
    const wchar_t* name = wcsrchr(path, L'\\');
    return util::WideToUtf8(name ? name + 1 : path);
}

HMODULE FindGuiModule()
{
    HMODULE mod = GetModuleHandleW(kGuiModule);
    if(mod)
        return mod;

    // Fallback for renamed/custom GUI modules: pick a loaded module whose name
    // contains "gui" and which imports DbgStackCommentGet.
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if(snap == INVALID_HANDLE_VALUE)
        return nullptr;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    HMODULE found = nullptr;
    if(Module32FirstW(snap, &me))
    {
        do
        {
            std::wstring name = me.szModule;
            for(wchar_t& c : name)
                c = (wchar_t)towlower(c);
            if(name.find(L"gui") != std::wstring::npos)
            {
                found = me.hModule;
                break;
            }
        }
        while(Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

bool PatchGuiIat(HMODULE gui, std::vector<Slot>& slots, std::string& error)
{
    auto* base = (BYTE*)gui;
    auto* dos = (IMAGE_DOS_HEADER*)base;
    if(dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        error = "GUI 模块不是有效 PE";
        return false;
    }
    auto* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if(nt->Signature != IMAGE_NT_SIGNATURE)
    {
        error = "GUI 模块 NT 头无效";
        return false;
    }

    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!dir.VirtualAddress || !dir.Size)
    {
        error = "GUI 模块没有导入表";
        return false;
    }

    HMODULE bridge = GetModuleHandleW(kBridgeModule);
    FARPROC bridgeFunc = bridge ? GetProcAddress(bridge, "DbgStackCommentGet") : nullptr;

    auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    for(; imp->Name; ++imp)
    {
        const char* dllName = (const char*)(base + imp->Name);
        auto* oft = imp->OriginalFirstThunk ? (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk) : nullptr;
        auto* ft = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for(size_t i = 0; ft[i].u1.Function; ++i)
        {
            bool match = false;
            if(oft)
            {
                auto& thunk = oft[i];
                if(IMAGE_SNAP_BY_ORDINAL(thunk.u1.Ordinal))
                {
                    if(IMAGE_ORDINAL(thunk.u1.Ordinal) == 77)
                        match = true;
                }
                else if(thunk.u1.AddressOfData)
                {
                    auto* byName = (IMAGE_IMPORT_BY_NAME*)(base + thunk.u1.AddressOfData);
                    if(byName->Name[0] && strcmp((const char*)byName->Name, "DbgStackCommentGet") == 0)
                        match = true;
                }
            }
            if(!match && bridgeFunc && ft[i].u1.Function == (ULONG_PTR)bridgeFunc)
                match = true;

            if(!match)
                continue;

            Slot s;
            s.slot = (void**)(base + imp->FirstThunk + i * sizeof(void*));
            s.original = *s.slot;
            s.dll = dllName ? dllName : "";
            if(!s.original)
                continue;
            slots.push_back(s);
        }
    }

    if(slots.empty())
    {
        error = "GUI 导入表中没有 DbgStackCommentGet";
        return false;
    }
    return true;
}

bool PatchSlots(std::vector<Slot>& slots, void* replacement, std::string& error)
{
    size_t patched = 0;
    for(auto& s : slots)
    {
        DWORD oldProtect = 0;
        if(!VirtualProtect(s.slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            error = util::Format("VirtualProtect 失败: %lu", (unsigned long)GetLastError());
            break;
        }
        void* prev = InterlockedExchangePointer(s.slot, replacement);
        DWORD ignored = 0;
        VirtualProtect(s.slot, sizeof(void*), oldProtect, &ignored);
        if(prev != s.original)
        {
            // Another plugin changed it concurrently; restore and abort.
            DWORD p2 = 0;
            if(VirtualProtect(s.slot, sizeof(void*), PAGE_READWRITE, &p2))
            {
                InterlockedExchangePointer(s.slot, s.original);
                VirtualProtect(s.slot, sizeof(void*), p2, &ignored);
            }
            error = "IAT 槽在安装期间被其他插件修改";
            break;
        }
        patched++;
    }
    if(patched != slots.size())
    {
        // Roll back everything we patched.
        for(size_t i = 0; i < patched; ++i)
        {
            DWORD p = 0;
            if(VirtualProtect(slots[i].slot, sizeof(void*), PAGE_READWRITE, &p))
            {
                InterlockedExchangePointer(slots[i].slot, slots[i].original);
                VirtualProtect(slots[i].slot, sizeof(void*), p, &p);
            }
        }
        return false;
    }
    return true;
}

bool __cdecl StackCommentHook(duint addr, STACK_COMMENT* comment)
{
    g_calls.fetch_add(1, std::memory_order_relaxed);
    struct Guard
    {
        Guard() { g_inHook.fetch_add(1, std::memory_order_relaxed); }
        ~Guard() { g_inHook.fetch_sub(1, std::memory_order_relaxed); }
    } guard;

    if(!comment || !g_original)
        return false;

    if(!g_enabled.load(std::memory_order_relaxed))
        return g_original(addr, comment);

    try
    {
        stackcomment::BuiltComment bc = stackcomment::BuildForHook(addr, g_original);
        if(bc.empty())
            return g_original(addr, comment);

        *comment = {};
        strncpy_s(comment->comment, sizeof(comment->comment), bc.text.c_str(), _TRUNCATE);
        strncpy_s(comment->color, sizeof(comment->color), bc.color.c_str(), _TRUNCATE);
        return true;
    }
    catch(...)
    {
        return g_original(addr, comment);
    }
}
}

bool Install()
{
    if(g_installed.load())
        return true;

    HMODULE gui = FindGuiModule();
    if(!gui)
    {
        g_lastError = "找不到 GUI 模块（x64gui.dll/x32gui.dll）";
        return false;
    }
    g_guiModule = util::Utf8ToWide(ModuleName(gui));

    std::vector<Slot> slots;
    std::string error;
    if(!PatchGuiIat(gui, slots, error))
    {
        g_lastError = error;
        return false;
    }

    if(!PatchSlots(slots, (void*)&StackCommentHook, error))
    {
        g_lastError = error;
        return false;
    }

    g_slots = std::move(slots);
    g_original = (StackCommentFn)g_slots.front().original;
    g_installed.store(true);
    g_lastError.clear();

    _plugin_logprintf("[StackEnhance] IAT hook 安装成功: gui=%s slots=%u original=%p slot=%p\n",
                      util::WideToUtf8(g_guiModule).c_str(), (unsigned)g_slots.size(),
                      (void*)g_original, (void*)g_slots.front().slot);
    return true;
}

bool Shutdown()
{
    g_enabled.store(false);
    if(!g_installed.load())
        return true;

    bool ok = true;
    size_t restored = 0;
    for(auto& s : g_slots)
    {
        DWORD oldProtect = 0;
        if(!VirtualProtect(s.slot, sizeof(void*), PAGE_READWRITE, &oldProtect))
        {
            g_lastError = util::Format("恢复 IAT 时 VirtualProtect 失败: %lu", (unsigned long)GetLastError());
            ok = false;
            continue;
        }
        void* prev = InterlockedCompareExchangePointer(s.slot, s.original, (void*)&StackCommentHook);
        DWORD ignored = 0;
        VirtualProtect(s.slot, sizeof(void*), oldProtect, &ignored);
        if(prev != (void*)&StackCommentHook)
        {
            g_lastError = "恢复 IAT 时发现槽已被其他插件修改";
            ok = false;
            continue;
        }
        restored++;
    }

    if(ok)
        g_installed.store(false);

    const int inHook = g_inHook.load(std::memory_order_relaxed);
    _plugin_logprintf("[StackEnhance] IAT hook 卸载: restored=%u/%u inHook=%d ok=%d\n",
                      (unsigned)restored, (unsigned)g_slots.size(), inHook, ok ? 1 : 0);

    // Do not wait here: plugstop runs on the debugger thread and the GUI may be
    // blocked inside a synchronous bridge call. If a hook is still on the stack,
    // tell the host to keep the DLL loaded; the next unload can retry.
    return ok && inHook == 0;
}

bool IsInstalled()
{
    return g_installed.load();
}

void SetEnabled(bool enabled)
{
    g_enabled.store(enabled);
}

bool IsEnabled()
{
    return g_enabled.load();
}

uint64_t GetCallCount()
{
    return g_calls.load(std::memory_order_relaxed);
}

int GetInHookCount()
{
    return g_inHook.load(std::memory_order_relaxed);
}

void SetLoadId(uint64_t id)
{
    g_loadId = id;
}

std::string StatusText()
{
    std::string out;
    out += util::Format("StackEnhance %d.%02d (%s)\n", PLUGIN_VERSION / 100, PLUGIN_VERSION % 100, SE_ARCH);
    out += util::Format("loadId=0x%llx\n", (unsigned long long)g_loadId);
    out += util::Format("enabled=%d\n", g_enabled.load() ? 1 : 0);
    out += util::Format("hookInstalled=%d\n", g_installed.load() ? 1 : 0);
    out += util::Format("hookCalls=%llu\n", (unsigned long long)g_calls.load());
    out += util::Format("inHook=%d\n", g_inHook.load());
    out += util::Format("iatSlots=%u\n", (unsigned)g_slots.size());
    if(!g_slots.empty())
    {
        out += util::Format("iatSlot=%p\n", (void*)g_slots.front().slot);
        out += util::Format("original=%p\n", g_slots.front().original);
        out += util::Format("iatDll=%s\n", g_slots.front().dll.c_str());
    }
    out += util::Format("guiModule=%s\n", util::WideToUtf8(g_guiModule).c_str());
    out += util::Format("paused=%d\n", stackcomment::IsPaused() ? 1 : 0);
    if(DbgIsDebugging())
    {
        out += util::Format("%s=%s\n", SE_IP, util::HexPtr(DbgValFromString("cip")).c_str());
        out += util::Format("%s=%s\n", SE_SP, util::HexPtr(DbgValFromString("csp")).c_str());
        out += util::Format("%s=%s\n", SE_BP, util::HexPtr(DbgValFromString("cbp")).c_str());
    }
    if(!g_lastError.empty())
        out += "lastError=" + g_lastError + "\n";
    return out;
}

bool WriteStatus(const std::wstring& path, std::string& error)
{
    return util::WriteUtf8File(path, StatusText(), error);
}
}
