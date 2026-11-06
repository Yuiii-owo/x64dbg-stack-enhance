#include <windows.h>
#include <intrin.h>
#include <cstdint>

static const char* g_ascii = "StackEnhance ASCII";
static const wchar_t* g_wide = L"StackEnhance WIDE \u4E2D\u6587";

__declspec(noinline, dllexport) void stack_probe()
{
    volatile uintptr_t slots[8] = {};
    slots[0] = 0;
    slots[1] = 0x1001;
    slots[2] = (uintptr_t)(void*)&stack_probe;
    slots[3] = (uintptr_t)(const void*)g_ascii;
    slots[4] = (uintptr_t)(const void*)g_wide;
    slots[5] = (uintptr_t)(void*)&slots[3];
    slots[6] = 0xDEADBEEF;
    void* heap = HeapAlloc(GetProcessHeap(), 0, 64);
    slots[7] = (uintptr_t)heap;

    __debugbreak();

    if(heap)
        HeapFree(GetProcessHeap(), 0, heap);
    volatile uintptr_t sink = slots[0] + slots[1] + slots[2] + slots[3] +
                              slots[4] + slots[5] + slots[6] + slots[7];
    (void)sink;
}

__declspec(noinline) static void level2()
{
    stack_probe();
}

__declspec(noinline) static void level1()
{
    level2();
}

int main()
{
    level1();
    return 0;
}
