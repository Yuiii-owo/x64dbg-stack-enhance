#pragma once

#include "plugin.h"

namespace stackcomment
{
using OriginalFn = bool(__cdecl*)(duint addr, STACK_COMMENT* comment);

struct BuiltComment
{
    std::string text;
    std::string color;

    bool empty() const
    {
        return text.empty();
    }
};

struct Context
{
    bool  valid = false;
    duint rsp = 0;
    duint rbp = 0;
    duint stackBase = 0;
    duint stackSize = 0;
    bool  rbpInStack = false;

    duint stackEnd() const
    {
        return stackBase + stackSize;
    }
};

void OnDebugStateChanged();
void SetPaused(bool paused);
bool IsPaused();

BuiltComment BuildForHook(duint addr, OriginalFn original);
void InvalidateCache();

std::string BuildText();
bool CopyText(std::string& error);
}
