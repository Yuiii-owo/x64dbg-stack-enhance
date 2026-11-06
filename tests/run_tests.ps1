param(
    [ValidateSet('all','x64','x86')]
    [string]$Arch = 'all',
    [switch]$SkipBuild,
    [string]$X64dbgHome = '',
    [string]$SandboxRoot = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Out = Join-Path $Root 'tests\out'

if(-not $X64dbgHome) { $X64dbgHome = $env:X64DBG_HOME }
if(-not $X64dbgHome) { $X64dbgHome = 'C:\x64dbg' }
if(-not (Test-Path (Join-Path $X64dbgHome 'release')))
{
    throw "未找到 x64dbg 安装目录: $X64dbgHome。请设置 X64DBG_HOME 或传入 -X64dbgHome。"
}
if(-not $SandboxRoot) { $SandboxRoot = Join-Path $Root 'tests\sandbox' }
New-Item -ItemType Directory -Force $SandboxRoot | Out-Null

New-Item -ItemType Directory -Force $Out | Out-Null

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class SeNative
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
    public delegate bool EnumProc(IntPtr h, IntPtr l);

    public static IntPtr FindWindow(uint pid, string titleMatch)
    {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            uint p; GetWindowThreadProcessId(h, out p);
            if(p != pid || !IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512);
            GetWindowTextW(h, sb, sb.Capacity);
            if(sb.ToString().IndexOf(titleMatch, StringComparison.OrdinalIgnoreCase) >= 0)
            {
                found = h;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Invoke-Build
{
    if($SkipBuild) { return }
    $env:X64DBG_SDK = Join-Path $X64dbgHome 'pluginsdk'
    $env:X64DBG_HOME = $X64dbgHome
    Write-Host '[build] x64 plugin + test targets'
    cmd /c "cd /d `"$Root`" && build.bat x64 tests" | Out-Host
    if($LASTEXITCODE -ne 0) { throw 'x64 build failed' }
    Write-Host '[build] x86 plugin'
    cmd /c "cd /d `"$Root`" && build.bat x86" | Out-Host
    if($LASTEXITCODE -ne 0) { throw 'x86 build failed' }
}

function Set-IniValue
{
    param([string]$Path, [string]$Section, [string]$Key, [string]$Value)
    $lines = @()
    if(Test-Path $Path) { $lines = @(Get-Content $Path) }
    $sec = -1
    for($i = 0; $i -lt $lines.Count; $i++)
    {
        if($lines[$i] -eq "[$Section]") { $sec = $i; break }
    }
    if($sec -lt 0)
    {
        if($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne '') { $lines += '' }
        $lines += "[$Section]"
        $sec = $lines.Count - 1
    }
    $end = $lines.Count
    for($i = $sec + 1; $i -lt $lines.Count; $i++)
    {
        if($lines[$i] -match '^\[') { $end = $i; break }
    }
    $found = $false
    for($i = $sec + 1; $i -lt $end; $i++)
    {
        if($lines[$i] -match ('^' + [regex]::Escape($Key) + '='))
        {
            $lines[$i] = "$Key=$Value"
            $found = $true
            break
        }
    }
    if(-not $found)
    {
        $before = if($sec + 1 -ge $lines.Count) { @() } else { @($lines[($sec + 1)..($lines.Count - 1)]) }
        $lines = @($lines[0..$sec]) + @("$Key=$Value") + $before
    }
    Set-Content -Path $Path -Value $lines -Encoding ascii
}

function New-Sandbox
{
    param([string]$Arch)
    $rel = if($Arch -eq 'x64') { 'x64' } else { 'x32' }
    $src = Join-Path $X64dbgHome "release\$rel"
    $dst = Join-Path $SandboxRoot $Arch
    if(-not (Test-Path (Join-Path $src 'x64dbg.exe')) -and -not (Test-Path (Join-Path $src 'x32dbg.exe')))
    {
        throw "找不到 x64dbg 安装: $src"
    }
    if(Test-Path $dst) { Remove-Item -Recurse -Force $dst }
    New-Item -ItemType Directory -Force $dst | Out-Null
    Copy-Item -Path (Join-Path $src '*') -Destination $dst -Recurse -Force
    Remove-Item (Join-Path $dst 'plugins\*') -Recurse -Force -ErrorAction SilentlyContinue

    $dll = if($Arch -eq 'x64') { 'StackEnhance.dp64' } else { 'StackEnhance.dp32' }
    Copy-Item (Join-Path $Root "dist\$dll") (Join-Path $dst 'plugins') -Force
    Copy-Item (Join-Path $Root "tests\bin\$Arch\teststack.exe") $dst -Force
    Copy-Item (Join-Path $Root "tests\bin\$Arch\teststack.pdb") $dst -Force

    $ini = if($Arch -eq 'x64') { Join-Path $dst 'x64dbg.ini' } else { Join-Path $dst 'x32dbg.ini' }
    if($Arch -eq 'x64')
    {
        $template = Join-Path $X64dbgHome 'release\x32\x32dbg.ini'
        if(Test-Path $template) { Copy-Item $template $ini -Force }
    }
    Set-IniValue $ini 'Gui' 'ReleaseNotesEpoch' 'FFFFFFFF'
    Set-IniValue $ini 'Gui' 'ReleaseNotesPrevEpoch' 'FFFFFFFF'
    Set-IniValue $ini 'StackEnhance' 'Enabled' '1'
    Set-IniValue $ini 'StackEnhance' 'Markers' '1'
    Set-IniValue $ini 'StackEnhance' 'Depth' '3'
    Set-IniValue $ini 'StackEnhance' 'ShowStrings' '1'
    Set-IniValue $ini 'StackEnhance' 'ShowDisasm' '1'
    Set-IniValue $ini 'StackEnhance' 'Colorize' '1'
    if($Arch -eq 'x64')
    {
        Set-IniValue $ini 'Main Window Settings' 'CPUStackColumnWidth0' 'B4'
        Set-IniValue $ini 'Main Window Settings' 'CPUStackColumnWidth1' '78'
        Set-IniValue $ini 'Main Window Settings' 'CPUStackColumnWidth2' '3C0'
    }
    return $dst
}

function Get-UiaRoot
{
    param([IntPtr]$Hwnd)
    for($i = 0; $i -lt 12; $i++)
    {
        try
        {
            return [System.Windows.Automation.AutomationElement]::FromHandle($Hwnd)
        }
        catch
        {
            Start-Sleep -Milliseconds 300
        }
    }
    throw "无法连接 x64dbg UI Automation (HWND=$Hwnd)"
}

function Get-AllElements
{
    param($Root)
    return $Root.FindAll([System.Windows.Automation.TreeScope]::Descendants,
                         [System.Windows.Automation.Condition]::TrueCondition)
}

function Find-ElementByClass
{
    param($Root, [string]$ClassName)
    foreach($e in (Get-AllElements $Root))
    {
        if($e.Current.ClassName -eq $ClassName) { return $e }
    }
    return $null
}

function Send-Command
{
    param($Root, [string]$Command)
    $cmd = Find-ElementByClass $Root 'CommandLineEdit'
    if(-not $cmd) { throw '找不到 x64dbg 命令行控件' }
    $cmd.SetFocus()
    Start-Sleep -Milliseconds 100
    $vp = $cmd.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
    $vp.SetValue($Command)
    Start-Sleep -Milliseconds 100
    [System.Windows.Forms.SendKeys]::SendWait('{ENTER}')
    Start-Sleep -Milliseconds 800
}

function Get-CurrentIp
{
    param($Root)
    foreach($e in (Get-AllElements $Root))
    {
        $n = $e.Current.Name
        if($n -like 'RIP =*' -or $n -like 'EIP =*') { return $n }
    }
    return ''
}

function Wait-TestBreakpoint
{
    param($Root)
    Start-Sleep -Milliseconds 1600
    for($i = 0; $i -lt 5; $i++)
    {
        Send-Command $Root 'run'
        Start-Sleep -Milliseconds 1200
        $ip = Get-CurrentIp $Root
        if($ip -and $ip -notmatch 'mainCRTStartup|ntdll|kernelbase|RtlUserThreadStart')
        {
            Write-Host "[run] paused at $ip"
            return $ip
        }
    }
    throw "没有到达测试断点，最后 IP: $ip"
}

function Get-StackRect
{
    param($Root)
    $stack = Find-ElementByClass $Root 'CPUStack'
    if(-not $stack) { throw '找不到 CPUStack 控件' }
    return $stack.Current.BoundingRectangle
}

function Capture-Stack
{
    param([IntPtr]$Hwnd, $Root, [string]$Path)
    [SeNative]::ShowWindow($Hwnd, 3) | Out-Null
    [SeNative]::SetWindowPos($Hwnd, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002) | Out-Null
    [SeNative]::SetForegroundWindow($Hwnd) | Out-Null
    Start-Sleep -Milliseconds 800
    $rect = Get-StackRect $Root
    $w = [int]$rect.Width
    $h = [int]$rect.Height
    if($w -le 0 -or $h -le 0) { throw "无效的 Stack 控件区域: ${w}x${h}" }
    $bmp = New-Object System.Drawing.Bitmap($w, $h)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen([int]$rect.X, [int]$rect.Y, 0, 0, $bmp.Size)
    $g.Dispose()
    $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    [SeNative]::SetWindowPos($Hwnd, [IntPtr](-2), 0, 0, 0, 0, 0x0001 -bor 0x0002) | Out-Null
    Write-Host "[shot] $Path (${w}x${h})"
}

function Assert-Contains
{
    param([string]$Text, [string]$Needle, [string]$Label)
    if($Text.IndexOf($Needle, [StringComparison]::OrdinalIgnoreCase) -lt 0)
    {
        throw "$Label 缺少: $Needle"
    }
}

function Assert-NotContains
{
    param([string]$Text, [string]$Needle, [StringComparison]$Comparison, [string]$Label)
    if($Text.IndexOf($Needle, [StringComparison]::OrdinalIgnoreCase) -ge 0)
    {
        throw "$Label 不应包含: $Needle"
    }
}

function Get-LoadId
{
    param([string]$StatusPath)
    $line = Select-String -Path $StatusPath -Pattern '^loadId=' | Select-Object -First 1
    if(-not $line) { throw "状态文件缺少 loadId: $StatusPath" }
    return $line.Line
}

function Close-ModalDialogs
{
    param([uint32]$ProcessId)
    foreach($title in @('Error', 'Release notes'))
    {
        $h = [SeNative]::FindWindow($ProcessId, $title)
        if($h -ne [IntPtr]::Zero)
        {
            [SeNative]::SendMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 300
        }
    }
}

function Stop-Debugger
{
    param($Process, [string]$Arch, [string]$Dst)
    if($Process -and -not $Process.HasExited)
    {
        $m = [regex]::Match($Process.MainWindowTitle, 'PID:\s*(\d+)')
        if($m.Success)
        {
            $child = [int]$m.Groups[1].Value
            Get-Process -Id $child -ErrorAction SilentlyContinue | Stop-Process -Force
        }
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    }
    Get-Process teststack -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$Dst*" } |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

function Test-Arch
{
    param([string]$Arch)

    Write-Host "`n================ $Arch ================"
    $dst = New-Sandbox $Arch
    $exe = if($Arch -eq 'x64') { Join-Path $dst 'x64dbg.exe' } else { Join-Path $dst 'x32dbg.exe' }
    $target = Join-Path $dst 'teststack.exe'
    $dllRel = if($Arch -eq 'x64') { 'plugins\StackEnhance.dp64' } else { 'plugins\StackEnhance.dp32' }
    $dll = Join-Path $dst $dllRel
    $ip = $null
    $proc = $null

    try
    {
        $proc = Start-Process -FilePath $exe -ArgumentList @("`"$target`"") -WorkingDirectory $dst -PassThru
        Write-Host "[launch] pid=$($proc.Id)"
        $deadline = (Get-Date).AddSeconds(20)
        while((Get-Date) -lt $deadline -and $proc.MainWindowHandle -eq 0) { Start-Sleep -Milliseconds 400 }
        if($proc.MainWindowHandle -eq 0) { throw 'x64dbg 主窗口未出现' }
        $hwnd = $proc.MainWindowHandle
        Start-Sleep -Milliseconds 800
        Close-ModalDialogs ([uint32]$proc.Id)
        Start-Sleep -Milliseconds 500
        $proc.Refresh()
        if($proc.HasExited) { throw 'x64dbg 在启动阶段退出' }
        if($proc.MainWindowHandle -ne 0) { $hwnd = $proc.MainWindowHandle }
        $root = Get-UiaRoot $hwnd

        $ip = Wait-TestBreakpoint $root

        $textPath = Join-Path $Out "text_$Arch.txt"
        $statusPath = Join-Path $Out "status_$Arch.txt"
        Removed-ItemSafe $textPath
        Removed-ItemSafe $statusPath
        Send-Command $root "stackenhance text `"$textPath`""
        Send-Command $root "stackenhance status `"$statusPath`""

        if(-not (Test-Path $textPath)) { throw "没有生成 $textPath" }
        if(-not (Test-Path $statusPath)) { throw "没有生成 $statusPath" }

        $text = Get-Content $textPath -Raw
        Assert-Contains $text '→' $Arch
        Assert-Contains $text 'teststack.stack_probe' $Arch
        Assert-Contains $text 'StackEnhance ASCII' $Arch
        Assert-Contains $text 'StackEnhance WIDE' $Arch
        Assert-Contains $text 'deadbeef' $Arch
        Assert-Contains $text 'invalid' $Arch
        Assert-Contains $text 'push' $Arch
        if($Arch -eq 'x64')
        {
            Assert-Contains $text 'rsp' $Arch
        }
        else
        {
            Assert-Contains $text 'esp' $Arch
            Assert-Contains $text 'ebp |' $Arch
        }

        $status = Get-Content $statusPath -Raw
        Assert-Contains $status 'hookInstalled=1' $Arch
        Assert-Contains $status 'enabled=1' $Arch
        Assert-Contains $status 'paused=1' $Arch
        $loadId1 = Get-LoadId $statusPath

        $shot = Join-Path $Out "stack_$Arch.png"
        Capture-Stack $hwnd $root $shot
        if((Get-Item $shot).Length -lt 5000) { throw "截图文件过小: $shot" }

        # 禁用后的基线：不应出现增强链/标记
        $basePath = Join-Path $Out "baseline_$Arch.txt"
        $beforeShot = Join-Path $Out "stack_${Arch}_before.png"
        Removed-ItemSafe $basePath
        Removed-ItemSafe $beforeShot
        Send-Command $root 'stackenhance off'
        Start-Sleep -Milliseconds 400
        Capture-Stack $hwnd $root $beforeShot
        Send-Command $root "stackenhance text `"$basePath`""
        if(-not (Test-Path $basePath)) { throw "没有生成基线 $basePath" }
        $baseline = Get-Content $basePath -Raw
        Assert-NotContains $baseline '→' ([StringComparison]::OrdinalIgnoreCase) $Arch
        Assert-NotContains $baseline '(teststack.stack_probe)' ([StringComparison]::OrdinalIgnoreCase) $Arch
        Send-Command $root 'stackenhance on'

        # 卸载/重载
        Send-Command $root 'plugunload StackEnhance'
        Start-Sleep -Milliseconds 500
        Send-Command $root 'plugunload StackEnhance'
        Start-Sleep -Milliseconds 500
        Send-Command $root "plugload `"$dll`""
        Start-Sleep -Milliseconds 700
        $reloadPath = Join-Path $Out "reload_$Arch.txt"
        Removed-ItemSafe $reloadPath
        Send-Command $root "stackenhance status `"$reloadPath`""
        if(-not (Test-Path $reloadPath)) { throw "重载后没有生成状态 $reloadPath" }
        $status2 = Get-Content $reloadPath -Raw
        Assert-Contains $status2 'hookInstalled=1' $Arch
        $loadId2 = Get-LoadId $reloadPath
        if($loadId1 -eq $loadId2) { throw "重载后 loadId 未变化: $loadId1" }
        Write-Host "[ok] $Arch loadId $loadId1 -> $loadId2"
    }
    finally
    {
        Stop-Debugger $proc $Arch $dst
    }
}

function Removed-ItemSafe
{
    param([string]$Path)
    if(Test-Path $Path) { Remove-Item $Path -Force -ErrorAction SilentlyContinue }
}

Invoke-Build

if($Arch -eq 'all' -or $Arch -eq 'x64') { Test-Arch 'x64' }
if($Arch -eq 'all' -or $Arch -eq 'x86') { Test-Arch 'x86' }

Write-Host "`n全部测试通过。截图: $Out\stack_x64.png / stack_x86.png"
