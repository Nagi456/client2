#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include "Install.h"
#include "RuntimeWin.h"
#include "DownloadWin.h"
#include "../BridgeChannel.h"
#include <atomic>
#include <thread>
#include <vector>

namespace {
using namespace MyClientLauncher;
enum : int { Game = 100, PickGame, Apply, Guide, Logs, StatusText, Version, CheckUpdates, AutoDetect, Caption = 200, TitleText };
// Gaming-purple theme. Only colours/shapes; behaviour of every control is unchanged.
constexpr COLORREF ColBg = RGB(15, 11, 26), ColPanel = RGB(27, 20, 48), ColText = RGB(236, 230, 250),
                   ColMuted = RGB(150, 138, 180), ColAccent = RGB(139, 92, 246), ColAccentHi = RGB(178, 150, 252),
                   ColAccentDown = RGB(104, 62, 214), ColSecondary = RGB(44, 33, 76), ColSecondaryDown = RGB(68, 50, 118),
                   ColBorder = RGB(92, 70, 156), ColDisabled = RGB(30, 24, 48), ColDisabledText = RGB(110, 100, 140),
                   ColError = RGB(255, 128, 146);
constexpr UINT PreparedMessage = WM_APP + 1;
constexpr UINT UpdatedMessage = WM_APP + 2, PackageMessage = WM_APP + 3, CheckMessage = WM_APP + 4;
enum class State { Idle, Preparing, Installing, Waiting, Checking, Fetching, Attaching };
// Attach mode: the protected GDK executable is never copied or edited. The game is
// started normally and PreLoader.dll is injected into the running process.
bool attachMode = false;
fs::path attachDir;
DWORD injectedPid = 0, seenPid = 0;
ULONGLONG seenAt = 0;
int injectFails = 0;
HWND window;
HFONT font, heading, smallFont, boldFont;
HBRUSH background, panelBrush;
bool statusError = false;
int progressPercent = -1;   // -1 = no progress bar
std::atomic<unsigned long long> copyDone{0}, copyTotal{0};   // written by the game-copy thread
std::wstring progressText;
int dpi = 96;
State state = State::Idle;
HANDLE installer = nullptr;
HANDLE gameProcess = nullptr;
HANDLE hudProcess = nullptr;
DWORD hudGamePid = 0;
fs::path bundle, privateDirectory;
std::thread preparation;
struct PreparationResult { Instance instance; std::string error; };
Instance pending;
Catalog catalog, bundledCatalog;
Release selected;
fs::path packageRoot;
Json sources = Json::object();
struct UpdateResult { Catalog catalog; std::string error; };
struct PackageResult { fs::path root; std::string error; };

void ApplyClient();
void RefreshVersions(const std::string& preferred);
void CheckForUpdates();

ULONGLONG startedAt = 0;
std::vector<HWND> editable;

std::wstring Wide(const std::string& value)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!size && !value.empty()) return L"文字列の形式が不正です。";
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

fs::path KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR text = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &text)))
        throw std::runtime_error("Windowsのユーザーフォルダーを取得できません。");
    const fs::path result(text);
    CoTaskMemFree(text);
    return result;
}

std::wstring Text(int id)
{
    HWND control = GetDlgItem(window, id);
    std::wstring result(GetWindowTextLengthW(control) + 1, L'\0');
    const int count = GetWindowTextW(control, result.data(), static_cast<int>(result.size()));
    result.resize(count);
    return result;
}

void Status(const std::wstring& text)
{
    statusError = text.find(L"できません") != std::wstring::npos || text.find(L"失敗") != std::wstring::npos ||
                  text.find(L"不正") != std::wstring::npos || text.find(L"拒否") != std::wstring::npos;
    SetWindowTextW(GetDlgItem(window, StatusText), text.c_str());
    InvalidateRect(GetDlgItem(window, StatusText), nullptr, TRUE);
    std::ofstream log(privateDirectory / "launcher.log", std::ios::app | std::ios::binary);
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size, nullptr, nullptr);
    log << utf8 << '\n';
}

int Px(int value);

RECT ProgressBarArea()
{
    return RECT{Px(24), Px(418), Px(724), Px(448)};
}

// Status text without writing to launcher.log (progress updates arrive several times a second).
void StatusQuiet(const std::wstring& text)
{
    statusError = false;
    SetWindowTextW(GetDlgItem(window, StatusText), text.c_str());
    InvalidateRect(GetDlgItem(window, StatusText), nullptr, TRUE);
}

void BeginProgress()
{
    std::error_code ignored;
    fs::remove(privateDirectory / "cache" / "progress.txt", ignored);
    progressPercent = 0;
    progressText.clear();
    const RECT area = ProgressBarArea();
    InvalidateRect(window, &area, TRUE);
}

void EndProgress()
{
    progressPercent = -1;
    const RECT area = ProgressBarArea();
    InvalidateRect(window, &area, TRUE);
}

// Reads "percent|phase|index/count|label|received/total" written by PrepareRuntime.ps1.
void PollProgress()
{
    const auto file = privateDirectory / "cache" / "progress.txt";
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return;
    char buffer[2048];
    DWORD read = 0;
    const BOOL ok = ReadFile(handle, buffer, sizeof buffer - 1, &read, nullptr);
    CloseHandle(handle);
    if (!ok || !read) return;
    buffer[read] = '\0';
    std::vector<std::string> field;
    std::string current;
    for (const char* c = buffer; *c; ++c)
    {
        if (*c == '|') { field.push_back(current); current.clear(); }
        else current += *c;
    }
    field.push_back(current);
    if (field.size() < 2) return;
    const int percent = std::clamp(atoi(field[0].c_str()), 0, 100);
    const std::string& phase = field[1];
    const std::wstring index = field.size() > 2 ? Wide(field[2]) : L"";
    const std::wstring label = field.size() > 3 ? Wide(field[3]) : L"";
    const std::wstring detail = field.size() > 4 ? Wide(field[4]) : L"";
    std::wstring message;
    if (phase == "download")
        message = L"ダウンロード中 (" + index + L")\r\n" + label + (detail.empty() ? L"" : L"   " + detail + L" MB");
    else if (phase == "cached") message = L"保存済みのファイルを確認しました (" + index + L")\r\n" + label;
    else if (phase == "extract") message = L"展開中\r\n" + label;
    else if (phase == "activate") message = L"ゲーム本体にローダーを組み込んでいます。1分ほどかかることがあります。";
    else if (phase == "done") message = L"準備が完了しました。Minecraftを起動します…";
    else message = L"ダウンロードの準備をしています…";
    const std::wstring text = L"ローダーの準備中  " + std::to_wstring(percent) + L"%\r\n" + message +
                              L"\r\n初回のみ時間がかかります。このウィンドウは閉じないでください。";
    if (percent == progressPercent && text == progressText) return;
    progressPercent = percent;
    progressText = text;
    StatusQuiet(text);
    const RECT area = ProgressBarArea();
    InvalidateRect(window, &area, TRUE);
}

