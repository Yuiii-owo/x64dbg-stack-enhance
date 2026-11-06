#include "stackcomment.h"
#include "util.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace stackcomment
{
namespace
{
enum class Kind
{
    None,
    Code,
    String,
    Invalid,
    Stack,
    Data
};

struct TargetInfo
{
    bool        readable = false;
    bool        code = false;
    bool        isString = false;
    std::string symbol;
    std::string stringValue;
    std::string insn;
};

struct TargetCache
{
    std::unordered_map<duint, TargetInfo> map;
};

struct HookCache
{
    unsigned generation = 0;
    uint64_t ctxTick = 0;
    Context  ctx;
    std::unordered_map<duint, std::pair<BuiltComment, uint64_t>> comments;
    TargetCache targets;
};

std::atomic<unsigned> s_generation { 1 };
std::atomic<bool>     s_paused { true };
HookCache             s_hookCache; // GUI thread only

std::string SafeCopy(const char* p, size_t cap)
{
    if(!p)
        return std::string();
    size_t n = 0;
    while(n < cap && p[n])
        n++;
    return std::string(p, n);
}

Context AcquireContext()
{
    Context c;
    if(!DbgIsDebugging() || DbgIsRunning())
        return c;

    duint rsp = DbgValFromString("csp");
    if(!rsp)
        return c;

    duint size = 0;
    duint base = DbgMemFindBaseAddr(rsp, &size);
    if(!base || !size || rsp < base || rsp >= base + size)
        return c;

    duint rbp = DbgValFromString("cbp");
    c.valid = true;
    c.rsp = rsp;
    c.rbp = rbp;
    c.stackBase = base;
    c.stackSize = size;
    c.rbpInStack = rbp != 0 && rbp >= base && rbp < base + size &&
                   (rbp % sizeof(duint)) == 0;
    return c;
}

std::string MakeMarker(duint addr, const Context& c)
{
    if(!g_settings.markers || !c.valid)
        return std::string();

    const duint slot = sizeof(duint);
    if(addr % slot)
        return std::string();

    const bool inStack = addr >= c.stackBase && addr < c.stackEnd();
    const bool isRsp = addr == c.rsp;
    const bool isRbp = c.rbpInStack && addr == c.rbp;
    if(!inStack && !isRsp && !isRbp)
        return std::string();

    if(isRsp && isRbp)
        return std::string(SE_SP) + "/" + SE_BP + " | ";
    if(isRsp)
        return std::string(SE_SP) + " | ";
    if(isRbp)
        return std::string(SE_BP) + " | ";

    duint base = c.rbpInStack ? c.rbp : c.rsp;
    dsint off = (dsint)(addr - base);
    return util::HexOffset(off) + " | ";
}

std::string ColorFor(Kind k)
{
    if(!g_settings.colorize)
        return std::string();
    switch(k)
    {
    case Kind::Code:
        return "#E5C07B";
    case Kind::String:
        return "#C678DD";
    case Kind::Invalid:
        return "#E06C75";
    case Kind::Stack:
        return "#61AFEF";
    default:
        return std::string();
    }
}

std::string ShortenLabel(const std::string& label)
{
    std::string s = label;
    std::string suffix;

    size_t paren = s.find('(');
    if(paren != std::string::npos)
    {
        size_t close = s.rfind(')');
        if(close != std::string::npos && close + 1 < s.size())
            suffix = s.substr(close + 1);
        s = s.substr(0, paren);
    }

    static const char* conventions[] =
    {
        " __cdecl ", " __stdcall ", " __fastcall ", " __vectorcall ", " __thiscall "
    };
    for(const char* cc : conventions)
    {
        size_t p = s.find(cc);
        if(p != std::string::npos)
        {
            s = s.substr(p + strlen(cc));
            break;
        }
    }

    s = util::Trim(s);
    if(s.empty())
        s = label;
    return s + suffix;
}

TargetInfo ResolveTarget(duint addr)
{
    TargetInfo ti;
    ti.readable = DbgMemIsValidReadPtr(addr);
    if(!ti.readable)
        return ti;

    char module[MAX_MODULE_SIZE] = {};
    char label[MAX_LABEL_SIZE] = {};
    bool hasModule = DbgGetModuleAt(addr, module) && module[0];
    bool hasLabel = DbgGetLabelAt(addr, SEG_DEFAULT, label) && label[0];

    duint modBase = 0;
    if(hasModule && g_DbgF && g_DbgF->ModBaseFromAddr)
        modBase = g_DbgF->ModBaseFromAddr(addr);

    if(hasModule && hasLabel)
    {
        std::string shortLabel = ShortenLabel(label);
        std::string prefix = std::string(module) + ".";
        if(shortLabel.rfind(prefix, 0) == 0 || shortLabel.rfind(std::string(module) + "!", 0) == 0)
            ti.symbol = shortLabel;
        else
            ti.symbol = prefix + shortLabel;
    }
    else if(hasModule && modBase)
        ti.symbol = std::string(module) + "+0x" + util::Format("%llx", (unsigned long long)(addr - modBase));
    else if(hasLabel)
        ti.symbol = std::string("<") + ShortenLabel(label) + ">";

    if(g_DbgF && g_DbgF->MemIsCodePage)
        ti.code = g_DbgF->MemIsCodePage(addr, false);

    if(!ti.code && hasModule && g_DbgF && g_DbgF->SectionFromAddr)
    {
        char section[MAX_SECTION_SIZE] = {};
        if(g_DbgF->SectionFromAddr(addr, section))
        {
            std::string s = util::ToLower(section);
            if(s.find("code") != std::string::npos || s.find(".text") != std::string::npos)
                ti.code = true;
        }
    }

    if(!ti.code && g_settings.showStrings)
    {
        char text[MAX_STRING_SIZE] = {};
        if(DbgGetStringAt(addr, text))
        {
            std::string s = text;
            // &"..." / &L"..." means the address holds a pointer to a string;
            // the pointer chain will follow it, so do not treat it as a direct string here.
            bool direct = !s.empty() && (s[0] == '"' || (s[0] == 'L' && s.size() > 1 && s[1] == '"'));
            if(direct)
            {
                ti.isString = true;
                ti.stringValue = s;
            }
        }
    }

    if(ti.code && g_settings.showDisasm)
    {
        DISASM_INSTR instr = {};
        DbgDisasmAt(addr, &instr);
        std::string insn = util::Trim(instr.instruction);
        if(!insn.empty() && insn != "???")
            ti.insn = insn;
    }

    return ti;
}

TargetInfo& GetTarget(TargetCache& tc, duint addr)
{
    auto it = tc.map.find(addr);
    if(it != tc.map.end())
        return it->second;
    if(tc.map.size() > 8192)
        tc.map.clear();
    auto res = tc.map.emplace(addr, ResolveTarget(addr));
    return res.first->second;
}

std::string BuildChain(duint addr, const Context& c, TargetCache& tc, Kind& kind, size_t maxLen)
{
    duint v = 0;
    if(!util::ReadPtr(addr, v))
        return std::string();
    if(v == 0 || v < 0x10000)
        return std::string();

    if(!DbgMemIsValidReadPtr(v))
    {
        kind = Kind::Invalid;
        return util::TruncateUtf8("→ " + util::HexPtr(v) + " (invalid)", maxLen);
    }

    const bool startsInStack = v >= c.stackBase && v < c.stackEnd();
    std::unordered_set<duint> seen;
    std::string out;
    duint cur = v;

    for(int depth = 0; depth < g_settings.depth; ++depth)
    {
        if(!seen.insert(cur).second)
        {
            util::AppendBounded(out, " (cycle)", maxLen);
            break;
        }
        if(!util::AppendBounded(out, (depth == 0 ? "→ " : " → ") + util::HexPtr(cur), maxLen))
            break;

        TargetInfo& ti = GetTarget(tc, cur);
        if(!ti.readable)
        {
            util::AppendBounded(out, " (unreadable)", maxLen);
            kind = Kind::Invalid;
            break;
        }

        const bool inStack = cur >= c.stackBase && cur < c.stackEnd();

        if(ti.code)
        {
            if(!ti.symbol.empty())
                util::AppendBounded(out, " (" + ti.symbol + ")", maxLen);
            if(g_settings.showDisasm && !ti.insn.empty())
                util::AppendBounded(out, " ← " + ti.insn, maxLen);
            kind = Kind::Code;
            break;
        }

        if(ti.isString)
        {
            util::AppendBounded(out, " ← " + ti.stringValue, maxLen);
            kind = Kind::String;
            break;
        }

        if(inStack)
        {
            duint off = cur >= c.rsp ? (cur - c.rsp) : (c.rsp - cur);
            std::string tag = std::string(" [stack") + (cur >= c.rsp ? "+" : "-") +
                              util::Format("%llx", (unsigned long long)off) + "]";
            util::AppendBounded(out, tag, maxLen);
            if(kind == Kind::None)
                kind = Kind::Stack;
        }
        else if(!ti.symbol.empty())
        {
            util::AppendBounded(out, " (" + ti.symbol + ")", maxLen);
        }

        duint next = 0;
        if(!util::ReadPtr(cur, next))
            break;
        if(next == 0)
        {
            util::AppendBounded(out, " ← 0", maxLen);
            break;
        }
        if(next < 0x10000)
        {
            util::AppendBounded(out, " ← " + util::HexValue(next), maxLen);
            break;
        }
        if(!DbgMemIsValidReadPtr(next))
        {
            util::AppendBounded(out, " ← " + util::HexPtr(next) + " (invalid)", maxLen);
            kind = Kind::Invalid;
            break;
        }
        cur = next;
    }

    if(kind == Kind::None)
        kind = startsInStack ? Kind::Stack : Kind::Data;
    return out;
}

BuiltComment BuildComment(duint addr, const Context& c, OriginalFn original, bool addMarker,
                          bool enhance, TargetCache& tc)
{
    STACK_COMMENT orig = {};
    bool hasOrig = false;
    if(original)
        hasOrig = original(addr, &orig);

    std::string origText;
    std::string origColor;
    if(hasOrig)
    {
        origText = SafeCopy(orig.comment, sizeof(orig.comment));
        origColor = SafeCopy(orig.color, sizeof(orig.color));
    }

    const bool special = !origColor.empty() && origColor[0] == '!';
    std::string marker = addMarker ? MakeMarker(addr, c) : std::string();

    if(special)
    {
        BuiltComment r;
        r.text = util::TruncateUtf8(marker + origText, (size_t)std::max(1, g_settings.maxComment));
        r.color = origColor;
        return r;
    }

    Kind kind = Kind::None;
    std::string chain;
    if(enhance && c.valid)
    {
        size_t budget = marker.size() < (size_t)g_settings.maxComment
                        ? (size_t)g_settings.maxComment - marker.size()
                        : 0;
        chain = BuildChain(addr, c, tc, kind, budget);
    }

    if(!chain.empty())
    {
        BuiltComment r;
        r.text = util::TruncateUtf8(marker + chain, (size_t)std::max(1, g_settings.maxComment));
        r.color = ColorFor(kind);
        return r;
    }

    if(!origText.empty())
    {
        BuiltComment r;
        r.text = util::TruncateUtf8(marker + origText, (size_t)std::max(1, g_settings.maxComment));
        r.color = origColor;
        return r;
    }

    BuiltComment r;
    r.text = marker;
    return r;
}
}

void OnDebugStateChanged()
{
    s_generation.fetch_add(1, std::memory_order_relaxed);
}

void SetPaused(bool paused)
{
    s_paused.store(paused, std::memory_order_relaxed);
    if(!paused)
        s_generation.fetch_add(1, std::memory_order_relaxed);
}

bool IsPaused()
{
    return s_paused.load(std::memory_order_relaxed);
}

BuiltComment BuildForHook(duint addr, OriginalFn original)
{
    if(!s_paused.load(std::memory_order_relaxed) || !g_settings.enabled)
        return BuiltComment();

    const unsigned gen = s_generation.load(std::memory_order_relaxed);
    const uint64_t now = GetTickCount64();

    if(gen != s_hookCache.generation)
    {
        s_hookCache.comments.clear();
        s_hookCache.targets.map.clear();
        s_hookCache.generation = gen;
        s_hookCache.ctxTick = 0;
    }

    if(!s_hookCache.ctx.valid || now - s_hookCache.ctxTick > 250)
    {
        s_hookCache.ctx = AcquireContext();
        s_hookCache.ctxTick = now;
    }

    if(!s_hookCache.ctx.valid)
        return BuildComment(addr, s_hookCache.ctx, original, false, false, s_hookCache.targets);

    auto it = s_hookCache.comments.find(addr);
    if(it != s_hookCache.comments.end())
    {
        if(now - it->second.second <= (uint64_t)std::max(0, g_settings.cacheTtlMs))
            return it->second.first;
    }

    BuiltComment bc = BuildComment(addr, s_hookCache.ctx, original, true, true, s_hookCache.targets);
    if(s_hookCache.comments.size() > 4096)
        s_hookCache.comments.clear();
    s_hookCache.comments[addr] = { bc, now };
    return bc;
}

void InvalidateCache()
{
    s_generation.fetch_add(1, std::memory_order_relaxed);
}

std::string BuildText()
{
    Context c = AcquireContext();
    std::string out = util::Format("[Stack Enhance] %s %s enabled=%d markers=%d\n",
                                   SE_ARCH, c.valid ? "paused" : "not-paused", g_settings.enabled ? 1 : 0,
                                   g_settings.markers ? 1 : 0);
    if(!c.valid)
    {
        out += "not debugging or running\n";
        return out;
    }

    duint rip = DbgValFromString("cip");
    out += util::Format("%s=%s %s=%s %s=%s stack=%s-%s\n",
                        SE_SP, util::HexPtr(c.rsp).c_str(), SE_BP, util::HexPtr(c.rbp).c_str(),
                        SE_IP, util::HexPtr(rip).c_str(),
                        util::HexPtr(c.stackBase).c_str(), util::HexPtr(c.stackBase + c.stackSize).c_str());

    TargetCache tc;
    const dsint slot = (dsint)sizeof(duint);
    const int above = std::max(0, g_settings.textRowsAbove);
    const int below = std::max(0, g_settings.textRowsBelow);
    for(int i = -above; i <= below; ++i)
    {
        dsint off = (dsint)i * slot;
        duint addr = (duint)((dsint)c.rsp + off);
        if(addr < c.stackBase || addr >= c.stackBase + c.stackSize)
            continue;
        BuiltComment bc;
        if(g_settings.enabled)
            bc = BuildComment(addr, c, &DbgStackCommentGet, true, true, tc);
        else
            bc = BuildComment(addr, c, &DbgStackCommentGet, false, false, tc);
        out += util::HexPtr(addr) + "\t" + bc.text + "\n";
    }
    return out;
}

bool CopyText(std::string& error)
{
    std::string text = BuildText();
    return util::CopyUtf8ToClipboard(text, error);
}
}
