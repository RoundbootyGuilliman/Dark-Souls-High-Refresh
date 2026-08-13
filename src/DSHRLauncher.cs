using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;

internal static class Program
{
    private const string SupportedSha1 = "F0ECFBE20D780751248DFB2A9759D6215E246676";
    private const uint CreateSuspended = 0x00000004;
    private const uint Infinite = 0xFFFFFFFF;
    private const uint WaitObject0 = 0x00000000;
    private const uint WaitTimeout = 0x00000102;
    private const uint MemCommit = 0x00001000;
    private const uint MemReserve = 0x00002000;
    private const uint MemRelease = 0x00008000;
    private const uint MemPrivate = 0x00020000;
    private const uint PageReadWrite = 0x04;
    private const uint PageGuard = 0x100;
    private const uint PageNoAccess = 0x01;

    // Vtable RVAs in Dark Souls Remastered 1.03.
    private const long Flipper60VtableRva = 0x12A7248;
    private const long Flipper140VtableRva = 0x12A72A0;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct StartupInfo
    {
        public int cb;
        public string lpReserved;
        public string lpDesktop;
        public string lpTitle;
        public int dwX;
        public int dwY;
        public int dwXSize;
        public int dwYSize;
        public int dwXCountChars;
        public int dwYCountChars;
        public int dwFillAttribute;
        public int dwFlags;
        public short wShowWindow;
        public short cbReserved2;
        public IntPtr lpReserved2;
        public IntPtr hStdInput;
        public IntPtr hStdOutput;
        public IntPtr hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessInformation
    {
        public IntPtr hProcess;
        public IntPtr hThread;
        public int dwProcessId;
        public int dwThreadId;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ProcessBasicInformation
    {
        public IntPtr Reserved1;
        public IntPtr PebBaseAddress;
        public IntPtr Reserved2_0;
        public IntPtr Reserved2_1;
        public IntPtr UniqueProcessId;
        public IntPtr Reserved3;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MemoryBasicInformation64
    {
        public ulong BaseAddress;
        public ulong AllocationBase;
        public uint AllocationProtect;
        public uint Alignment1;
        public ulong RegionSize;
        public uint State;
        public uint Protect;
        public uint Type;
        public uint Alignment2;
    }

    private sealed class MemoryRegion
    {
        public ulong BaseAddress;
        public ulong RegionSize;

        public MemoryRegion(ulong baseAddress, ulong regionSize)
        {
            BaseAddress = baseAddress;
            RegionSize = regionSize;
        }
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CreateProcessW(
        string applicationName,
        StringBuilder commandLine,
        IntPtr processAttributes,
        IntPtr threadAttributes,
        bool inheritHandles,
        uint creationFlags,
        IntPtr environment,
        string currentDirectory,
        ref StartupInfo startupInfo,
        out ProcessInformation processInformation);

    [DllImport("ntdll.dll")]
    private static extern int NtQueryInformationProcess(
        IntPtr processHandle,
        int processInformationClass,
        ref ProcessBasicInformation processInformation,
        int processInformationLength,
        out int returnLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool ReadProcessMemory(
        IntPtr process,
        IntPtr baseAddress,
        [Out] byte[] buffer,
        IntPtr size,
        out IntPtr bytesRead);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool WriteProcessMemory(
        IntPtr process,
        IntPtr baseAddress,
        byte[] buffer,
        IntPtr size,
        out IntPtr bytesWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(
        IntPtr process,
        IntPtr address,
        IntPtr size,
        uint allocationType,
        uint protection);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool VirtualFreeEx(
        IntPtr process,
        IntPtr address,
        IntPtr size,
        uint freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(
        IntPtr process,
        IntPtr threadAttributes,
        IntPtr stackSize,
        IntPtr startAddress,
        IntPtr parameter,
        uint creationFlags,
        out int threadId);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandleW(string moduleName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern UIntPtr VirtualQueryEx(
        IntPtr process,
        IntPtr address,
        out MemoryBasicInformation64 information,
        UIntPtr informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint ResumeThread(IntPtr thread);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool TerminateProcess(IntPtr process, uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr window, string text, string caption, uint type);

    private static int Main(string[] args)
    {
        try
        {
            bool verifyOnly = HasArgument(args, "--verify") || HasArgument(args, "--dry-run");
            bool smokeTest = HasArgument(args, "--smoke-test");
            bool waitForExit = HasArgument(args, "--wait");
            string gameDirectory = ResolveGameDirectory();
            string gameExe = Path.Combine(gameDirectory, "DarkSoulsRemastered.exe");

            Log("Dark Souls High Refresh launcher");
            Log("Game executable: " + gameExe);
            VerifyExecutable(gameExe);

            if (verifyOnly)
            {
                Log("Verification passed. This Dark Souls Remastered 1.03 executable is supported.");
                return 0;
            }

            if (Process.GetProcessesByName("DarkSoulsRemastered").Length != 0)
                throw new InvalidOperationException("Dark Souls Remastered is already running. Close it before using DSHR.");

            ProcessInformation processInfo;
            StartupInfo startupInfo = new StartupInfo();
            startupInfo.cb = Marshal.SizeOf(typeof(StartupInfo));
            StringBuilder commandLine = new StringBuilder("\"" + gameExe + "\"");

            bool created = CreateProcessW(
                gameExe,
                commandLine,
                IntPtr.Zero,
                IntPtr.Zero,
                false,
                CreateSuspended,
                IntPtr.Zero,
                gameDirectory,
                ref startupInfo,
                out processInfo);

            if (!created)
                ThrowWin32("Could not create the game process");

            bool resumed = false;
            try
            {
                long imageBase = GetImageBase(processInfo.hProcess);
                Log("Game created suspended (PID " + processInfo.dwProcessId + ", image base 0x" + imageBase.ToString("X") + ").");

                string hookPath = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "DSHRHook.dll");
                InjectDll(processInfo.hProcess, hookPath);
                Thread.Sleep(100);

                uint previousSuspendCount = ResumeThread(processInfo.hThread);
                if (previousSuspendCount == Infinite)
                    ThrowWin32("ResumeThread failed");
                resumed = true;
                Log("Game resumed with high-refresh render plus corrected gameplay, visual-FX, and Remo cinematic timing hooks installed.");

                IntPtr scheduler = WaitForWindowAndScheduler(
                    processInfo,
                    imageBase,
                    TimeSpan.FromSeconds(45));

                long flipper140Vtable = imageBase + Flipper140VtableRva;
                WriteExact(processInfo.hProcess, scheduler, BitConverter.GetBytes(flipper140Vtable));

                byte[] verify = ReadExact(processInfo.hProcess, scheduler, 8);
                if (BitConverter.ToInt64(verify, 0) != flipper140Vtable)
                    throw new InvalidOperationException("The 140-FPS scheduler switch did not verify after writing.");

                IntPtr modeField = FindSchedulerModeField(processInfo.hProcess, scheduler);
                WriteExact(processInfo.hProcess, modeField, BitConverter.GetBytes(5));
                if (BitConverter.ToInt32(ReadExact(processInfo.hProcess, modeField, 4), 0) != 5)
                    throw new InvalidOperationException("The post-start mode switch did not verify after writing.");

                Log("Switched the live scheduler at 0x" + scheduler.ToInt64().ToString("X") +
                    " from FrpgFlipper_60FPS to FrpgFlipper_140FPS.");
                Log("Switched the live scheduler owner at 0x" + modeField.ToInt64().ToString("X") +
                    " from shipping mode 4 to high-refresh mode 5 after window initialization.");
                Log("The game window initialized normally with the high-refresh timing corrections active; DarkSoulsRemastered.exe was not modified.");

                if (smokeTest)
                {
                    uint waitResult = WaitForSingleObject(processInfo.hProcess, 10000);
                    if (waitResult == WaitTimeout)
                    {
                        using (Process game = Process.GetProcessById(processInfo.dwProcessId))
                        {
                            game.Refresh();
                            if (game.MainWindowHandle == IntPtr.Zero)
                                throw new InvalidOperationException("Smoke test failed: the process survived but lost its game window after the scheduler switch.");
                        }
                        Log("Smoke test passed: the high-refresh scheduler and gameplay, visual-FX, and Remo timing hooks ran with a game window for 10 seconds.");
                        TerminateProcess(processInfo.hProcess, 0xD5140);
                    }
                    else if (waitResult == WaitObject0)
                    {
                        uint exitCode;
                        if (!GetExitCodeProcess(processInfo.hProcess, out exitCode))
                            ThrowWin32("GetExitCodeProcess failed");
                        throw new InvalidOperationException("Smoke test failed: the game exited with code 0x" + exitCode.ToString("X8") + ".");
                    }
                    else
                    {
                        ThrowWin32("WaitForSingleObject failed during smoke test");
                    }
                    return 0;
                }

                if (waitForExit)
                    WaitForSingleObject(processInfo.hProcess, Infinite);
            }
            finally
            {
                if (!resumed)
                    TerminateProcess(processInfo.hProcess, 0xD5141);
                CloseHandle(processInfo.hThread);
                CloseHandle(processInfo.hProcess);
            }

            return 0;
        }
        catch (Exception exception)
        {
            string message = "Dark Souls High Refresh could not start:\r\n\r\n" + exception.Message;
            Log("ERROR: " + exception);
            try { MessageBoxW(IntPtr.Zero, message, "Dark Souls High Refresh", 0x10); }
            catch { }
            return 1;
        }
    }

    private static void InjectDll(IntPtr process, string dllPath)
    {
        if (!File.Exists(dllPath))
            throw new FileNotFoundException("DSHRHook.dll was not found beside the launcher.", dllPath);

        byte[] pathBytes = Encoding.Unicode.GetBytes(Path.GetFullPath(dllPath) + "\0");
        IntPtr remotePath = VirtualAllocEx(
            process,
            IntPtr.Zero,
            new IntPtr(pathBytes.Length),
            MemCommit | MemReserve,
            PageReadWrite);
        if (remotePath == IntPtr.Zero)
            ThrowWin32("VirtualAllocEx failed while preparing the DSHR hook");

        try
        {
            WriteExact(process, remotePath, pathBytes);
            IntPtr kernel32 = GetModuleHandleW("kernel32.dll");
            if (kernel32 == IntPtr.Zero)
                ThrowWin32("GetModuleHandleW(kernel32.dll) failed");
            IntPtr loadLibrary = GetProcAddress(kernel32, "LoadLibraryW");
            if (loadLibrary == IntPtr.Zero)
                ThrowWin32("GetProcAddress(LoadLibraryW) failed");

            int threadId;
            IntPtr remoteThread = CreateRemoteThread(
                process,
                IntPtr.Zero,
                IntPtr.Zero,
                loadLibrary,
                remotePath,
                0,
                out threadId);
            if (remoteThread == IntPtr.Zero)
                ThrowWin32("CreateRemoteThread failed while loading the DSHR hook");
            try
            {
                uint waitResult = WaitForSingleObject(remoteThread, 15000);
                if (waitResult != WaitObject0)
                    throw new TimeoutException("Loading DSHRHook.dll did not complete within 15 seconds.");
                uint moduleResult;
                if (!GetExitCodeThread(remoteThread, out moduleResult))
                    ThrowWin32("GetExitCodeThread failed while loading the DXGI hook");
                if (moduleResult == 0)
                    throw new InvalidOperationException("LoadLibraryW could not load DSHRHook.dll into the game process.");
            }
            finally
            {
                CloseHandle(remoteThread);
            }
        }
        finally
        {
            VirtualFreeEx(process, remotePath, IntPtr.Zero, MemRelease);
        }
        Log("Loaded DSHRHook.dll into the suspended game process.");
    }

    private static IntPtr WaitForWindowAndScheduler(
        ProcessInformation processInfo,
        long imageBase,
        TimeSpan timeout)
    {
        Stopwatch timer = Stopwatch.StartNew();
        bool windowSeen = false;
        long flipper60Vtable = imageBase + Flipper60VtableRva;

        using (Process game = Process.GetProcessById(processInfo.dwProcessId))
        {
            while (timer.Elapsed < timeout)
            {
                uint waitResult = WaitForSingleObject(processInfo.hProcess, 0);
                if (waitResult == WaitObject0)
                {
                    uint exitCode;
                    if (!GetExitCodeProcess(processInfo.hProcess, out exitCode))
                        ThrowWin32("GetExitCodeProcess failed");
                    throw new InvalidOperationException("The game exited during startup with code 0x" + exitCode.ToString("X8") + ".");
                }

                game.Refresh();
                if (!windowSeen && game.MainWindowHandle != IntPtr.Zero)
                {
                    windowSeen = true;
                    Log("Game window initialized; locating the live 60-FPS scheduler.");
                }

                if (windowSeen)
                {
                    IntPtr address = FindScheduler(processInfo.hProcess, flipper60Vtable);
                    if (address != IntPtr.Zero)
                        return address;
                }

                Thread.Sleep(250);
            }
        }

        if (!windowSeen)
            throw new TimeoutException("The unmodified game did not create a window within 45 seconds.");
        throw new TimeoutException("The game window opened, but the live 60-FPS scheduler was not found within 45 seconds.");
    }

    private static IntPtr FindScheduler(IntPtr process, long expectedVtable)
    {
        List<MemoryRegion> regions = EnumerateWritablePrivateRegions(process);

        // In this build the scheduler lives in a distinctive 0x02001000-byte
        // game heap. Search such heaps first so the switch is effectively instant.
        foreach (MemoryRegion region in regions)
            if (region.RegionSize == 0x02001000)
            {
                IntPtr found = SearchRegion(process, region, expectedVtable);
                if (found != IntPtr.Zero)
                    return found;
            }

        foreach (MemoryRegion region in regions)
            if (region.RegionSize != 0x02001000 && region.RegionSize <= 0x08000000)
            {
                IntPtr found = SearchRegion(process, region, expectedVtable);
                if (found != IntPtr.Zero)
                    return found;
            }

        return IntPtr.Zero;
    }

    private static IntPtr FindSchedulerModeField(IntPtr process, IntPtr scheduler)
    {
        long schedulerValue = scheduler.ToInt64();
        List<MemoryRegion> regions = EnumerateWritablePrivateRegions(process);
        foreach (MemoryRegion region in regions)
        {
            if (region.RegionSize > 0x08000000)
                continue;
            IntPtr pointerField = SearchPointerRegion(process, region, schedulerValue);
            if (pointerField == IntPtr.Zero || pointerField.ToInt64() < 8)
                continue;
            IntPtr candidate = new IntPtr(pointerField.ToInt64() - 8);
            try
            {
                if (BitConverter.ToInt32(ReadExact(process, candidate, 4), 0) == 4)
                    return candidate;
            }
            catch { }
        }
        throw new InvalidOperationException("The scheduler owner mode field was not found after normal startup.");
    }

    private static IntPtr SearchPointerRegion(IntPtr process, MemoryRegion region, long expectedValue)
    {
        const int chunkSize = 1024 * 1024;
        byte[] buffer = new byte[chunkSize];
        ulong offset = 0;
        while (offset < region.RegionSize)
        {
            int wanted = (int)Math.Min((ulong)buffer.Length, region.RegionSize - offset);
            IntPtr read;
            IntPtr address = new IntPtr((long)(region.BaseAddress + offset));
            if (!ReadProcessMemory(process, address, buffer, new IntPtr(wanted), out read) || read.ToInt64() != wanted)
                return IntPtr.Zero;
            int alignedStart = (int)((8 - ((region.BaseAddress + offset) & 7)) & 7);
            for (int index = alignedStart; index + 8 <= wanted; index += 8)
                if (BitConverter.ToInt64(buffer, index) == expectedValue)
                    return new IntPtr((long)(region.BaseAddress + offset + (ulong)index));
            offset += (ulong)wanted;
        }
        return IntPtr.Zero;
    }

    private static List<MemoryRegion> EnumerateWritablePrivateRegions(IntPtr process)
    {
        List<MemoryRegion> result = new List<MemoryRegion>();
        ulong address = 0x10000;
        ulong limit = 0x100000000;
        UIntPtr informationSize = new UIntPtr((uint)Marshal.SizeOf(typeof(MemoryBasicInformation64)));

        while (address < limit)
        {
            MemoryBasicInformation64 information;
            UIntPtr queried = VirtualQueryEx(
                process,
                new IntPtr((long)address),
                out information,
                informationSize);
            if (queried == UIntPtr.Zero || information.RegionSize == 0)
                break;

            uint basicProtection = information.Protect & 0xFF;
            if (information.State == MemCommit &&
                information.Type == MemPrivate &&
                basicProtection == PageReadWrite &&
                (information.Protect & PageGuard) == 0 &&
                basicProtection != PageNoAccess)
            {
                result.Add(new MemoryRegion(information.BaseAddress, information.RegionSize));
            }

            ulong next = information.BaseAddress + information.RegionSize;
            if (next <= address)
                break;
            address = next;
        }
        return result;
    }

    private static IntPtr SearchRegion(IntPtr process, MemoryRegion region, long expectedVtable)
    {
        const int chunkSize = 1024 * 1024;
        byte[] buffer = new byte[chunkSize];
        ulong offset = 0;

        while (offset < region.RegionSize)
        {
            int wanted = (int)Math.Min((ulong)buffer.Length, region.RegionSize - offset);
            IntPtr read;
            IntPtr address = new IntPtr((long)(region.BaseAddress + offset));
            if (!ReadProcessMemory(process, address, buffer, new IntPtr(wanted), out read) || read.ToInt64() != wanted)
                return IntPtr.Zero;

            int alignedStart = (int)((8 - ((region.BaseAddress + offset) & 7)) & 7);
            for (int index = alignedStart; index + 16 <= wanted; index += 8)
            {
                if (BitConverter.ToInt64(buffer, index) == expectedVtable &&
                    BitConverter.ToInt64(buffer, index + 8) == 0)
                {
                    return new IntPtr((long)(region.BaseAddress + offset + (ulong)index));
                }
            }
            offset += (ulong)wanted;
        }
        return IntPtr.Zero;
    }

    private static string ResolveGameDirectory()
    {
        string launcherDirectory = AppDomain.CurrentDomain.BaseDirectory;
        if (File.Exists(Path.Combine(launcherDirectory, "DarkSoulsRemastered.exe")))
            return Path.GetFullPath(launcherDirectory);

        throw new InvalidOperationException(
            "DarkSoulsRemastered.exe was not found. Place DSHRLauncher.exe in the game directory beside DarkSoulsRemastered.exe.");
    }

    private static void VerifyExecutable(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("DarkSoulsRemastered.exe was not found.", path);

        string hash;
        using (SHA1 sha1 = SHA1.Create())
        using (FileStream stream = File.OpenRead(path))
            hash = ToHex(sha1.ComputeHash(stream));

        if (!String.Equals(hash, SupportedSha1, StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                "Unsupported DarkSoulsRemastered.exe build.\r\nExpected SHA-1: " + SupportedSha1 + "\r\nFound SHA-1: " + hash);
    }

    private static long GetImageBase(IntPtr process)
    {
        ProcessBasicInformation information = new ProcessBasicInformation();
        int returned;
        int status = NtQueryInformationProcess(
            process,
            0,
            ref information,
            Marshal.SizeOf(typeof(ProcessBasicInformation)),
            out returned);
        if (status != 0)
            throw new InvalidOperationException("NtQueryInformationProcess failed with NTSTATUS 0x" + status.ToString("X8") + ".");

        byte[] pointerBytes = ReadExact(process, new IntPtr(information.PebBaseAddress.ToInt64() + 0x10), IntPtr.Size);
        return IntPtr.Size == 8 ? BitConverter.ToInt64(pointerBytes, 0) : BitConverter.ToInt32(pointerBytes, 0);
    }

    private static byte[] ReadExact(IntPtr process, IntPtr address, int count)
    {
        byte[] result = new byte[count];
        IntPtr read;
        if (!ReadProcessMemory(process, address, result, new IntPtr(count), out read) || read.ToInt64() != count)
            ThrowWin32("ReadProcessMemory failed at 0x" + address.ToInt64().ToString("X"));
        return result;
    }

    private static void WriteExact(IntPtr process, IntPtr address, byte[] bytes)
    {
        IntPtr written;
        if (!WriteProcessMemory(process, address, bytes, new IntPtr(bytes.Length), out written) || written.ToInt64() != bytes.Length)
            ThrowWin32("WriteProcessMemory failed at 0x" + address.ToInt64().ToString("X"));
    }

    private static bool HasArgument(string[] args, string name)
    {
        foreach (string arg in args)
            if (String.Equals(arg, name, StringComparison.OrdinalIgnoreCase))
                return true;
        return false;
    }

    private static string ToHex(byte[] bytes)
    {
        StringBuilder builder = new StringBuilder(bytes.Length * 2);
        foreach (byte value in bytes)
            builder.Append(value.ToString("X2", CultureInfo.InvariantCulture));
        return builder.ToString();
    }

    private static void ThrowWin32(string message)
    {
        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), message);
    }

    private static void Log(string message)
    {
        string line = "[" + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss", CultureInfo.InvariantCulture) + "] " + message;
        try
        {
            File.AppendAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "DSHR.log"), line + Environment.NewLine);
        }
        catch { }
    }
}