// Game-copy progress: the copy thread publishes byte counters, the timer shows them.
void PollCopyProgress()
{
    const unsigned long long done = copyDone, total = copyTotal;
    const int percent = total ? static_cast<int>(std::min<unsigned long long>(100, done * 100 / total)) : 0;
    wchar_t size[96];
    if (total >= (1ull << 30))
        swprintf(size, 96, L"%.2f / %.2f GB", done / 1073741824.0, total / 1073741824.0);
    else
        swprintf(size, 96, L"%.0f / %.0f MB", done / 1048576.0, total / 1048576.0);
    const std::wstring text = L"Minecraftをコピー中  " + std::to_wstring(percent) + L"%\r\n" + size +
                              L"\r\nゲーム本体を専用フォルダーへコピーしています。初回のみ時間がかかります。\r\nこのウィンドウは閉じないでください。";
    if (percent == progressPercent && text == progressText) return;
    progressPercent = percent;
    progressText = text;
    StatusQuiet(text);
    const RECT area = ProgressBarArea();
    InvalidateRect(window, &area, TRUE);
}

void Busy(State next)
{
    state = next;
    for (HWND control : editable) EnableWindow(control, state == State::Idle);
}

void Save()
{
    sources[selected.target.id] = fs::path(Text(Game)).u8string();
    Json config{{"sources", sources}, {"selectedVersion", selected.target.id}, {"schemaVersion", 3}};
    const auto temporary = privateDirectory / "launcher.json.tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << config.dump(2);
    output.close();
    if (!output || !MoveFileExW(temporary.c_str(), (privateDirectory / "launcher.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("ランチャー設定を保存できません。");
}

void Pick(int id)
{
    std::vector<wchar_t> path(32768);
    const std::wstring existing = Text(id);
    if (existing.size() < path.size()) std::copy(existing.begin(), existing.end(), path.begin());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof dialog;
    dialog.hwndOwner = window;
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.lpstrFilter = L"Windowsプログラム (*.exe)\0*.exe\0\0";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    const auto title = L"Minecraft " + Wide(selected.target.id) + L"のMinecraft.Windows.exe";
    dialog.lpstrTitle = title.c_str();
    if (GetOpenFileNameW(&dialog))
    {
        SetWindowTextW(GetDlgItem(window, id), path.data());
        Save();
        Status(L"選択を保存しました。「適用して起動」で導入先を確認して起動します。");
    }
}

HANDLE Start(const fs::path& executable, const std::vector<std::wstring>& arguments,
             const fs::path& directory, const fs::path& logPath = {})
{
    std::wstring command = QuoteArg(executable.wstring());
    for (const auto& argument : arguments) command += L" " + QuoteArg(argument);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    HANDLE log = INVALID_HANDLE_VALUE, nullInput = INVALID_HANDLE_VALUE;
    struct LogGuard {
        HANDLE& log; HANDLE& input;
        ~LogGuard() { if (log != INVALID_HANDLE_VALUE) CloseHandle(log); if (input != INVALID_HANDLE_VALUE) CloseHandle(input); }
    } logGuard{log, nullInput};
    if (!logPath.empty())
    {
        SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        nullInput = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
        if (log == INVALID_HANDLE_VALUE || nullInput == INVALID_HANDLE_VALUE)
            throw std::runtime_error("準備処理のログを作成できません。");
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = startup.hStdError = log;
        startup.hStdInput = nullInput;
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, !logPath.empty(),
                        logPath.empty() ? 0 : CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &process))
        throw std::runtime_error("プログラムを起動できません。Windowsエラー: " + std::to_string(GetLastError()));
    CloseHandle(process.hThread);
    return process.hProcess;
}

// Match the executable's directory, including console builds.
// Denied access to a Minecraft process blocks replacement rather than assuming
// the game is stopped. Other accessible Minecraft instances are ignored.
DWORD GamePid(const fs::path& directory)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("起動中のゲームを確認できません。");
    struct SnapshotGuard { HANDLE handle; ~SnapshotGuard() { CloseHandle(handle); } } guard{snapshot};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    DWORD found = 0;
    if (!Process32FirstW(snapshot, &entry)) throw std::runtime_error("起動中のゲーム一覧を取得できません。");
    do
    {
        const auto name = Lower(fs::path(entry.szExeFile).u8string());
        if (name != "minecraft.windows.exe" && name != "minecraft.windows.console.exe") continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process)
        {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // Already exited.
            throw std::runtime_error("Minecraftの確認に失敗しました。ゲームを終了して再試行してください。");
        }
        std::vector<wchar_t> path(32768);
        DWORD length = static_cast<DWORD>(path.size());
        const bool read = QueryFullProcessImageNameW(process, 0, path.data(), &length) != FALSE;
        DWORD exitCode = STILL_ACTIVE;
        GetExitCodeProcess(process, &exitCode);
        CloseHandle(process);
        if (!read)
        {
            if (exitCode != STILL_ACTIVE) continue;
            throw std::runtime_error("Minecraftの実行パスを取得できません。ゲームを終了して再試行してください。");
        }
        std::error_code error;
        if (fs::equivalent(fs::path(std::wstring(path.data(), length)).parent_path(), directory, error))
        {
            if (found) throw std::runtime_error("同じ環境のMinecraftが複数起動しています。1つにしてください。");
            found = entry.th32ProcessID;
        }
        else if (error) throw std::runtime_error("Minecraftの導入先を比較できません。");
    } while (Process32NextW(snapshot, &entry));
    if (GetLastError() != ERROR_NO_MORE_FILES)
        throw std::runtime_error("ゲーム一覧の確認が途中で失敗しました。再試行してください。");
    return found;
}

struct WindowSearch { DWORD pid; HWND found; };
BOOL CALLBACK FindGameWindow(HWND handle, LPARAM parameter)
{
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(handle, &pid);
    wchar_t title[256]{};
    GetWindowTextW(handle, title, 256);
    if (pid == search.pid && IsWindowVisible(handle) && std::wstring(title) == L"Minecraft")
    {
        search.found = handle;
        return FALSE;
    }
    return TRUE;
}

