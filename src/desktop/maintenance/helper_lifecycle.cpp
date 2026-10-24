#include "helper_lifecycle.hpp"
#include "durable_files.hpp"
#include "native_runtime.hpp"
#include "native_dialog.hpp"
#include <stdexcept>
#include <algorithm>
#include <thread>
#include <chrono>
#include <map>
#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
extern char** environ;
#endif

namespace marksix::maintenance {
std::map<fs::path, std::string> retireUserHelper(const fs::path& executable) {
    const auto stage = executable.parent_path();
    if (!pathUtf8(stage.filename()).starts_with("marksix-maintenance-")) throw std::runtime_error("Unrecognized user helper staging");
    std::map<fs::path, std::string> files;
    for (const auto& entry : fs::directory_iterator(stage)) {
        if (entry.path() != executable && entry.path() != stage / "request.bin" && entry.path() != stage / "remaining.txt")
            throw std::runtime_error("Unexpected user staging file; retained for review");
        files.emplace(entry.path(), readBoundedFile(entry.path(), 64 * 1024 * 1024));
    }
    if (!files.contains(executable) || !files.contains(stage / "request.bin")) throw std::runtime_error("User helper recovery state is missing");
    try { for (const auto& [path, bytes] : files) { (void)bytes; durableRemove(path); } durableRemove(stage); }
    catch (...) { restoreUserHelper(executable, files); throw; }
    return files;
}
void restoreUserHelper(const fs::path& executable, const std::map<fs::path, std::string>& files) {
    const auto stage = executable.parent_path();
    if (!entryExists(stage)) makePrivateDirectory(stage);
    for (const auto& [path, bytes] : files) if (!entryExists(path)) durableWrite(path, bytes);
#ifndef _WIN32
    if (chmod(executable.c_str(), 0700)) throw std::runtime_error("Cannot restore runnable user retry helper");
#endif
}
#ifdef _WIN32
namespace {
struct ScopedHandle { HANDLE handle; ~ScopedHandle() { if (handle) CloseHandle(handle); } };
std::string base64(const std::string& bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        result += alphabet[a >> 2]; result += alphabet[((a & 3) << 4) | (b >> 4)];
        result += i + 1 < bytes.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        result += i + 2 < bytes.size() ? alphabet[c & 63] : '=';
    }
    return result;
}
std::wstring finalizerEnvironment(const fs::path& powershell) {
    std::map<std::wstring, std::wstring> entries;
    const auto raw = GetEnvironmentStringsW();
    if (!raw) throw std::runtime_error("Cannot create native finalization environment");
    for (auto cursor = raw; *cursor;) {
        const std::wstring entry(cursor); cursor += entry.size() + 1;
        const auto split = entry.find(L'='); if (split == 0 || split == std::wstring::npos) continue;
        auto key = entry.substr(0, split);
        for (auto& letter : key) if (letter >= L'a' && letter <= L'z') letter = wchar_t(letter - L'a' + L'A');
        if (key.starts_with(L"COR_") || key.starts_with(L"COMPLUS_") || key.starts_with(L"DOTNET_") ||
            key.starts_with(L"APPDOMAIN_MANAGER_") || key == L"DEVPATH") continue;
        entries[key] = entry.substr(split + 1);
    }
    FreeEnvironmentStringsW(raw);
    entries[L"PSMODULEPATH"] = (powershell.parent_path() / "Modules").native();
    std::wstring result;
    for (const auto& [key, value] : entries) { result += key + L'=' + value; result += L'\0'; }
    result += L'\0'; return result;
}
// Fixed code only. User-controlled paths and reports arrive as JSON data over
// an inherited anonymous pipe; none are interpolated into PowerShell source.
const char finalizer[] = R"PS(
$ErrorActionPreference = 'Stop'
$reader = [IO.StreamReader]::new([Console]::OpenStandardInput(), [Text.UTF8Encoding]::new($false, $true), $false)
$d = $reader.ReadLine() | ConvertFrom-Json
$report = [string]$d.report
$ok = [bool]$d.ok
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object Windows.Forms.Form
[void]$form.Handle
[Console]::Out.WriteLine('M6FINALIZERREADY1')
[Console]::Out.Flush()
try {
    $owner = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::AutoReset)
    $owner.SafeWaitHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new([IntPtr]([Int64]$d.ownerHandle), $true)
    try { if (-not $owner.WaitOne(30000)) { throw 'Maintenance helper has not exited.' } } finally { $owner.Dispose() }
    if ($ok) {
        $stage = Get-Item -LiteralPath $d.stage -Force
        if (($stage.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            -not $stage.Name.StartsWith('marksix-maintenance-') -or
            [IO.Path]::GetDirectoryName($d.exe) -cne $stage.FullName) { throw 'Changed helper staging directory.' }
        $exe = Get-Item -LiteralPath $d.exe -Force
        $state = Get-Item -LiteralPath $d.state -Force
        if (($exe.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            ($state.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            (Get-FileHash -LiteralPath $exe.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $d.exeHash -or
            (Get-FileHash -LiteralPath $state.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $d.stateHash) { throw 'Changed helper cleanup files.' }
        $expectedCount = 2
        $remainingBytes = $null
        if ($d.remainingHash.Length) {
            $remaining = Get-Item -LiteralPath $d.remaining -Force
            if (($remaining.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
                (Get-FileHash -LiteralPath $remaining.FullName -Algorithm SHA256).Hash.ToLowerInvariant() -cne $d.remainingHash) { throw 'Changed retry report.' }
            $remainingBytes = [IO.File]::ReadAllBytes($remaining.FullName); $expectedCount++
        }
        if (@(Get-ChildItem -LiteralPath $stage.FullName -Force).Count -ne $expectedCount) { throw 'Unexpected staging content.' }
        $exeBytes = [IO.File]::ReadAllBytes($exe.FullName)
        $stateBytes = [IO.File]::ReadAllBytes($state.FullName)
        try {
            [IO.File]::Delete($exe.FullName)
            [IO.File]::Delete($state.FullName)
            if ($null -ne $remainingBytes) { [IO.File]::Delete($d.remaining) }
            [IO.Directory]::Delete($stage.FullName, $false)
            if ([IO.Directory]::Exists($stage.FullName)) { throw 'Helper staging remains.' }
            if ($d.fullHandle -ne '0') {
                $root = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::AutoReset)
                $root.SafeWaitHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new([IntPtr]([Int64]$d.fullHandle), $true)
                try { if (-not $root.WaitOne(120000)) { throw 'Administrator helper cleanup has not finished.' } } finally { $root.Dispose() }
                if (Test-Path -LiteralPath $d.fullStage -ErrorAction Stop) { throw 'Protected retry helper remains; complete cleanup was not achieved.' }
                $gate = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\brucerry\MarkSixStudio\Maintenance')
                if ($null -ne $gate) { $gate.Close(); throw 'System maintenance state remains; retry is required.' }
                $report = $report.Replace('Final helper cleanup is still pending.', 'Temporary helper cleanup is verified.')
                if (-not $d.fullCancelled) { $report = 'Uninstall completed with the listed legacy files preserved. Temporary helpers and operation state have been removed.' + "`r`n`r`n" + $report }
                else { $report = 'Uninstall cancelled. Temporary helpers and operation state have been removed.' + "`r`n`r`n" + $report }
            }
        } catch {
            if (-not [IO.Directory]::Exists($stage.FullName)) { [void][IO.Directory]::CreateDirectory($stage.FullName) }
            if ([IO.Directory]::Exists($stage.FullName)) {
                if (-not [IO.File]::Exists($exe.FullName)) {
                    $f = [IO.File]::Open($exe.FullName, [IO.FileMode]::CreateNew); try { $f.Write($exeBytes, 0, $exeBytes.Length) } finally { $f.Close() }
                }
                if (-not [IO.File]::Exists($state.FullName)) {
                    $f = [IO.File]::Open($state.FullName, [IO.FileMode]::CreateNew); try { $f.Write($stateBytes, 0, $stateBytes.Length) } finally { $f.Close() }
                }
                if ($null -ne $remainingBytes -and -not [IO.File]::Exists($d.remaining)) {
                    $f = [IO.File]::Open($d.remaining, [IO.FileMode]::CreateNew); try { $f.Write($remainingBytes, 0, $remainingBytes.Length) } finally { $f.Close() }
                }
            }
            throw
        }
    }
} catch { $ok = $false; $report = 'Partial cleanup: ' + $_.Exception.Message + "`r`n`r`n" + $report }
if (-not $ok) {
    $report += "`r`n`r`nStandalone retry helper: " + $d.exe
    $temporaryReport = [IO.Path]::Combine($d.stage, '.report-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        $reportBytes = [Text.Encoding]::UTF8.GetBytes($report)
        $f = [IO.File]::Open($temporaryReport, [IO.FileMode]::CreateNew)
        try { $f.Write($reportBytes, 0, $reportBytes.Length); $f.Flush($true) } finally { $f.Close() }
        if ([IO.File]::Exists($d.remaining)) { [IO.File]::Delete($d.remaining) }
        [IO.File]::Move($temporaryReport, $d.remaining)
    } catch { $report += "`r`nThe retry report could not be updated: " + $_.Exception.Message }
    finally { if ([IO.File]::Exists($temporaryReport)) { try { [IO.File]::Delete($temporaryReport) } catch {} } }
}
$form.Text = 'Mark Six Studio maintenance result'; $form.Width = 840; $form.Height = 600
$form.StartPosition = 'CenterScreen'
$text = New-Object Windows.Forms.TextBox
$text.Multiline = $true; $text.ReadOnly = $true; $text.ScrollBars = 'Both'; $text.WordWrap = $true
$text.AccessibleName = 'Maintenance result and preserved legacy files'; $text.Text = $report
$text.Dock = 'Fill'; $form.Controls.Add($text)
$buttons = New-Object Windows.Forms.FlowLayoutPanel; $buttons.Dock = 'Bottom'; $buttons.Height = 48
$close = New-Object Windows.Forms.Button; $close.Text = 'Close'; $close.Width = 100
$close.Add_Click({ $form.Close() }); $buttons.Controls.Add($close); $form.CancelButton = $close
if ($ok -and [IO.File]::Exists($d.application)) {
    $open = New-Object Windows.Forms.Button; $open.Text = 'Reopen app'; $open.Width = 130
    $open.Add_Click({
        $info = New-Object Diagnostics.ProcessStartInfo
        $info.FileName = $d.application; $info.UseShellExecute = $false
        $root = [string]$d.dataRoot
        # Windows command-line quoting doubles backslashes only before a quote
        # and at the closing quote. The selected root is data, never source.
        $root = [regex]::Replace($root, '(\\*)"', '$1$1\"')
        $root = [regex]::Replace($root, '(\\+)$', '$1$1')
        $info.Arguments = '--data-dir "' + $root + '"'
        [Diagnostics.Process]::Start($info) | Out-Null; $form.Close()
    }); $buttons.Controls.Add($open)
}
$form.Controls.Add($buttons); $form.Add_Shown({ $close.Focus() }); [void]$form.ShowDialog()
)PS";
}
#endif
void finishUserHelper(const fs::path& executable, bool success, const std::string& report,
                      const fs::path& application, const fs::path& dataRoot, const FullCleanupFinalization& full) {
    const auto stage = executable.parent_path(), state = stage / "request.bin";
    const auto remaining = stage / "remaining.txt";
    validateContainedPath(executable, stage);
    if (!pathUtf8(stage.filename()).starts_with("marksix-maintenance-")) throw std::runtime_error("Unrecognized helper staging location");
    if (!success) durableWrite(remaining, report);
#ifdef _WIN32
    const auto current = processIdentity(currentProcessId());
    if (!current) throw std::runtime_error("Cannot authenticate helper finalization");
    // The child waits on this exact kernel process object, never a potentially
    // reused PID. It receives synchronization access only.
    ScopedHandle owner{OpenProcess(SYNCHRONIZE, TRUE, currentProcessId())};
    if (!owner.handle) throw std::runtime_error("Cannot retain helper exit identity");
    const auto field = [](const fs::path& path) { return quotedReportField(pathUtf8(path)); };
    const auto payload = std::string("{\"ok\":") + (success ? "true" : "false") + ",\"report\":" + quotedReportField(report) +
        ",\"stage\":" + field(stage) + ",\"exe\":" + field(executable) + ",\"state\":" + field(state) +
        ",\"exeHash\":" + quotedReportField(sha256File(executable)) + ",\"stateHash\":" + quotedReportField(sha256File(state)) +
        ",\"remaining\":" + field(remaining) + ",\"remainingHash\":" + quotedReportField(entryExists(remaining) ? sha256File(remaining) : std::string()) +
        ",\"pid\":" + std::to_string(current->pid) + ",\"started\":\"" + std::to_string(current->started) +
        "\",\"ownerHandle\":\"" + std::to_string(reinterpret_cast<std::uintptr_t>(owner.handle)) +
        "\",\"application\":" + field(application) + ",\"dataRoot\":" + field(dataRoot) +
        ",\"fullHandle\":\"" + std::to_string(full.waitHandle) + "\",\"fullStage\":" + field(full.protectedStage) +
        ",\"fullCancelled\":" + (full.cancelled ? "true" : "false") + "}\n";
    const auto source = pathFromUtf8(finalizer).native();
    const std::string encoded = base64(std::string(reinterpret_cast<const char*>(source.data()), source.size() * sizeof(wchar_t)));
    wchar_t system[MAX_PATH]{}; if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("System tools unavailable");
    const auto program = fs::path(system) / "WindowsPowerShell/v1.0/powershell.exe";
    auto environment = finalizerEnvironment(program);
    auto command = L"\"" + program.native() + L"\" -NoLogo -NoProfile -NonInteractive -STA -EncodedCommand " + pathFromUtf8(encoded).native();
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE}; HANDLE input = nullptr, output = nullptr;
    if (!CreatePipe(&input, &output, &security, 0)) throw std::runtime_error("Cannot create finalization channel");
    SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
    HANDLE acknowledgement = nullptr, response = nullptr;
    if (!CreatePipe(&acknowledgement, &response, &security, 0)) {
        CloseHandle(input); CloseHandle(output); throw std::runtime_error("Cannot create finalization acknowledgement");
    }
    SetHandleInformation(acknowledgement, HANDLE_FLAG_INHERIT, 0);
    const auto sink = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input; startup.hStdOutput = response; startup.hStdError = sink;
    PROCESS_INFORMATION child{};
    const bool started = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), system, &startup, &child) != 0;
    CloseHandle(input); CloseHandle(response); if (sink != INVALID_HANDLE_VALUE) CloseHandle(sink);
    if (!started) { CloseHandle(output); CloseHandle(acknowledgement); throw std::runtime_error("Cannot start native finalization"); }
    bool sent = true; std::size_t position = 0;
    while (position < payload.size()) { DWORD count = 0;
        if (!WriteFile(output, payload.data() + position, DWORD(std::min<std::size_t>(65536, payload.size() - position)), &count, nullptr) || !count) { sent = false; break; }
        position += count;
    }
    CloseHandle(output);
    std::string ready; const auto deadline = GetTickCount64() + 15000;
    while (sent && GetTickCount64() < deadline && ready.find('\n') == std::string::npos && ready.size() < 256) {
        DWORD available = 0;
        if (!PeekNamedPipe(acknowledgement, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available) {
            char bytes[128]{}; DWORD count = 0;
            if (!ReadFile(acknowledgement, bytes, std::min<DWORD>(available, DWORD(sizeof(bytes))), &count, nullptr)) break;
            ready.append(bytes, count);
        } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CloseHandle(acknowledgement);
    const bool acknowledged = ready == "M6FINALIZERREADY1\r\n" || ready == "M6FINALIZERREADY1\n";
    if (!acknowledged) { TerminateProcess(child.hProcess, 1); WaitForSingleObject(child.hProcess, 5000); }
    CloseHandle(child.hThread); CloseHandle(child.hProcess);
    if (!sent || !acknowledged) throw std::runtime_error("Native finalization did not acknowledge its runnable report; retry helper retained");
#else
    (void)full;
    auto result = report; bool complete = success;
    if (complete) {
        const auto executableBytes = readBoundedFile(executable), stateBytes = readBoundedFile(state);
        const bool hasRemaining = entryExists(remaining);
        const auto remainingBytes = hasRemaining ? readBoundedFile(remaining, 64 * 1024 * 1024) : std::string();
        try {
            std::size_t count = 0; for (const auto& entry : fs::directory_iterator(stage)) { (void)entry; ++count; }
            if (count != (hasRemaining ? 3U : 2U)) throw std::runtime_error("Unexpected helper staging contents");
            durableRemove(executable); durableRemove(state);
            if (hasRemaining) durableRemove(remaining);
            durableRemove(stage);
        } catch (const std::exception& error) {
            complete = false; result = std::string("Partial cleanup: ") + error.what() + "\n\n" + result;
            if (entryExists(stage)) {
                if (!entryExists(executable)) { durableWrite(executable, executableBytes); chmod(executable.c_str(), 0700); }
                if (!entryExists(state)) durableWrite(state, stateBytes);
                if (hasRemaining && !entryExists(remaining)) durableWrite(remaining, remainingBytes);
            }
        }
    }
    if (!complete) {
        result += "\n\nStandalone retry helper: " + pathUtf8(executable) + "\nRemaining-target report: " + pathUtf8(remaining);
        durableWrite(remaining, result);
    }
    if (nativeReview("Mark Six Studio maintenance result", result, complete && entryExists(application) ? "Reopen app" : "", "Close")) {
        const auto path = pathUtf8(application), root = pathUtf8(dataRoot);
        char* arguments[]{const_cast<char*>(path.c_str()), const_cast<char*>("--data-dir"), const_cast<char*>(root.c_str()), nullptr};
        pid_t child = 0;
        if (posix_spawn(&child, application.c_str(), nullptr, nullptr, arguments, environ))
            nativeReview("App could not reopen", "Open Mark Six Studio manually. Cleanup's result is unchanged.");
    }
#endif
}
#ifdef _WIN32
FullCleanupFinalization launchPrivilegedFinalizer(const fs::path& executable, const MachineGate& gate, const ProcessIdentity& userAgent) {
    // Administrator code/data stay in a protected directory. This fixed
    // epilogue never deletes anything inside the original user's stage.
    static const char source[] = R"PS(
$ErrorActionPreference = 'Stop'
$r = [IO.StreamReader]::new([Console]::OpenStandardInput(), [Text.UTF8Encoding]::new($false, $true), $false)
$d = $r.ReadLine() | ConvertFrom-Json
[Console]::Out.WriteLine('M6ROOTFINALIZERREADY1'); [Console]::Out.Flush()
$d.stage = [IO.Path]::GetFullPath($d.stage)
$d.exe = [IO.Path]::GetFullPath($d.exe)
$d.userStage = [IO.Path]::GetFullPath($d.userStage)
$backup = @{}
try {
    foreach ($h in @($d.ownerHandle, $d.userHandle)) {
        $wait = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::AutoReset)
        $wait.SafeWaitHandle = [Microsoft.Win32.SafeHandles.SafeWaitHandle]::new([IntPtr]([Int64]$h), $true)
        try { if (-not $wait.WaitOne(60000)) { throw 'A native helper has not exited.' } } finally { $wait.Dispose() }
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(60)
    while (Test-Path -LiteralPath $d.userStage -ErrorAction Stop) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Original-user helper staging remains.' }
        Start-Sleep -Milliseconds 100
    }
    $stage = Get-Item -LiteralPath $d.stage -Force
    if (($stage.Attributes -band [IO.FileAttributes]::ReparsePoint) -or -not $stage.Name.StartsWith('MarkSixStudio-Maintenance-')) { throw 'Protected staging changed.' }
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\brucerry\MarkSixStudio\Maintenance')
    try {
        if ($null -eq $key -or [Convert]::ToBase64String($key.GetValue('Active')) -cne $d.gate) { throw 'Protected maintenance gate changed.' }
    } finally { if ($null -ne $key) { $key.Close() } }
    foreach ($file in @(Get-ChildItem -LiteralPath $stage.FullName -Force)) {
        if ($file.Name -notin @('mark-six-maintenance.exe', 'ledger.bin', '.operation.lock', 'remaining.txt') -or
            ($file.Attributes -band ([IO.FileAttributes]::ReparsePoint -bor [IO.FileAttributes]::Directory))) { throw 'Unexpected protected staging content.' }
        $backup[$file.FullName] = [IO.File]::ReadAllBytes($file.FullName)
    }
    if (-not $backup.ContainsKey($d.exe)) { throw 'Protected retry executable is missing.' }
    foreach ($path in $backup.Keys) { [IO.File]::Delete($path) }
    [IO.Directory]::Delete($stage.FullName, $false)
    $parent = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\brucerry\MarkSixStudio', $true)
    try { $parent.DeleteSubKey('Maintenance', $true) } finally { $parent.Close() }
    $product = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\brucerry\MarkSixStudio')
    $empty = $null -ne $product -and $product.SubKeyCount -eq 0 -and $product.ValueCount -eq 0
    if ($null -ne $product) { $product.Close() }
    if ($empty) {
        $brand = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\brucerry', $true)
        try { $brand.DeleteSubKey('MarkSixStudio', $false) } finally { $brand.Close() }
        $brand = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\brucerry')
        $empty = $null -ne $brand -and $brand.SubKeyCount -eq 0 -and $brand.ValueCount -eq 0
        if ($null -ne $brand) { $brand.Close() }
        if ($empty) {
            $software = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE', $true)
            try { $software.DeleteSubKey('brucerry', $false) } finally { $software.Close() }
        }
    }
    exit 0
} catch {
    $message = 'Protected cleanup remains incomplete: ' + $_.Exception.Message
    try {
        if (-not [IO.Directory]::Exists($d.stage)) {
            $acl = [Security.AccessControl.DirectorySecurity]::new()
            $acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;;0x1200a9;;;BU)')
            [void][IO.Directory]::CreateDirectory($d.stage, $acl)
        }
        foreach ($path in $backup.Keys) {
            if (-not [IO.File]::Exists($path)) {
                $f = [IO.File]::Open($path, [IO.FileMode]::CreateNew)
                try { $b = $backup[$path]; $f.Write($b, 0, $b.Length); $f.Flush($true) } finally { $f.Close() }
            }
        }
        if ([IO.File]::Exists($d.exe)) {
            $acl = [Security.AccessControl.FileSecurity]::new()
            $acl.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)')
            [IO.File]::SetAccessControl($d.exe, $acl)
        }
        [IO.File]::WriteAllText([IO.Path]::Combine($d.stage, 'remaining.txt'), $message + "`r`n" + $d.summary, [Text.UTF8Encoding]::new($false))
    } catch {}
    exit 1
}
)PS";
    const auto current = processIdentity(currentProcessId());
    if (!current || !administrator() || !sameProcessAlive(userAgent)) throw std::runtime_error("Native finalization process identity is unavailable");
    ScopedHandle owner{OpenProcess(SYNCHRONIZE, TRUE, currentProcessId())};
    ScopedHandle user{OpenProcess(SYNCHRONIZE | PROCESS_DUP_HANDLE, FALSE, userAgent.pid)};
    HANDLE inheritedUser = nullptr;
    if (!owner.handle || !user.handle || !DuplicateHandle(GetCurrentProcess(), user.handle, GetCurrentProcess(), &inheritedUser, SYNCHRONIZE, TRUE, 0))
        throw std::runtime_error("Cannot retain exact helper process objects");
    ScopedHandle userWait{inheritedUser};
    wchar_t system[MAX_PATH]{}; if (!GetSystemDirectoryW(system, MAX_PATH)) throw std::runtime_error("Native system directory unavailable");
    const auto program = fs::path(system) / "WindowsPowerShell/v1.0/powershell.exe";
    const auto nativeSource = pathFromUtf8(source).native();
    const auto encoded = base64(std::string(reinterpret_cast<const char*>(nativeSource.data()), nativeSource.size() * sizeof(wchar_t)));
    auto command = L"\"" + program.native() + L"\" -NoLogo -NoProfile -NonInteractive -EncodedCommand " + pathFromUtf8(encoded).native();
    auto environment = finalizerEnvironment(program);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE input = nullptr, output = nullptr, acknowledgement = nullptr, response = nullptr;
    if (!CreatePipe(&input, &output, &security, 0)) throw std::runtime_error("Cannot create privileged finalizer input");
    ScopedHandle inputGuard{input}, outputGuard{output}; SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&acknowledgement, &response, &security, 0)) throw std::runtime_error("Cannot create privileged finalizer acknowledgement");
    ScopedHandle ackGuard{acknowledgement}, responseGuard{response}; SetHandleInformation(acknowledgement, HANDLE_FLAG_INHERIT, 0);
    ScopedHandle sink{CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr)};
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = input; startup.hStdOutput = response; startup.hStdError = sink.handle;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(), system, &startup, &child))
        throw std::runtime_error("Cannot start protected native finalizer");
    ScopedHandle process{child.hProcess}, thread{child.hThread};
    CloseHandle(inputGuard.handle); inputGuard.handle = nullptr; CloseHandle(responseGuard.handle); responseGuard.handle = nullptr;
    try {
        const auto identity = processIdentity(child.dwProcessId);
        if (!identity || identity->uncertain) throw std::runtime_error("Native finalizer identity cannot be established");
        const MachineGate finalGate{gate.nonce, gate.stage, *identity}; updateMachineGateExecutor(finalGate);
        const auto payload = std::string("{\"stage\":") + quotedReportField(pathUtf8(gate.stage)) + ",\"exe\":" + quotedReportField(pathUtf8(executable)) +
            ",\"userStage\":" + quotedReportField(pathUtf8(userAgent.executable.parent_path())) + ",\"ownerHandle\":\"" + std::to_string(reinterpret_cast<std::uintptr_t>(owner.handle)) +
            "\",\"userHandle\":\"" + std::to_string(reinterpret_cast<std::uintptr_t>(userWait.handle)) + "\",\"gate\":" + quotedReportField(base64(encodeMachineGate(finalGate))) +
            ",\"summary\":" + quotedReportField(entryExists(gate.stage / "remaining.txt") ? readBoundedFile(gate.stage / "remaining.txt") : "The original-user result contains the preserved-file inventory.") + "}\n";
        std::size_t offset = 0;
        while (offset < payload.size()) { DWORD count = 0;
            if (!WriteFile(output, payload.data() + offset, DWORD(std::min<std::size_t>(65536, payload.size() - offset)), &count, nullptr) || !count) throw std::runtime_error("Privileged finalizer input interrupted");
            offset += count;
        }
        CloseHandle(outputGuard.handle); outputGuard.handle = nullptr;
        std::string ready; const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline && ready.find('\n') == std::string::npos && ready.size() < 128) {
            DWORD available = 0;
            if (!PeekNamedPipe(acknowledgement, nullptr, 0, nullptr, &available, nullptr)) break;
            if (available) { char bytes[128]{}; DWORD count = 0;
                if (!ReadFile(acknowledgement, bytes, std::min<DWORD>(available, DWORD(sizeof(bytes))), &count, nullptr)) break;
                ready.append(bytes, count);
            } else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (ready != "M6ROOTFINALIZERREADY1\r\n" && ready != "M6ROOTFINALIZERREADY1\n") throw std::runtime_error("Protected finalizer did not acknowledge readiness");
        HANDLE remote = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), process.handle, user.handle, &remote, SYNCHRONIZE, TRUE, 0)) throw std::runtime_error("Cannot provide original-user finalization evidence");
        return {reinterpret_cast<std::uintptr_t>(remote), gate.stage};
    } catch (...) { TerminateProcess(process.handle, 1); WaitForSingleObject(process.handle, 5000); throw; }
}
#endif
}
