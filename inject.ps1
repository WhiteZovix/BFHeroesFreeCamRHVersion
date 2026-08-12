param(
    [Parameter(Mandatory=$true)][int]$ProcessId,
    [Parameter(Mandatory=$true)][string]$DllPath
)

Add-Type @"
using System;
using System.Runtime.InteropServices;

public class Injector {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint processAccess, bool bInheritHandle, int processId);

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Auto)]
    public static extern IntPtr VirtualAllocEx(IntPtr hProcess, IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool WriteProcessMemory(IntPtr hProcess, IntPtr lpBaseAddress, byte[] lpBuffer, uint nSize, out UIntPtr lpNumberOfBytesWritten);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr GetModuleHandle(string lpModuleName);

    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi, ExactSpelling=true)]
    public static extern IntPtr GetProcAddress(IntPtr hModule, string procName);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr CreateRemoteThread(IntPtr hProcess, IntPtr lpThreadAttributes, uint dwStackSize, IntPtr lpStartAddress, IntPtr lpParameter, uint dwCreationFlags, out uint lpThreadId);

    [DllImport("kernel32.dll")]
    public static extern uint WaitForSingleObject(IntPtr hHandle, uint dwMilliseconds);

    [DllImport("kernel32.dll")]
    public static extern bool GetExitCodeThread(IntPtr hThread, out uint lpExitCode);

    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr hObject);
}
"@

$PROCESS_ALL_ACCESS = 0x1F0FFF
$MEM_COMMIT = 0x1000
$MEM_RESERVE = 0x2000
$PAGE_READWRITE = 0x04

$hProcess = [Injector]::OpenProcess($PROCESS_ALL_ACCESS, $false, $ProcessId)
if ($hProcess -eq [IntPtr]::Zero) { Write-Error "OpenProcess failed"; exit 1 }

$pathBytes = [System.Text.Encoding]::ASCII.GetBytes($DllPath + "`0")
$remoteMem = [Injector]::VirtualAllocEx($hProcess, [IntPtr]::Zero, [uint32]$pathBytes.Length, ($MEM_COMMIT -bor $MEM_RESERVE), $PAGE_READWRITE)
if ($remoteMem -eq [IntPtr]::Zero) { Write-Error "VirtualAllocEx failed"; exit 1 }

$written = [UIntPtr]::Zero
$ok = [Injector]::WriteProcessMemory($hProcess, $remoteMem, $pathBytes, [uint32]$pathBytes.Length, [ref]$written)
if (-not $ok) { Write-Error "WriteProcessMemory failed"; exit 1 }

$k32 = [Injector]::GetModuleHandle("kernel32.dll")
$loadLibAddr = [Injector]::GetProcAddress($k32, "LoadLibraryA")
if ($loadLibAddr -eq [IntPtr]::Zero) { Write-Error "GetProcAddress failed"; exit 1 }

$threadId = 0
$hThread = [Injector]::CreateRemoteThread($hProcess, [IntPtr]::Zero, 0, $loadLibAddr, $remoteMem, 0, [ref]$threadId)
if ($hThread -eq [IntPtr]::Zero) { Write-Error "CreateRemoteThread failed"; exit 1 }

[Injector]::WaitForSingleObject($hThread, 5000) | Out-Null
$exitCode = 0
[Injector]::GetExitCodeThread($hThread, [ref]$exitCode) | Out-Null
Write-Output "Injected. Remote LoadLibraryA returned module handle: 0x$($exitCode.ToString('X8'))"

[Injector]::CloseHandle($hThread) | Out-Null
[Injector]::CloseHandle($hProcess) | Out-Null