bool Ready(DWORD pid)
{
    if (!pid) return false;
    WindowSearch search{pid, nullptr};
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&search));
    if (!search.found) return false;
    MyClientIpc::Channel channel;
    if (!channel.Open(pid, false)) return false;
    const auto locked = channel.Lock(0);
    if (locked == WAIT_ABANDONED) { channel.Unlock(); return false; }
    if (locked != WAIT_OBJECT_0) return false;
    const bool valid = MyClientIpc::HasValidHeader(*channel.Data(), pid);
    channel.Unlock();
    return valid;
}

void StartHud(DWORD pid)
{
    if (hudProcess)
    {
        if (WaitForSingleObject(hudProcess, 0) == WAIT_TIMEOUT && hudGamePid == pid)
        {
            Status(L"HUDは既に起動しています。ゲームでGを押して設定を開いてください。");
            Busy(State::Idle);
            return;
        }
        CloseHandle(hudProcess);
        hudProcess = nullptr;
    }
    hudProcess = Start(bundle / "MyClient.exe", {L"--minecraft-pid=" + std::to_wstring(pid)}, bundle);
    hudGamePid = pid;
    Busy(State::Idle);
    Status(L"DLLへの接続を確認し、HUDを起動しました。\r\nワールドに入り、G → Arrow Counter / Block OutlineをONにしてください。");
}

void LaunchGame()
{
    RequireExecutableVersion(pending.directory / "Minecraft.Windows.exe", selected.target);
    RequireRuntime(pending.directory, packageRoot / "runtime.lock.json", selected.target);
    const DWORD pid = GamePid(pending.directory);
    if (pid)
    {
        if (!Ready(pid))
            throw std::runtime_error("Minecraftが起動中です。DLLを導入するには一度ゲームを終了してください。");
        StartHud(pid);
        return;
    }
    const auto backup = InstallMod(packageRoot / "mods/MyClientData", pending.directory, selected.target);
    if (!backup.empty()) Status(L"以前のMODをバックアップしました:\r\n" + backup.wstring());
    if (gameProcess) { CloseHandle(gameProcess); gameProcess = nullptr; }
    // The official PeEditor/PreLoader prepare the managed executable; no
    // external launcher, protocol activation or registration lookup is used.
    gameProcess = Start(pending.directory / "Minecraft.Windows.exe", {}, pending.directory);
    startedAt = GetTickCount64();
    Busy(State::Waiting);
    Status(L"Minecraftを直接起動しました。DLLへの接続を待っています…\r\n接続を確認するとHUDを自動で起動します。");
}

// Any running Minecraft.Windows.exe, regardless of its install folder.
DWORD GamePidAny()
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) throw std::runtime_error("起動中のゲームを確認できません。");
    struct SnapshotGuard { HANDLE handle; ~SnapshotGuard() { CloseHandle(handle); } } guard{snapshot};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    DWORD found = 0;
    if (!Process32FirstW(snapshot, &entry)) throw std::runtime_error("起動中のゲーム一覧を取得できません。");
    do
    {
        if (_wcsicmp(entry.szExeFile, L"Minecraft.Windows.exe") != 0) continue;
        if (found) throw std::runtime_error("Minecraftが複数起動しています。1つにしてください。");
        found = entry.th32ProcessID;
    } while (Process32NextW(snapshot, &entry));
    return found;
}

struct InjectResult { bool ok = false; std::string message; };

// Classic LoadLibraryW remote thread. Returns ok only if LoadLibraryW returned a module.
InjectResult InjectDll(DWORD pid, const fs::path& dll)
{
    InjectResult result;
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                 PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!process) { result.message = "プロセスを開けません (Windowsエラー " + std::to_string(GetLastError()) + ")"; return result; }
    struct Guard { HANDLE h; ~Guard() { CloseHandle(h); } } guard{process};
    const std::wstring path = dll.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { result.message = "メモリ確保に失敗 (Windowsエラー " + std::to_string(GetLastError()) + ")"; return result; }
    SIZE_T written = 0;
    if (!WriteProcessMemory(process, remote, path.c_str(), bytes, &written) || written != bytes)
    {
        result.message = "パスの書き込みに失敗 (Windowsエラー " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return result;
    }
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
    HANDLE thread = loadLibrary ? CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr) : nullptr;
    if (!thread)
    {
        result.message = "スレッド作成に失敗 (Windowsエラー " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return result;
    }
    const DWORD wait = WaitForSingleObject(thread, 30000);
    DWORD module = 0;
    GetExitCodeThread(thread, &module);
    CloseHandle(thread);
    if (wait == WAIT_OBJECT_0) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    if (wait != WAIT_OBJECT_0) result.message = "LoadLibraryが30秒で終わりませんでした";
    else if (!module) result.message = "LoadLibraryWが失敗しました。依存DLLの不足、またはファイルのアクセス権を確認してください";
    else result.ok = true;
    return result;
}

// UWP/GDK processes cannot always load DLLs from a user folder; allow app packages to read it.
void GrantAppPackageAccess(const fs::path& directory)
{
    std::vector<wchar_t> systemPath(32768);
    const UINT length = GetSystemDirectoryW(systemPath.data(), static_cast<UINT>(systemPath.size()));
    if (!length || length >= systemPath.size()) return;
    const auto icacls = fs::path(std::wstring(systemPath.data(), length)) / "icacls.exe";
    HANDLE process = Start(icacls, {directory.wstring(), L"/grant", L"*S-1-15-2-1:(OI)(CI)(RX)",
                                    L"/grant", L"*S-1-15-2-2:(OI)(CI)(RX)", L"/T", L"/C", L"/Q"},
                           directory, privateDirectory / "icacls.log");
    WaitForSingleObject(process, 60000);
    CloseHandle(process);
}

void LaunchAttach()
{
    RequireAttachRuntime(attachDir, packageRoot / "runtime.lock.json", selected.target);
    const DWORD pid = GamePidAny();
    if (pid && Ready(pid)) { StartHud(pid); return; }
    const auto backup = InstallMod(packageRoot / "mods/MyClientData", attachDir, selected.target);
    if (!backup.empty()) Status(L"以前のMODをバックアップしました:\r\n" + backup.wstring());
    GrantAppPackageAccess(attachDir);
    injectedPid = seenPid = 0;
    seenAt = 0;
    injectFails = 0;
    startedAt = GetTickCount64();
    if (!pid)
    {
        const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
            L"shell:AppsFolder\\Microsoft.MinecraftUWP_8wekyb3d8bbwe!Game", nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32)
            throw std::runtime_error("Minecraftを起動できません。Xboxアプリから一度起動できるか確認してください。(ShellExecute " + std::to_string(result) + ")");
        Status(L"Minecraftを通常起動しました。起動後にローダーを注入します…\r\n接続を確認するとHUDを自動で起動します。");
    }
    else Status(L"起動中のMinecraftへローダーを注入します…");
    Busy(State::Attaching);
}

