#pragma once

#include "plugin.h"

namespace hook
{
using StackCommentFn = bool(__cdecl*)(duint addr, STACK_COMMENT* comment);

bool Install();
bool Shutdown();
bool IsInstalled();
void SetEnabled(bool enabled);
bool IsEnabled();
uint64_t GetCallCount();
int GetInHookCount();
void SetLoadId(uint64_t id);
std::string StatusText();
bool WriteStatus(const std::wstring& path, std::string& error);
}
