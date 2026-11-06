# Stack Enhance 设计说明

## 1. 目标与边界

- 增强 x64dbg **自带 Stack 视图**的 Comments 列，不新建窗口/标签页。
- 保留 Address / Data 列、列头、行高、选择、滚动等原生行为。
- 不复制 pwndbg 的 `[ STACK ]` 标题/寄存器摘要；不新增列。
- 不修改 x64dbg 磁盘文件，不注入 Qt 控件；通过 IAT 钩子改变 Comments 列的数据来源。

## 2. 组件

```text
src/pluginmain.cpp   插件导出、菜单、命令、配置、调试事件
src/hook.cpp         GUI 模块 IAT 查找/替换/恢复、钩子入口、诊断
src/stackcomment.cpp 栈上下文、标记、指针链、符号/字符串/反汇编、缓存、文本导出
src/util.cpp         UTF-8/UTF-16、十六进制、跨页安全读、文件、剪贴板
```

数据流：

```text
GUI 绘制 Stack Comments
  -> x64gui.dll / x32gui.dll 的 IAT
  -> StackCommentHook(addr, STACK_COMMENT*)
  -> 调原 DbgStackCommentGet（保留 return/SEH/原生字符串）
  -> BuildComment（rsp/rbp 标记 + 指针链/符号/指令/字符串）
  -> 拷回 STACK_COMMENT（text + 单行颜色）
```

## 3. IAT 钩子

- 定位 GUI 模块：x64 `x64gui.dll`，x86 `x32gui.dll`；找不到时枚举模块名含 `gui` 的模块。
- 解析 PE 导入表：
  - 匹配 `DbgStackCommentGet` 的名称；兼容 ordinal 77。
  - 若导入按序号/被绑定，退回比较 IAT 中的函数指针与 `GetProcAddress(bridge, "DbgStackCommentGet")`。
- 保存槽位和原指针；`VirtualProtect` 后 `InterlockedExchangePointer` 替换为 `StackCommentHook`。
- 多个匹配槽位全部记录；安装失败时回滚已写入的槽位，并在 `stackenhance status` 报告 `hookInstalled=0`。
- 卸载时 `InterlockedCompareExchangePointer` 只在槽位仍指向本插件钩子时恢复原值；若被其他插件改写则不强行覆盖，并返回 `false` 让宿主保留 DLL。
- 不等待 `g_inHook == 0`：`plugstop` 在调试器线程执行，等待可能与 GUI 的同步 bridge 调用互相死锁。若仍有钩子在栈上，返回 `false`，IAT 已恢复，下一次卸载可重试。

## 4. 注释生成

### 4.1 上下文

- `rsp = DbgValFromString("csp")`、`rbp = DbgValFromString("cbp")`。
- `DbgMemFindBaseAddr(rsp, &size)` 得到栈范围；`rbp` 只有在范围内且按指针大小对齐时才作为帧基准。
- 缓存上下文 250 ms；调试状态 generation 变化（暂停/单步/断点/继续/停止）时强制刷新。

### 4.2 标记

- `addr == rsp`：`rsp | `（x86 为 `esp | `）。
- `addr == rbp` 且在栈内：`rbp | `（x86 `ebp | `）。
- 其他栈内地址：以 `rbp`（有效时）或 `rsp` 为基准的 `%+03X | `。
- 非栈地址/未对齐地址不加标记。

### 4.3 指针链

优先级：

1. 原函数的 `!rtnclr` / `!sehclr`：保留原文和颜色，仅加标记。
2. 代码指针：`→ 0x... (module.symbol+off) ← instruction`。
3. 字符串：`→ 0x... ← "..."` / `L"..."`。
4. 模块数据：`→ 0x... (module+0xRVA)`，可继续跳转。
5. 栈内指针：`→ 0x... [stack+NN]`。
6. 无效指针：`→ 0x... (invalid)`。
7. 普通可读指针：最多 `Depth` 跳；循环检测，重复时 `(cycle)`。

- 小整数（小于 0x10000）不解引用，避免把标志位/枚举误判为指针。
- 空值只显示标记；Data 列已经显示原始值，注释不重复 `← value`。
- 代码指针优先于字符串，避免把可执行页中的字节误判成字符串。
- 符号通过 `DbgGetLabelAt` / `DbgGetModuleAt` 获取；对 `void __cdecl func(void)` 形式做简短化，得到 `module.func`。
- 指令通过 `DbgDisasmAt` 获取。