void ApplyAttach(const fs::path& source)
{
    // Only MicrosoftGame.config is read; the protected executable is never opened.
    pending = ResolveGame(source, selected.target, false);
    attachMode = true;
    attachDir = privateDirectory / "attach" / fs::u8path(selected.target.id);
    fs::create_directories(attachDir);
    const auto marker = attachDir / "MyClient-game.json";
    if (!fs::exists(marker))
    {
        std::ofstream output(marker, std::ios::binary);
        output << Json{{"owner", "MyClient"}, {"schemaVersion", 1}, {"mode", "attach"}}.dump(2);
        output.close();
        if (!output) throw std::runtime_error("注入用フォルダーの情報を保存できません。");
    }
    bool ready = false;
    try { RequireAttachRuntime(attachDir, packageRoot / "runtime.lock.json", selected.target); ready = true; }
    catch (const std::exception&) { /* First use or interrupted preparation. */ }
    if (ready) { LaunchAttach(); return; }
    std::vector<wchar_t> systemPath(32768);
    const UINT length = GetSystemDirectoryW(systemPath.data(), static_cast<UINT>(systemPath.size()));
    if (!length || length >= systemPath.size()) throw std::runtime_error("Windowsのシステムフォルダーを取得できません。");
    const auto powershell = fs::path(std::wstring(systemPath.data(), length)) / "WindowsPowerShell/v1.0/powershell.exe";
    installer = Start(powershell,
        {L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File",
         (bundle / "PrepareRuntime.ps1").wstring(), L"-ManifestPath", (packageRoot / "runtime.lock.json").wstring(),
         L"-ExpectedGameVersion", Wide(selected.target.runtimeGame), L"-ExpectedLoaderVersion", Wide(selected.target.loader),
         L"-GameDirectory", attachDir.wstring(), L"-CacheDirectory", (privateDirectory / "cache").wstring(), L"-Attach"},
         attachDir, privateDirectory / "runtime-setup.log");
    Busy(State::Installing);
    BeginProgress();
    Status(L"ローダーを公式配布元から準備しています（本体は変更しません）。初回はインターネット接続が必要です。\r\n進行状況は「ログを開く」で確認できます。");
}

void PrepareRuntimeOrLaunch()
{
    RequireExecutableVersion(pending.directory / "Minecraft.Windows.exe", selected.target);
    if (GamePid(pending.directory)) throw std::runtime_error("ローダーの導入前にMinecraftを終了してください。");
    bool ready = false;
    try { RequireRuntime(pending.directory, packageRoot / "runtime.lock.json", selected.target); ready = true; }
    catch (const std::exception&) { /* First use or interrupted preparation. */ }
    if (ready) { LaunchGame(); return; }
    const auto originalImports = ImportedDlls(pending.directory / "MyClient-original.exe");
    if (originalImports.count("preloader.dll") || originalImports.count("levilauncher.dll"))
        throw std::runtime_error("保存した元のゲーム本体が変更されています。公式インストール元から準備し直してください。");
    std::vector<wchar_t> systemPath(32768);
    const UINT length = GetSystemDirectoryW(systemPath.data(), static_cast<UINT>(systemPath.size()));
    if (!length || length >= systemPath.size()) throw std::runtime_error("Windowsのシステムフォルダーを取得できません。");
    const auto powershell = fs::path(std::wstring(systemPath.data(), length)) / "WindowsPowerShell/v1.0/powershell.exe";
    installer = Start(powershell,
        {L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-ExecutionPolicy", L"Bypass", L"-File",
         (bundle / "PrepareRuntime.ps1").wstring(), L"-ManifestPath", (packageRoot / "runtime.lock.json").wstring(),
         L"-ExpectedGameVersion", Wide(selected.target.runtimeGame), L"-ExpectedLoaderVersion", Wide(selected.target.loader),
         L"-GameDirectory", pending.directory.wstring(), L"-CacheDirectory", (privateDirectory / "cache").wstring()},
         pending.directory, privateDirectory / "runtime-setup.log");
    Busy(State::Installing);
    BeginProgress();
    Status(L"必要なローダーを公式配布元から準備しています。初回はインターネット接続が必要です。\r\n準備が終わるとMinecraftを起動します。進行状況は「ログを開く」で確認できます。");
}

bool FindInstalledGame(bool quiet);

