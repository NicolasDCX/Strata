param([string]$Exe, [string]$ArgLine, [string]$Out)
# Runs $Exe with $ArgLine, stdout+stderr to $Out, and appends the process's peak
# working set and peak private commit (GetProcessMemoryInfo, 64-bit SIZE_T) and
# the lowest free physical memory seen.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PeakMem {
    [StructLayout(LayoutKind.Sequential)]
    public struct Counters {
        public uint cb; public uint PageFaultCount;
        public UIntPtr PeakWorkingSetSize; public UIntPtr WorkingSetSize;
        public UIntPtr QuotaPeakPagedPoolUsage; public UIntPtr QuotaPagedPoolUsage;
        public UIntPtr QuotaPeakNonPagedPoolUsage; public UIntPtr QuotaNonPagedPoolUsage;
        public UIntPtr PagefileUsage; public UIntPtr PeakPagefileUsage;
    }
    [DllImport("psapi.dll", SetLastError = true)]
    static extern bool GetProcessMemoryInfo(IntPtr process, out Counters counters, uint size);
    public static ulong[] Peaks(IntPtr process) {
        Counters c;
        if (!GetProcessMemoryInfo(process, out c, (uint)Marshal.SizeOf(typeof(Counters)))) { return new ulong[] { 0, 0 }; }
        return new ulong[] { c.PeakWorkingSetSize.ToUInt64(), c.PeakPagefileUsage.ToUInt64() };
    }
}
'@
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.Arguments = $ArgLine
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.WorkingDirectory = Split-Path $Exe
$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
$sb = New-Object System.Text.StringBuilder
$handler = { if ($EventArgs.Data -ne $null) { [void]$Event.MessageData.AppendLine($EventArgs.Data) } }
$e1 = Register-ObjectEvent -InputObject $p -EventName OutputDataReceived -Action $handler -MessageData $sb
$e2 = Register-ObjectEvent -InputObject $p -EventName ErrorDataReceived -Action $handler -MessageData $sb
$sw = [System.Diagnostics.Stopwatch]::StartNew()
[void]$p.Start()
$handle = $p.Handle
$p.BeginOutputReadLine()
$p.BeginErrorReadLine()
$peakWs = 0; $peakPriv = 0; $minFree = [double]::MaxValue
while (-not $p.HasExited) {
    $v = [PeakMem]::Peaks($handle)
    $peakWs = [Math]::Max($peakWs, [double]$v[0]); $peakPriv = [Math]::Max($peakPriv, [double]$v[1])
    $free = (Get-CimInstance Win32_OperatingSystem -Property FreePhysicalMemory).FreePhysicalMemory * 1KB
    $minFree = [Math]::Min($minFree, $free)
    Start-Sleep -Milliseconds 250
}
# The handle stays open, so the final peaks of the exited process are still readable.
$v = [PeakMem]::Peaks($handle)
$peakWs = [Math]::Max($peakWs, [double]$v[0]); $peakPriv = [Math]::Max($peakPriv, [double]$v[1])
$p.WaitForExit()
Start-Sleep -Milliseconds 300
Unregister-Event -SourceIdentifier $e1.Name
Unregister-Event -SourceIdentifier $e2.Name
$text = $sb.ToString()
$text += ("PROCESS exit {0}, wall {1:N1} s, peak working set {2:N2} GiB, peak private commit {3:N2} GiB, lowest free RAM {4:N2} GiB`n" -f `
    $p.ExitCode, $sw.Elapsed.TotalSeconds, ($peakWs / 1GB), ($peakPriv / 1GB), ($minFree / 1GB))
[System.IO.File]::WriteAllText($Out, $text)