### 4.4 颜色

`STACK_COMMENT.color` 支持 `!rtnclr` / `!sehclr` 或 `#RRGGBB`。原生 Comments 列一行一个颜色，因此：

| 类型 | 颜色 |
|---|---|
| 代码 | `#E5C07B` |
| 字符串 | `#C678DD` |
| 无效指针 | `#E06C75` |
| 栈内指针 | `#61AFEF` |
| 标记/普通 | 不设置，使用主题文字色 |

`Colorize=0` 时全部使用默认文字色；原函数的特殊颜色不受影响。

### 4.5 缓存

- 钩子在 GUI 绘制线程执行；每个可见行都会调用。
- 最终注释按地址缓存，TTL 默认 300 ms；超过 4096 条清空。
- 目标地址的符号/字符串/指令按地址缓存；调试状态 generation 变化时清空。
- 文本导出 `stackenhance text` 在调试器线程使用独立的临时缓存，不共享 GUI 线程的 map。
- 用户手动修改内存后，最多 300 ms 刷新；可用 `stackenhance clearcache` 立即清空。

### 4.6 文本导出

`stackenhance text <path>` 扫描 `rsp - TextRowsAbove*slot` 到 `rsp + TextRowsBelow*slot`，每行：

```text
0x0000000000123F60	rsp | return to ...
0x0000000000123F70	+010 | → 0x... (module.func) ← push rbp
```

`stackenhance status [path]` 输出 `loadId/hookInstalled/hookCalls/iatSlot/original/guiModule/paused/rip/rsp/rbp`，用于诊断与自动化。

## 5. 线程与生命周期

- `CB_INITDEBUG / CB_PAUSEDEBUG / CB_BREAKPOINT / CB_STEPPED`：`paused=true`，generation++，必要时重试安装钩子。
- `CB_RESUMEDEBUG / CB_STOPDEBUG`：`paused=false`，generation++。
- 钩子先检查 `g_enabled` 和 `paused`；不满足时直接调用原函数。
- `BuildForHook` 在 GUI 线程读取 rsp/rbp/内存/符号；运行中 `AcquireContext` 返回无效，回退原函数。
- 钩子调用原函数时使用局部 `STACK_COMMENT`，再按优先级合并，避免污染宿主结构。
- 异常全部捕获并回退原函数。
- `plugstop` 恢复 IAT、注销命令、保存配置；无窗口/无线程需要销毁。

## 6. 构建

- MSVC 2022，`/std:c++17 /utf-8 /MT /LD /guard:cf`。
- 链接 `x64dbg.lib x64bridge.lib user32.lib shell32.lib`（x86 对应 `x32dbg.lib x32bridge.lib`）。
- `build.bat` 自动定位 vcvarsall 和 SDK；产物 `dist/StackEnhance.dp64/.dp32`。
- 测试目标 `/Od /Oy- /Zi /MT`，x64/x86 各一份。

## 7. 测试

`tests/run_tests.ps1`：

1. 构建插件和测试目标。
2. 复制 x64dbg 到 `tests/sandbox/`，清空插件目录，只放本插件，避免影响用户会话。
3. UI Automation 定位 x64dbg 命令行，`run` 到 `__debugbreak` 暂停。
4. `stackenhance text/status`，断言：
   - `teststack.stack_probe`、`StackEnhance ASCII`、`StackEnhance WIDE`、`deadbeef`、`invalid`、`→`、`push`；
   - x64 断言 `rsp`，x86 断言 `esp`/`ebp |`；
   - status 的 `hookInstalled=1`、`enabled=1`、`paused=1`。
5. UI Automation 找原生 `CPUStack` 的屏幕矩形并截图 `tests/out/stack_*.png`。
6. `stackenhance off` 后导出基线，断言无增强箭头/符号；再 `on`。
7. `plugunload` + `plugload`，断言 `loadId` 变化且 `hookInstalled=1`。

## 8. 限制与后续

- 逐 token 多色需要 Qt 注入或重建 x64dbg GUI；本插件刻意不做。
- Comments 列必须可见；列宽限制会裁剪长注释。
- x64 代码常把 RBP 当通用寄存器；此时没有 `rbp` 标记，只有 RSP 基准偏移。
- 后续可考虑：Qt 停靠页、按 token 着色、更多注释预设（若用户明确需要）。