void ApplyClient()
{
    Save();
    attachMode = false;
    // GDK saves are still shared; prevent concurrent managed instances.
    const auto games = privateDirectory / "game";
    if (fs::is_directory(games))
        for (const auto& entry : fs::directory_iterator(games))
            if (entry.is_directory() && entry.path().filename() != selected.target.id && GamePid(entry.path()))
                throw std::runtime_error("別バージョンのMinecraftを終了してから切り替えてください。");
    RequireMod(packageRoot / "mods/MyClientData", selected.target);
    RequireX64Pe(bundle / "MyClient.exe");
    const auto destination = privateDirectory / "game" / selected.target.id;
    if (fs::exists(destination))
    {
        pending = PrepareManagedGame({}, destination, selected.target);
        SetWindowTextW(GetDlgItem(window, Game), (destination / "Minecraft.Windows.exe").c_str());
        Save();
        const DWORD pid = GamePid(pending.directory);
        if (pid)
        {
            if (!Ready(pid)) throw std::runtime_error("Minecraftが起動中です。準備や更新の前にゲームを終了してください。");
            StartHud(pid);
            return;
        }
        PrepareRuntimeOrLaunch();
        return;
    }
    fs::path source(Text(Game));
    if (source.empty() || Lower(source.filename().u8string()) != "minecraft.windows.exe")
    {
        if (FindInstalledGame(true)) source = fs::path(Text(Game));
    }
    // Xbox-app (GDK) installs protect the executable: copying it is impossible, so attach instead.
    std::error_code existsError;
    if (!source.empty() && fs::exists(source, existsError) && ProbeRead(source) == ERROR_ACCESS_DENIED)
    {
        ApplyAttach(source);
        return;
    }
    const auto instance = ResolveGame(source, selected.target);
    RequireExecutableVersion(source, selected.target);
    if (GamePid(instance.directory)) throw std::runtime_error("ゲーム本体をコピーする前にMinecraftを終了してください。");
    Busy(State::Preparing);
    copyDone = 0;
    copyTotal = 0;
    BeginProgress();
    Status(L"Minecraftを自作ソフト用のフォルダーへコピーしています…\r\n初回は時間とゲーム本体分の空き容量が必要です。");
    const auto target = selected.target;
    preparation = std::thread([source, destination, target] {
        auto result = new PreparationResult;
        try
        {
            result->instance = PrepareManagedGame(source, destination, target,
                [](unsigned long long done, unsigned long long total) { copyDone = done; copyTotal = total; });
        }
        catch (const std::exception& error) { result->error = error.what(); }
        if (!PostMessageW(window, PreparedMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
    });
}

void Tick()
{
    if (state == State::Installing)
    {
        const DWORD wait = WaitForSingleObject(installer, 0);
        if (wait == WAIT_TIMEOUT) { PollProgress(); return; }
        PollProgress();
        EndProgress();
        DWORD exitCode = 1;
        GetExitCodeProcess(installer, &exitCode);
        CloseHandle(installer);
        installer = nullptr;
        Busy(State::Idle);
        if (wait != WAIT_OBJECT_0 || exitCode)
            throw std::runtime_error("ローダーの準備に失敗しました。ログを開いて確認してください。終了コード: " + std::to_string(exitCode));
        if (attachMode) LaunchAttach(); else LaunchGame();
    }
    else if (state == State::Preparing)
    {
        PollCopyProgress();
    }
    else if (state == State::Attaching)
    {
        const DWORD pid = GamePidAny();
        if (Ready(pid)) { StartHud(pid); return; }
        const ULONGLONG now = GetTickCount64();
        if (pid && injectedPid != pid)
        {
            if (seenPid != pid) { seenPid = pid; seenAt = now; }
            if (now - seenAt >= 1500)
            {
                const auto result = InjectDll(pid, attachDir / "PreLoader.dll");
                if (result.ok)
                {
                    injectedPid = pid;
                    Status(L"PreLoader.dllを注入しました。DLLへの接続を待っています…");
                }
                else if (++injectFails >= 20)
                {
                    Busy(State::Idle);
                    throw std::runtime_error("ローダーの注入に失敗しました: " + result.message);
                }
                else if (injectFails == 1 || injectFails % 5 == 0)
                    Status(L"注入を再試行しています (" + std::to_wstring(injectFails) + L"回目): " + Wide(result.message));
            }
        }
        if (now - startedAt > 180000)
        {
            Busy(State::Idle);
            Status(injectedPid ?
                L"注入はしましたが、DLLへの接続を確認できませんでした。\r\nPreLoaderが後読みに対応していない可能性があります。ログを開いて確認してください。" :
                L"Minecraftのプロセスを確認できませんでした。Xboxアプリから起動できるか確認してください。");
        }
    }
    else if (state == State::Waiting)
    {
        const DWORD pid = GamePid(pending.directory);
        if (Ready(pid)) { StartHud(pid); return; }
        if (!pid && gameProcess && WaitForSingleObject(gameProcess, 0) == WAIT_OBJECT_0)
        {
            DWORD exitCode = 0;
            GetExitCodeProcess(gameProcess, &exitCode);
            Busy(State::Idle);
            throw std::runtime_error("Minecraftが接続前に終了しました。Windowsエラー/終了コード: " + std::to_string(exitCode) +
                "。Xboxアプリでの起動、Gaming Services・GameInput・VC++ランタイムを確認してください。");
        }
        if (GetTickCount64() - startedAt > 120000)
        {
            Busy(State::Idle);
            Status(L"接続を確認できませんでした。ゲームのログとWindowsの必要な部品を確認してください。\r\nゲーム起動後に「適用して起動」を押すと、読み込み済みのDLLへの接続を再試行できます。");
        }
    }
}

int Px(int value) { return MulDiv(value, dpi, 96); }

void DrawButton(const DRAWITEMSTRUCT& item)
{
    const bool disabled = (item.itemState & ODS_DISABLED) != 0, pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool primary = item.CtlID == Apply;
    const COLORREF fill = disabled ? ColDisabled : primary ? (pressed ? ColAccentDown : ColAccent)
                                                          : (pressed ? ColSecondaryDown : ColSecondary);
    const COLORREF edge = disabled ? ColDisabled : primary ? ColAccentHi : ColBorder;
    HDC dc = item.hDC;
    RECT area = item.rcItem;
    FillRect(dc, &area, background);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HBRUSH brush = CreateSolidBrush(fill);
    HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, brush);
    RoundRect(dc, area.left, area.top, area.right - 1, area.bottom - 1, Px(12), Px(12));
    if ((item.itemState & ODS_FOCUS) && !disabled)
    {
        HPEN ring = CreatePen(PS_SOLID, 1, ColAccentHi);
        SelectObject(dc, ring);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        RoundRect(dc, area.left + Px(3), area.top + Px(3), area.right - 1 - Px(3), area.bottom - 1 - Px(3), Px(8), Px(8));
        SelectObject(dc, pen);
        DeleteObject(ring);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    DeleteObject(brush);
    wchar_t label[128]{};
    GetWindowTextW(item.hwndItem, label, 127);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? ColDisabledText : primary ? RGB(255, 255, 255) : ColText);
    HGDIOBJ oldFont = SelectObject(dc, primary ? boldFont : font);
    if (pressed) OffsetRect(&area, 0, Px(1));
    DrawTextW(dc, label, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);
}
HWND Control(const wchar_t* type, const wchar_t* text, int x, int y, int w, int h,
             int id = 0, DWORD style = 0, bool large = false)
{
    HWND handle = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        Px(x), Px(y), Px(w), Px(h), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (!handle) throw std::runtime_error("画面を作成できません。");
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(large ? heading : font), TRUE);
    return handle;
}

// Candidate Minecraft.Windows.exe files: the running game first, then every XboxGames
// folder on local drives. Nothing is modified; each candidate is validated by ResolveGame.
std::wstring searchReport;   // where GameCandidates() looked (shown when nothing is found)

std::vector<fs::path> GameCandidates()
{
    std::vector<fs::path> list;
    searchReport.clear();
    const auto managedRoot = Lower(privateDirectory.u8string());
    const auto add = [&](const fs::path& path) {
        // Protected (Xbox app) installs: the file exists but cannot be opened.
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES && GetLastError() != ERROR_ACCESS_DENIED) return;
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) return;
        if (Lower(path.u8string()).rfind(managedRoot, 0) == 0) return; // our own private copy
        std::error_code error;
        for (const auto& known : list)
            if (Lower(known.u8string()) == Lower(path.u8string())) return;
        list.push_back(path);
    };
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof entry;
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        {
            if (_wcsicmp(entry.szExeFile, L"Minecraft.Windows.exe") != 0) continue;
            const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            std::vector<wchar_t> buffer(32768);
            DWORD size = static_cast<DWORD>(buffer.size());
            if (QueryFullProcessImageNameW(process, 0, buffer.data(), &size))
                add(fs::path(std::wstring(buffer.data(), size)));
            CloseHandle(process);
        }
        CloseHandle(snapshot);
    }
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
    {
        if (!(drives & (1u << i))) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        const UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        std::error_code error;
        const fs::path xbox = fs::path(root) / L"XboxGames";
        const bool exists = fs::is_directory(xbox, error);
        searchReport += root + L"XboxGames: " + (exists ? L"あり" : L"なし") + L"\r\n";
        error.clear();
        for (fs::directory_iterator it(xbox, error), end; !error && it != end; it.increment(error))
        {
            searchReport += L"  - " + it->path().filename().wstring() + L"\r\n";
            add(it->path() / L"Content" / L"Minecraft.Windows.exe");
        }
    }
    // Installed-package registry (current user): covers installs outside XboxGames.
    HKEY packages = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
            0, KEY_READ, &packages) == ERROR_SUCCESS)
    {
        wchar_t name[512];
        for (DWORD index = 0;; ++index)
        {
            DWORD length = 512;
            if (RegEnumKeyExW(packages, index, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (_wcsnicmp(name, L"Microsoft.MinecraftUWP_", 23) != 0) continue;
            wchar_t folder[2048]{};
            DWORD size = sizeof folder;
            if (RegGetValueW(packages, name, L"PackageRootFolder", RRF_RT_REG_SZ, nullptr, folder, &size) != ERROR_SUCCESS) continue;
            searchReport += std::wstring(L"パッケージ: ") + folder + L"\r\n";
            add(fs::path(folder) / L"Content" / L"Minecraft.Windows.exe");
            add(fs::path(folder) / L"Minecraft.Windows.exe");
        }
        RegCloseKey(packages);
    }
    else searchReport += L"パッケージ情報: 読み取れません\r\n";
    return list;
}

bool FindInstalledGame(bool quiet)
{
    std::string lastError;
    size_t checked = 0;
    for (const auto& candidate : GameCandidates())
    {
        ++checked;
        try
        {
            ResolveGame(candidate, selected.target);
            SetWindowTextW(GetDlgItem(window, Game), candidate.c_str());
            Status(L"本体を自動検出しました:\r\n" + candidate.wstring() +
                   L"\r\n「適用して起動」で導入先を確認して起動します。");
            return true;
        }
        catch (const std::exception& error)
        {
            lastError = error.what();
            // Xbox-app (GDK) installs are protected: the exe cannot be opened, but the
            // folder's MicrosoftGame.config can. Accept it; "適用して起動" then uses attach mode.
            if (ProbeRead(candidate) == ERROR_ACCESS_DENIED)
            {
                try
                {
                    ResolveGame(candidate, selected.target, false);
                    SetWindowTextW(GetDlgItem(window, Game), candidate.c_str());
                    Status(L"保護されたXbox版の本体を検出しました:\r\n" + candidate.wstring() +
                           L"\r\n本体は変更せず、起動後にローダーを注入する方式で起動します。「適用して起動」を押してください。");
                    return true;
                }
                catch (const std::exception& inner) { lastError = inner.what(); }
            }
        }
    }
    if (!quiet)
    {
        std::wstring message = L"Minecraft " + Wide(selected.target.id) + L"の本体を自動検出できませんでした。";
        if (checked) message += L"\r\n見つかった本体の確認結果: " + Wide(lastError);
        else message += L"\r\nXboxGamesフォルダーや起動中のMinecraftが見つかりません。「選択…」で指定してください。\r\n確認した場所:\r\n" +
                        (searchReport.empty() ? std::wstring(L"(なし)") : searchReport);
        Status(message);
    }
    return false;
}

void SelectVersion()
{
    const auto index = SendDlgItemMessageW(window, Version, CB_GETCURSEL, 0, 0);
    if (index < 0 || static_cast<size_t>(index) >= catalog.releases.size()) return;
    selected = catalog.releases[static_cast<size_t>(index)];
    fs::path path = fs::u8path(sources.value(selected.target.id, std::string{}));
    const auto managed = privateDirectory / "game" / selected.target.id / "Minecraft.Windows.exe";
    if (fs::is_regular_file(managed)) path = managed;
    if (!path.empty() && Lower(path.filename().u8string()) != "minecraft.windows.exe") path.clear();
    SetWindowTextW(GetDlgItem(window, Game), path.c_str());
    EnableWindow(GetDlgItem(window, Apply), selected.available);
    EnableWindow(GetDlgItem(window, PickGame), selected.available);
    EnableWindow(GetDlgItem(window, AutoDetect), selected.available);
    // First time for this version: look for an installed game quietly.
    const bool detected = selected.available && path.empty() && FindInstalledGame(true);
    if (!detected)
        Status(selected.available ? L"選択した版の本体を準備して起動できます。DLLは自動取得・検査します。" : Wide(selected.reason));
}

void RefreshVersions(const std::string& preferred)
{
    SendDlgItemMessageW(window, Version, CB_RESETCONTENT, 0, 0);
    size_t chosen = 0;
    for (size_t i = 0; i < catalog.releases.size(); ++i)
    {
        const auto& release = catalog.releases[i];
        const auto label = Wide(release.target.id) + (release.available ? L"  — DLLあり" : L"  — 対応待ち");
        SendDlgItemMessageW(window, Version, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (release.target.id == preferred) chosen = i;
    }
    SendDlgItemMessageW(window, Version, CB_SETCURSEL, chosen, 0);
    SelectVersion();
}

void CheckForUpdates()
{
    Busy(State::Checking);
    Status(L"対応バージョンとDLLの更新を確認しています…");
    const auto previous = catalog;
    preparation = std::thread([previous] {
        auto result = new UpdateResult;
        const auto temporary = privateDirectory / "catalog.download";
        try {
            DownloadHttps(CatalogUrl, temporary, 1024 * 1024);
            result->catalog = ParseCatalog(ReadJson(temporary));
            CheckAdvance(previous, result->catalog);
            if (!MoveFileExW(temporary.c_str(), (privateDirectory / "catalog.json").c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("更新一覧を保存できません。");
        }
        catch (const std::exception& error) { result->error = error.what(); }
        std::error_code ignored; fs::remove(temporary, ignored);
        if (!PostMessageW(window, UpdatedMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
    });
}

void FetchAndApply()
{
    if (!selected.available) throw std::runtime_error(selected.reason);
    Save();
    Busy(State::Fetching);
    Status(L"選択した版のDLLを取得・検査しています…\r\n保存済みの同じ版は再ダウンロードしません。");
    const auto release = selected;
    preparation = std::thread([release] {
        auto result = new PackageResult;
        try {
            bool local = false;
            for (const auto& candidate : bundledCatalog.releases)
                if (candidate.available && candidate.target.id == release.target.id && candidate.files == release.files &&
                    candidate.revision == release.revision && candidate.target.loader == release.target.loader) local = true;
            result->root = AcquirePackage(privateDirectory / "packages", release, Sha256File,
                [local, release](const std::string& url, const fs::path& destination, uint64_t limit) {
                    if (!local) { DownloadHttps(url, destination, limit); return; }
                    for (const auto& file : release.files.items())
                        if (file.value().at("url") == url)
                        {
                            // Extra game versions ship under versions/<id>/; 26.51.1 stays at the bundle root.
                            auto source = SafeChild(bundle / "versions" / fs::u8path(release.target.id), file.key());
                            if (!fs::is_regular_file(source)) source = SafeChild(bundle, file.key());
                            fs::copy_file(source, destination);
                            return;
                        }
                    throw std::runtime_error("同梱更新ファイルが見つかりません。");
                });
        }
        catch (const std::exception& error) { result->error = error.what(); }
        if (!PostMessageW(window, PackageMessage, 0, reinterpret_cast<LPARAM>(result))) delete result;
    });
}

void BuildUi()
{
    Control(L"STATIC", L"MyClient Launcher", 26, 16, 680, 40, TitleText, 0, true);
    Control(L"STATIC", L"Minecraftのバージョン", 28, 78, 260, 24);
    editable.push_back(Control(L"COMBOBOX", L"", 28, 106, 430, 220, Version, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP));
    editable.push_back(Control(L"BUTTON", L"更新を確認", 478, 104, 240, 36, CheckUpdates, WS_TABSTOP | BS_OWNERDRAW));
    Control(L"STATIC", L"選択した版のMinecraft本体（初回は自動検出、または「選択…」で指定）", 28, 156, 690, 24);
    editable.push_back(Control(L"EDIT", L"", 28, 186, 470, 34, Game, WS_BORDER | ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP));
    editable.push_back(Control(L"BUTTON", L"自動検出", 508, 185, 100, 36, AutoDetect, WS_TABSTOP | BS_OWNERDRAW));
    editable.push_back(Control(L"BUTTON", L"選択…", 618, 185, 100, 36, PickGame, WS_TABSTOP | BS_OWNERDRAW));
    SendMessageW(Control(L"STATIC", L"DLLは版ごとに保存し、公開された更新を起動時に確認します。\r\n本体の自動取得・ワールドの版ごとの分離はまだ利用できません。", 28, 236, 690, 40, Caption), WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), TRUE);
    editable.push_back(Control(L"BUTTON", L"適用して起動", 28, 292, 400, 56, Apply, WS_TABSTOP | BS_OWNERDRAW));
    editable.push_back(Control(L"BUTTON", L"使い方・必要な部品", 442, 292, 276, 56, Guide, WS_TABSTOP | BS_OWNERDRAW));
    SendMessageW(Control(L"STATIC", L"対応待ちの版は起動できません。対応DLLの公開後に利用可能になります。\r\n配布先が未公開・オフラインの場合は保存済み一覧を使います。", 28, 362, 690, 40, Caption), WM_SETFONT, reinterpret_cast<WPARAM>(smallFont), TRUE);
    Control(L"STATIC", L"", 38, 473, 672, 104, StatusText);
    editable.push_back(Control(L"BUTTON", L"ログを開く", 578, 598, 140, 36, Logs, WS_TABSTOP | BS_OWNERDRAW));
    bundledCatalog = ParseCatalog(ReadJson(bundle / "catalog.json"));
    catalog = bundledCatalog;
    if (fs::exists(privateDirectory / "catalog.json"))
    {
        try { auto cached = ParseCatalog(ReadJson(privateDirectory / "catalog.json")); CheckAdvance(catalog, cached); catalog = std::move(cached); }
        catch (const std::exception& error) { Status(L"保存済み一覧を使用できません: " + Wide(error.what())); }
    }
    std::string preferred = "26.51.1";
    if (fs::exists(privateDirectory / "launcher.json"))
    {
        const auto config = ReadJson(privateDirectory / "launcher.json");
        if (config.value("schemaVersion", 0) == 3)
        {
            sources = config.at("sources");
            if (!sources.is_object()) throw std::runtime_error("保存した本体一覧が不正です。");
            preferred = config.value("selectedVersion", preferred);
        }
        else sources["26.51.1"] = config.value("game", std::string{});
    }
    RefreshVersions(preferred);
    if (!SetTimer(window, 1, 500, nullptr)) throw std::runtime_error("接続確認タイマーを開始できません。");
    PostMessageW(window, CheckMessage, 0, 0);
}

LRESULT CALLBACK WindowProc(HWND handle, UINT message, WPARAM wParam, LPARAM lParam)
{
    try
    {
        switch (message)
        {
        case WM_CREATE: window = handle; BuildUi(); return 0;
        case CheckMessage: if (state == State::Idle) CheckForUpdates(); return 0;
        case UpdatedMessage:
        {
            std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lParam));
            if (preparation.joinable()) preparation.join();
            Busy(State::Idle);
            if (result->error.empty()) catalog = std::move(result->catalog);
            RefreshVersions(selected.target.id);
            Status(result->error.empty() ? L"更新一覧を確認しました。版を選んで起動してください。" :
                L"保存済み一覧を使用します。" + Wide(result->error));
            return 0;
        }
        case PackageMessage:
        {
            std::unique_ptr<PackageResult> result(reinterpret_cast<PackageResult*>(lParam));
            if (preparation.joinable()) preparation.join();
            Busy(State::Idle);
            if (!result->error.empty()) throw std::runtime_error(result->error);
            packageRoot = result->root;
            ApplyClient();
            return 0;
        }
        case PreparedMessage:
        {
            std::unique_ptr<PreparationResult> result(reinterpret_cast<PreparationResult*>(lParam));
            if (preparation.joinable()) preparation.join();
            Busy(State::Idle);
            EndProgress();
            if (!result->error.empty()) throw std::runtime_error(result->error);
            pending = result->instance;
            SetWindowTextW(GetDlgItem(window, Game), (pending.directory / "Minecraft.Windows.exe").c_str());
            Save();
            PrepareRuntimeOrLaunch();
            return 0;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == Version && HIWORD(wParam) == CBN_SELCHANGE && state == State::Idle)
            { Save(); SelectVersion(); Save(); return 0; }
            if (HIWORD(wParam) != BN_CLICKED) return 0;
            switch (LOWORD(wParam))
            {
            case PickGame: if (state == State::Idle) Pick(Game); break;
            case AutoDetect: if (state == State::Idle && FindInstalledGame(false)) Save(); break;
            case Apply: if (state == State::Idle) FetchAndApply(); break;
            case CheckUpdates: if (state == State::Idle) CheckForUpdates(); break;
            case Guide: ShellExecuteW(handle, L"open", (bundle / "README-ja.md").c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
            case Logs:
                ShellExecuteW(handle, L"open", (fs::exists(privateDirectory / "runtime-setup.log") ?
                    privateDirectory / "runtime-setup.log" : privateDirectory / "launcher.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                break;
            }
            return 0;
        case WM_TIMER: Tick(); return 0;
        case WM_DRAWITEM:
        {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item && item->CtlType == ODT_BUTTON) { DrawButton(*item); return TRUE; }
            break;
        }
        case WM_PAINT:
        {
            PAINTSTRUCT paint;
            HDC dc = BeginPaint(handle, &paint);
            const auto fillRect = [&](int l, int t, int r, int b, COLORREF colour) {
                RECT box{Px(l), Px(t), Px(r), Px(b)};
                HBRUSH brush = CreateSolidBrush(colour);
                FillRect(dc, &box, brush);
                DeleteObject(brush);
            };
            fillRect(28, 62, 148, 66, ColAccent);
            fillRect(148, 63, 718, 65, ColBorder);
            HPEN pen = CreatePen(PS_SOLID, 1, ColBorder);
            HGDIOBJ oldPen = SelectObject(dc, pen), oldBrush = SelectObject(dc, panelBrush);
            if (progressPercent >= 0)
            {
                HPEN trackPen = CreatePen(PS_SOLID, 1, ColBorder);
                HBRUSH trackBrush = CreateSolidBrush(ColPanel);
                HGDIOBJ penBefore = SelectObject(dc, trackPen), brushBefore = SelectObject(dc, trackBrush);
                RoundRect(dc, Px(28), Px(426), Px(720), Px(440), Px(8), Px(8));
                SelectObject(dc, brushBefore);
                SelectObject(dc, penBefore);
                DeleteObject(trackBrush);
                DeleteObject(trackPen);
                const int span = Px(720) - Px(28) - 4;
                const int fill = span * progressPercent / 100;
                if (fill > 0)
                {
                    HBRUSH accent = CreateSolidBrush(ColAccent);
                    HGDIOBJ before = SelectObject(dc, accent);
                    SelectObject(dc, GetStockObject(NULL_PEN));
                    RoundRect(dc, Px(28) + 2, Px(426) + 2, Px(28) + 2 + fill, Px(440) - 1, Px(6), Px(6));
                    SelectObject(dc, before);
                    DeleteObject(accent);
                    SelectObject(dc, pen);   // back to the border pen for the status panel
                }
            }
            RoundRect(dc, Px(24), Px(461), Px(724), Px(589), Px(14), Px(14));
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);
            DeleteObject(pen);
            EndPaint(handle, &paint);
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), ColText);
            SetBkColor(reinterpret_cast<HDC>(wParam), ColPanel);
            return reinterpret_cast<LRESULT>(panelBrush);
        case WM_CTLCOLORSTATIC:
        {
            const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
            HDC dc = reinterpret_cast<HDC>(wParam);
            if (id == StatusText || id == Game)  // read-only edit boxes are reported as "static"
            {
                SetTextColor(dc, id == StatusText && statusError ? ColError : ColText);
                SetBkColor(dc, ColPanel);
                return reinterpret_cast<LRESULT>(panelBrush);
            }
            SetTextColor(dc, id == TitleText ? ColAccentHi : id == Caption ? ColMuted : ColText);
            SetBkColor(dc, ColBg);
            return reinterpret_cast<LRESULT>(background);
        }
        case WM_CLOSE:
            if (state == State::Installing || state == State::Preparing || state == State::Checking || state == State::Fetching)
            {
                Status(L"ゲームとローダーの準備が終わるまで、この画面を開いたままにしてください。");
                return 0;
            }
            DestroyWindow(handle);
            return 0;
        case WM_DESTROY:
            KillTimer(handle, 1);
            if (hudProcess) CloseHandle(hudProcess);
            if (gameProcess) CloseHandle(gameProcess);
            PostQuitMessage(0);
            return 0;
        }
    }
    catch (const std::exception& error)
    {
        if (state != State::Installing) Busy(State::Idle);
        Status(L"処理できませんでした:\r\n" + Wide(error.what()) + L"\r\n初回の準備と選択したファイルを確認してください。");
    }
    return DefWindowProcW(handle, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show)
{
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try
    {
        std::vector<wchar_t> path(32768);
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) throw std::runtime_error("配布フォルダーを取得できません。");
        bundle = fs::path(std::wstring(path.data(), length)).parent_path();
        privateDirectory = KnownFolder(FOLDERID_LocalAppData) / "MyClient";
        fs::create_directories(privateDirectory);
        HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\MyClient.Launcher");
        const DWORD mutexError = GetLastError();
        if (!single) throw std::runtime_error("ランチャーの起動状態を確認できません。");
        if (mutexError == ERROR_ALREADY_EXISTS)
        {
            HWND previous = FindWindowW(L"MyClientLauncherWindow", nullptr);
            if (previous) { ShowWindow(previous, SW_RESTORE); SetForegroundWindow(previous); }
            CloseHandle(single);
            CoUninitialize();
            return 0;
        }
        HDC screen = GetDC(nullptr);
        dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        font = CreateFontW(-Px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        heading = CreateFontW(-Px(27), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        smallFont = CreateFontW(-Px(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        boldFont = CreateFontW(-Px(18), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Yu Gothic UI");
        background = CreateSolidBrush(ColBg);
        panelBrush = CreateSolidBrush(ColPanel);
        WNDCLASSW type{};
        type.hInstance = instance;
        type.lpfnWndProc = WindowProc;
        type.lpszClassName = L"MyClientLauncherWindow";
        type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        type.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        type.hbrBackground = background;
        RegisterClassW(&type);
        RECT rect{0, 0, Px(748), Px(656)};
        constexpr DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&rect, style, FALSE);
        HWND handle = CreateWindowW(type.lpszClassName, L"MyClient — 適用・起動", style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, nullptr);
        if (!handle) throw std::runtime_error("ランチャー画面を作成できません。");
        ShowWindow(handle, show);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            if (!IsDialogMessageW(handle, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        DeleteObject(font);
        DeleteObject(heading);
        DeleteObject(background);
        DeleteObject(panelBrush);
        DeleteObject(smallFont);
        DeleteObject(boldFont);
        CloseHandle(single);
    }
    catch (const std::exception& error)
    {
        MessageBoxW(nullptr, Wide(error.what()).c_str(), L"MyClient Launcher", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }
    CoUninitialize();
    return 0;
}
