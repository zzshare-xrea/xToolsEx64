// xToolsEx64.dll - 拖把更名器 (xTools / SmartRename) 64 位资源管理器右键扩展
//
// 原版 xToolsEx.dll 是 32 位 Delphi shell 扩展，64 位资源管理器(explorer.exe)
// 无法加载它，所以 Win10/Win11 x64 上右键菜单不出现。
//
// 本 DLL 是完整的 64 位替代实现：
//   1. 在资源管理器里为 文件 / 文件夹 / 驱动器 / 文件夹空白处 添加“拖把更名器”菜单项；
//   2. 点击后按原版协议交接文件列表：
//        写 %TEMP%\~XTLS50.tmp（每行一个完整路径，CRLF，ANSI/CP_ACP）
//        -> 启动 "xTools.exe" -a -> 等主窗口出现 -> 把窗口带到前台。
//      不再发送 WM_DROPFILES：原程序没有注册 WS_EX_ACCEPTFILES，
//      它在启动时用 RegisterDragDrop 接收拖放，消息投递对它无效。
//      也不做 OLE 拖放兜底：主程序的 IDropTarget 属于它自己的进程，
//      跨进程调用必然访问非法内存，实测会让 Explorer 崩溃。
//   3. 若 %TEMP%\~XTLS50.tmp 被一个同名文件夹占用，先把它改名让位（.bak/.bak2…）；
//      改名不成功就弹提示，把完整路径告诉用户手动处理。
//
// 编译:
//   x86_64-w64-mingw32-g++ -std=c++11 -Os -fno-rtti -ffunction-sections -fdata-sections \
//     -shared -o xToolsEx64.dll xToolsEx64.cpp xToolsEx64.def -Wl,--gc-sections,--strip-all \
//     -lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luser32 -lgdi32 \
//     -static -static-libgcc -static-libstdc++
//   （不要加 -fno-exceptions：代码里有 try/catch，加了就编不过）

#define WINVER 0x0601
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <new>
#include <climits>
#include <utility>   // std::pair（第 491/773 行用到），不能靠 <vector>/<string> 间接引入

// {9E7B4C21-3A55-4F0E-8D1A-2B3C4D5E6F70}
static const GUID CLSID_xToolsEx64 =
    { 0x9E7B4C21, 0x3A55, 0x4F0E, { 0x8D, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F, 0x70 } };

static const wchar_t* CLSID_STR    = L"{9E7B4C21-3A55-4F0E-8D1A-2B3C4D5E6F70}";
static const wchar_t* HANDLER_KEY  = L"xToolsEx64";
static const wchar_t* HANDLER_KEY86 = L"xToolsEx86";
static const wchar_t* CONFIG_KEY   = L"Software\\xTools\\ShellEx64";
static const wchar_t* DEFAULT_MENU = L"拖把更名器";
static const wchar_t* DEFAULT_HELP = L"添加到拖把更名器的文件列表";

static HINSTANCE g_hInst = NULL;
static volatile LONG g_busy = 0;   // >0 时不允许被卸载

// ---------------- 资源管理：异常/提前 return 时也能正确释放 ----------------
class ScopedRegKey
{
public:
    explicit ScopedRegKey(HKEY h = NULL) : m_h(h) {}
    ~ScopedRegKey() { reset(); }
    ScopedRegKey(const ScopedRegKey&) = delete;
    ScopedRegKey& operator=(const ScopedRegKey&) = delete;
    HKEY get() const { return m_h; }
    void reset(HKEY h = NULL)
    {
        if (m_h) RegCloseKey(m_h);
        m_h = h;
    }
private:
    HKEY m_h;
};

class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE h = NULL) : m_h(h) {}
    ~ScopedHandle() { reset(); }
    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;
    HANDLE get() const { return m_h; }
    bool valid() const { return m_h != NULL && m_h != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h = NULL)
    {
        if (valid()) CloseHandle(m_h);
        m_h = h;
    }
private:
    HANDLE m_h;
};

// GetData 成功后接管 STGMEDIUM：所有返回/异常路径都 ReleaseStgMedium。
class ScopedStgMedium
{
public:
    explicit ScopedStgMedium(STGMEDIUM* p) : m_p(p) {}
    ~ScopedStgMedium() { if (m_p) ReleaseStgMedium(m_p); }
    ScopedStgMedium(const ScopedStgMedium&) = delete;
    ScopedStgMedium& operator=(const ScopedStgMedium&) = delete;
private:
    STGMEDIUM* m_p;
};

// 取得互斥量后创建；析构释放所有权。必须先 unlock() 再 CloseHandle，
// 否则持锁时关句柄会把互斥量变成 abandoned。
class ScopedMutexLock
{
public:
    explicit ScopedMutexLock(HANDLE h = NULL) : m_h(h), m_owned(h != NULL) {}
    ~ScopedMutexLock() { unlock(); }
    ScopedMutexLock(const ScopedMutexLock&) = delete;
    ScopedMutexLock& operator=(const ScopedMutexLock&) = delete;
    void unlock()
    {
        if (m_owned) { ReleaseMutex(m_h); m_owned = false; }
    }
private:
    HANDLE m_h;
    bool m_owned;
};

// 列表文件写成功后暂由本线程负责清理；确认进程启动成功再放弃责任。
class ScopedDeleteFile
{
public:
    ScopedDeleteFile(const wchar_t* path, bool active) : m_path(path), m_active(active) {}
    ~ScopedDeleteFile() { cleanup(); }
    ScopedDeleteFile(const ScopedDeleteFile&) = delete;
    ScopedDeleteFile& operator=(const ScopedDeleteFile&) = delete;
    void release() { m_active = false; }        // 交出去，永不删
    // 立刻删掉并交还删除权（幂等）。必须在“释放投递互斥量”之前调用：
    // 否则锁一放开，下一次投递就可能写出同名文件，而被这里的析构删掉。
    //
    // 这里做有限重试是因为杀软/索引器常会短暂占用文件，单次 DeleteFileW 容易失败；
    // 但无论成败都必须清 m_active —— 把删除权留到锁外，等于又给下一次投递埋了误删。
    // 删除彻底失败只会残留一个 tmp 文件，本身无害：下次投递用 CREATE_ALWAYS 直接覆盖。
    void cleanup()
    {
        if (!m_active || !m_path) return;
        for (int i = 0; i < 4; ++i)
        {
            if (DeleteFileW(m_path)) break;
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) break;
            Sleep(20);
        }
        m_active = false;
    }
private:
    const wchar_t* m_path;
    bool m_active;
};

// ---------------------------------------------------------------- 小工具

static std::wstring GetModulePath()
{
    // GetModuleFileNameW 在缓冲不足时会截断；这里动态扩容直到取到完整路径
    // （DLL 装在深层目录时固定 MAX_PATH*2 会截断，导致找不到同目录的 xTools.exe）。
    DWORD cap = MAX_PATH;
    for (;;)
    {
        std::vector<wchar_t> buf((size_t)cap, 0);
        SetLastError(ERROR_SUCCESS);
        DWORD n = GetModuleFileNameW(g_hInst, buf.data(), cap);
        if (n == 0) return std::wstring();
        if (n < cap && GetLastError() != ERROR_INSUFFICIENT_BUFFER)
            return std::wstring(buf.data(), n);
        if (cap >= 32768) return std::wstring(buf.data(), n);
        cap = (cap >= 16384) ? 32768 : cap * 2;
    }
}

static std::wstring GetModuleDir()
{
    std::wstring p = GetModulePath();
    size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(0, pos);
}

static bool FileExistsW(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring ReadRegString(HKEY root, const std::wstring& key, const std::wstring& name)
{
    std::wstring out;
    HKEY rawKey = NULL;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &rawKey) != ERROR_SUCCESS)
        return out;
    ScopedRegKey keyHandle(rawKey);
    HKEY h = keyHandle.get();
    DWORD type = 0, size = 0;
    if (RegQueryValueExW(h, name.c_str(), NULL, &type, NULL, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t))
    {
        std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(h, name.c_str(), NULL, &type, (LPBYTE)buf.data(), &size) == ERROR_SUCCESS)
        {
            out.assign(buf.data());
            if (type == REG_EXPAND_SZ)
            {
                // 展开后可能超过 MAX_PATH*2（长环境变量/长路径），动态取长度
                DWORD need = ExpandEnvironmentStringsW(out.c_str(), NULL, 0);
                if (need == 0)
                    out.clear();
                else
                {
                    std::vector<wchar_t> exp((size_t)need, 0);
                    DWORD written = ExpandEnvironmentStringsW(out.c_str(), exp.data(), need);
                    if (written == 0 || written > need) out.clear();
                    else out.assign(exp.data());
                }
            }
        }
    }
    return out;
}

static std::wstring Config(const wchar_t* name, const wchar_t* def)
{
    std::wstring v = ReadRegString(HKEY_CURRENT_USER, CONFIG_KEY, name);
    if (v.empty()) v = ReadRegString(HKEY_LOCAL_MACHINE, CONFIG_KEY, name);
    if (v.empty() && def) v = def;
    return v;
}

// ------------------------------------------------------- 定位 xTools.exe

static std::wstring ResolveExePath()
{
    std::wstring p = Config(L"ExePath", L"");
    if (!p.empty())
    {
        for (auto& c : p) if (c == L'/') c = L'\\';
        if (FileExistsW(p)) return p;
    }

    std::wstring dir = GetModuleDir();
    std::wstring cand = dir + L"\\xTools.exe";
    if (FileExistsW(cand)) return cand;

    size_t pos = dir.find_last_of(L"\\/");
    if (pos != std::wstring::npos)
    {
        cand = dir.substr(0, pos) + L"\\xTools.exe";
        if (FileExistsW(cand)) return cand;
        cand = dir.substr(0, pos) + L"\\拖把更名器\\xTools.exe";
        if (FileExistsW(cand)) return cand;
    }

    // App Paths
    p = ReadRegString(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\xTools.exe", L"");
    if (!p.empty() && FileExistsW(p)) return p;
    p = ReadRegString(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\xTools.exe", L"");
    if (!p.empty() && FileExistsW(p)) return p;

    // SearchPathW 在缓冲不足时返回所需字符数（含结尾 NUL），据此扩容
    DWORD cap = MAX_PATH;
    for (;;)
    {
        std::vector<wchar_t> found((size_t)cap, 0);
        DWORD n = SearchPathW(NULL, L"xTools.exe", NULL, cap, found.data(), NULL);
        if (n == 0) break;
        if (n < cap) return std::wstring(found.data(), n);
        if (n >= 32768) break;
        cap = n + 1;
    }

    return std::wstring();
}

// ------------------------------------------------------------ 找主窗口

struct FindCtx
{
    const wchar_t* exeName;      // 目标 exe 文件名
    DWORD  pid;                  // 只看某个进程(0 = 不限)
    HWND   best;                 // 可见且带标题的最佳窗口
    HWND   any;                  // 任意顶层窗口
    std::vector<wchar_t> path;   // 进程映像路径缓冲：外层分配一次，回调复用
};

static BOOL CALLBACK FindEnumProc(HWND hwnd, LPARAM lp)
{
    // 异常绝不能越过 Windows 回调边界：EnumWindows 内部没有 C++ 展开帧，
    // 让它逃出去就是未定义行为（实际表现是直接终止进程 = 右键崩 Explorer）。
    try
    {
        FindCtx* ctx = (FindCtx*)lp;

        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (!pid) return TRUE;
        if (ctx->pid && pid != ctx->pid) return TRUE;

        if (!ctx->pid)
        {
            HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!hp) return TRUE;
            // 缓冲由 FindExeWindow 分配一次，这里只重置长度。
            // MAX_PATH * 2 只有 520 字符，长路径会查询失败，所以缓冲给到 NT 的 32768 上限。
            DWORD sz = (DWORD)ctx->path.size();
            BOOL ok = QueryFullProcessImageNameW(hp, 0, ctx->path.data(), &sz);
            CloseHandle(hp);
            if (!ok) return TRUE;   // 进程打不开或查不到映像路径：跳过这个窗口，不猜它是目标

            const wchar_t* imagePath = ctx->path.data();
            const wchar_t* base = imagePath;
            for (const wchar_t* q = imagePath; *q; ++q)
                if (*q == L'\\' || *q == L'/') base = q + 1;
            if (_wcsicmp(base, ctx->exeName) != 0) return TRUE;
        }

        if (!IsWindowVisible(hwnd)) return TRUE;
        if (!ctx->any) ctx->any = hwnd;

        LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
        if ((style & WS_CHILD) == 0 && GetWindowTextLengthW(hwnd) > 0)
        {
            ctx->best = hwnd;
            return FALSE;   // 找到就停
        }
    }
    catch (...)
    {
        return TRUE;        // 单个窗口处理出问题不影响整体枚举
    }
    return TRUE;
}

static HWND FindExeWindow(const wchar_t* exeName, DWORD pidOnly)
{
    FindCtx ctx;
    ctx.exeName = exeName;
    ctx.pid = pidOnly;
    ctx.best = NULL;
    ctx.any = NULL;
    ctx.path.assign(32768, 0);   // 一次分配，回调内复用（不再每个窗口分配 64KB）
    EnumWindows(FindEnumProc, (LPARAM)&ctx);
    return ctx.best ? ctx.best : ctx.any;
}

static HWND WaitExeWindow(DWORD pid, int timeoutMs)
{
    HWND h = NULL;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < (DWORD)timeoutMs)
    {
        h = FindExeWindow(L"xTools.exe", pid);
        if (h) return h;
        Sleep(150);
    }
    return NULL;
}

// ------------------------------------------------- 把主窗口带到前台

// 不再发送 WM_DROPFILES：原程序启动时用 RegisterDragDrop 接收拖放，
// 它没有注册 WS_EX_ACCEPTFILES，消息投递对它无效（OLE 拖放兜底也已否决）。
// 但用户点完菜单必须看到窗口，否则会以为没反应。
static void ActivateWindow(HWND hwnd)
{
    if (!hwnd) return;
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

static BOOL LaunchExe(const std::wstring& exe, const std::wstring& args, DWORD* outPid)
{
    std::wstring cmd = L"\"" + exe + L"\"";
    if (!args.empty()) cmd += L" " + args;

    // 工作目录设为程序所在目录：绿色版要读同目录下的 Language\*.ini，
    // 继承 Explorer 的工作目录（多为 System32）可能读不到语言文件。
    std::wstring workDir = exe;
    size_t slash = workDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) workDir.resize(slash); else workDir.clear();

    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));

    // CreateProcessW 会就地改写命令行，不能传只读的 c_str()
    BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], NULL, NULL, FALSE,
                             0, NULL, workDir.empty() ? NULL : workDir.c_str(), &si, &pi);
    if (ok)
    {
        if (outPid) *outPid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    return ok;
}

// ------------------------------------------------------------ 主逻辑

// 分块写，避免一次 WriteFile 的 4GB 限制与大缓冲失败
static bool WriteAll(HANDLE h, const char* data, size_t bytes)
{
    while (bytes)
    {
        DWORD chunk = (DWORD)(bytes > 0x7ffff000u ? 0x7ffff000u : bytes);
        DWORD written = 0;
        if (!WriteFile(h, data, chunk, &written, NULL) || written == 0)
            return false;
        data  += written;
        bytes -= written;
    }
    return true;
}

// 保证 listfile 这个路径能被当成“文件”来写。
// 典型故障：%TEMP% 下存在同名【文件夹】~XTLS50.tmp，CreateFileW(CREATE_ALWAYS)
// 打开目录必然失败，表现就是“写入 xTools 文件列表失败”。
// 处理：目录 -> 改名让位（同卷 rename，不复制内容）；只读文件 -> 清掉只读位。
static bool PrepareListFilePath(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;        // 不存在，CREATE_ALWAYS 会新建

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        if (attr & FILE_ATTRIBUTE_READONLY)
        {
            // 只读文件 CREATE_ALWAYS 必然失败；清不掉就直接认输，
            // 让上层立刻给出“写入失败 + 目标路径”的提示，而不是失败在更远的地方。
            if (!SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY))
                return false;
        }
        return true;                                         // 普通文件，会被覆盖
    }

    // 是目录：先清只读位（只读目录 MoveFileW 会失败），再改名让位
    if (attr & FILE_ATTRIBUTE_READONLY)
    {
        if (!SetFileAttributesW(path.c_str(), attr & ~FILE_ATTRIBUTE_READONLY))
            return false;
    }

    for (int i = 0; i < 10; ++i)
    {
        // 不用 std::to_wstring：它会拉进一大段 libstdc++ 数字格式化代码（约 50KB）
        wchar_t suffix[16];
        if (i == 0) lstrcpyW(suffix, L".bak");
        else wsprintfW(suffix, L".bak%d", i + 1);
        std::wstring bak = path + suffix;
        if (GetFileAttributesW(bak.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (MoveFileW(path.c_str(), bak.c_str())) return true;
    }
    return false;
}

static bool WriteListFile(const std::wstring& path, const std::vector<std::wstring>& files)
{
    if (!PrepareListFilePath(path)) return false;

    ScopedHandle h(CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL));
    if (!h.valid()) return false;

    bool ok = true;
    try
    {
        for (size_t i = 0; i < files.size(); ++i)
        {
            // 先算长度再分配：原来固定 char[MAX_PATH*4] 会把长路径截断成错路径
            if (files[i].size() > (size_t)INT_MAX) { ok = false; break; }
            int need = WideCharToMultiByte(CP_ACP, 0, files[i].c_str(), (int)files[i].size(),
                                           NULL, 0, NULL, NULL);
            if (need <= 0) { ok = false; break; }
            std::string line((size_t)need, '\0');
            if (!WideCharToMultiByte(CP_ACP, 0, files[i].c_str(), (int)files[i].size(),
                                     &line[0], need, NULL, NULL)) { ok = false; break; }
            line += "\r\n";
            if (!WriteAll(h.get(), line.data(), line.size())) { ok = false; break; }
        }
        if (ok && !FlushFileBuffers(h.get())) ok = false;   // 确保 -a 启动时读到完整列表
    }
    catch (...)
    {
        h.reset();                 // 先关闭文件，否则 DeleteFileW 会因文件被占用失败
        DeleteFileW(path.c_str());
        return false;              // 保持 bool 语义，错误提示仍由调用方给
    }

    h.reset();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

static void HandoffFiles(std::vector<std::wstring> files, std::wstring folder)
{
    if (files.empty() && !folder.empty())
        files.push_back(folder);
    if (files.empty()) return;

    std::wstring exe = ResolveExePath();
    if (exe.empty())
    {
        MessageBoxW(NULL,
            L"没有找到 xTools.exe。\r\n\r\n请把 xToolsEx64.dll 放到「拖把更名器」程序目录，\r\n"
            L"或运行安装脚本时指定程序所在文件夹。",
            L"拖把更名器 - 64 位右键扩展", MB_ICONWARNING);
        return;
    }

    // 与原版 DLL 一致的投递方式：
    //   1) 把选中的路径按 CRLF 写进 %TEMP%\~XTLS50.tmp
    //   2) 用 "xTools.exe -a" 启动主程序，主程序 -a 时读取该列表并加入文件列表
    // %TEMP% 可能被重定向到长路径，所以不用 MAX_PATH 固定缓冲。
    std::vector<wchar_t> tmpbuf(32768, 0);
    DWORD tn = GetTempPathW((DWORD)tmpbuf.size(), tmpbuf.data());
    if (tn == 0 || tn >= tmpbuf.size())
    {
        MessageBoxW(NULL, L"无法取得 Windows 临时目录路径。", L"拖把更名器", MB_ICONERROR);
        return;
    }
    std::wstring listfile(tmpbuf.data(), tn);
    if (!listfile.empty() &&
        listfile[listfile.size() - 1] != L'\\' && listfile[listfile.size() - 1] != L'/')
        listfile += L'\\';
    listfile += L"~XTLS50.tmp";

    // 固定文件名无法并存，用命名互斥量把“写列表 + 启动 + 等窗口”串行化，
    // 避免两次连续投递互相覆盖导致文件串单。
    ScopedHandle mutex(CreateMutexW(NULL, FALSE, L"Local\\xToolsEx64_Handoff"));
    if (!mutex.valid())
    {
        MessageBoxW(NULL, L"无法创建投递互斥量，未执行本次投递。",
                    L"拖把更名器", MB_ICONERROR);
        return;
    }
    DWORD wait = WaitForSingleObject(mutex.get(), 30000);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
    {
        mutex.reset();
        MessageBoxW(NULL, L"等待上一次投递超时或失败，请稍后再试。",
                    L"拖把更名器", MB_ICONWARNING);
        return;
    }
    ScopedMutexLock mutexLock(mutex.get());

    bool wrote = WriteListFile(listfile, files);
    ScopedDeleteFile cleanupList(listfile.c_str(), wrote);  // wrote 成功后暂由本线程负责删
    DWORD pid = 0;
    bool launched = wrote && LaunchExe(exe, L"-a", &pid);
    // 启动失败就必须在锁内立刻删掉本次的临时文件：等退出函数再靠析构删的话，
    // 锁早已放开，下一次投递可能已经把同名文件重写好了，会被这里误删。
    if (launched) cleanupList.release();                    // 已交给主程序，不再删
    else cleanupList.cleanup();                             // 没交出去，立刻删

    // 锁内只等到“主窗口出现”，确认这一次投递已经真正开始；置前放到锁外，
    // 避免长时间占锁阻塞下一次投递。
    HWND hwnd = NULL;
    if (launched)
    {
        hwnd = pid ? WaitExeWindow(pid, 5000) : NULL;
        if (!hwnd) hwnd = FindExeWindow(L"xTools.exe", 0);
    }

    mutexLock.unlock();                // 先释放所有权，再关闭句柄
    mutex.reset();

    if (!wrote || !launched)
    {
        std::wstring msg;
        if (!wrote)
        {
            DWORD attr = GetFileAttributesW(listfile.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                msg = L"临时目录下的 ~XTLS50.tmp 是一个文件夹，无法改名让位。\r\n\r\n"
                      L"请手动删除或移走这个文件夹后重试：\r\n" + listfile;
            else
                msg = L"写入 xTools 文件列表失败。\r\n\r\n目标文件：\r\n" + listfile;
        }
        else
        {
            msg = L"无法启动 xTools.exe。";
        }
        MessageBoxW(NULL, msg.c_str(), L"拖把更名器", MB_ICONERROR);
        return;
    }

    // 不做拖放兜底：主程序的 IDropTarget 是它自己进程里的指针，跨进程调用必然崩溃。
    // 这里只把窗口带到前台，否则用户会以为点了没反应。
    if (hwnd) ActivateWindow(hwnd);
}

static DWORD WINAPI HandoffThread(LPVOID param)
{
    // 参数由本线程拥有：无论交接过程是否抛出异常，都必须释放参数并归还 busy 计数，
    // 否则异常越过线程入口会让进程直接终止，且 g_busy 永远 > 0。
    std::pair<std::vector<std::wstring>, std::wstring>* p =
        (std::pair<std::vector<std::wstring>, std::wstring>*)param;
    try
    {
        if (p) HandoffFiles(p->first, p->second);
    }
    catch (...)
    {
        MessageBoxW(NULL, L"处理文件列表时发生内存或内部错误。",
                    L"拖把更名器", MB_ICONERROR);
    }
    delete p;
    InterlockedDecrement(&g_busy);
    return 0;
}

// ------------------------------------------------------------ COM 对象


// ------------------------------------------------------------------ 菜单图标
// 把 HICON 画成 32bpp DIB（top-down），并做 alpha 预乘，供 MIIM_BITMAP 使用。
// 菜单项图标只能是 HBITMAP（不能直接给 HICON），且必须是 32bpp 带 alpha。
static HBITMAP IconToMenuBitmap(HICON hIcon, int cx, int cy)
{
    if (!hIcon || cx <= 0 || cy <= 0) return NULL;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = cx;
    bmi.bmiHeader.biHeight      = -cy;          // 负数 = 自上而下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP hbmp = CreateDIBSection(NULL, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbmp || !bits) { if (hbmp) DeleteObject(hbmp); return NULL; }
    ZeroMemory(bits, (size_t)cx * cy * 4);

    // 下列失败都发生在位图“从未交给 Shell”的阶段，可以安全 DeleteObject；
    // 返回 NULL 让菜单退化为无图标，比返回一张 alpha 全 0 的空白位图更好。
    HDC hdc = CreateCompatibleDC(NULL);
    if (!hdc)
    {
        DeleteObject(hbmp);
        return NULL;
    }

    HGDIOBJ old = SelectObject(hdc, hbmp);
    if (!old || old == HGDI_ERROR)
    {
        DeleteDC(hdc);
        DeleteObject(hbmp);
        return NULL;
    }

    BOOL drawn = DrawIconEx(hdc, 0, 0, hIcon, cx, cy, 0, NULL, DI_NORMAL);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    if (!drawn)
    {
        DeleteObject(hbmp);
        return NULL;
    }

    // 老式 24bpp（无 alpha 通道）图标在某些系统/GDI 实现下，DrawIconEx(DI_NORMAL)
    // 写出的 alpha 通道全是 0；按预乘处理后整张图变全透明，菜单上就是“没有图标”。
    // 检测到整张图 alpha 全 0 时，用 DI_MASK 取遮罩重建透明度：
    // mask 白 = 该像素透明，mask 黑 = 不透明。
    DWORD* px = (DWORD*)bits;
    size_t pixelCount = (size_t)cx * (size_t)cy;

    bool anyAlpha = false;
    for (size_t i = 0; i < pixelCount; ++i)
        if (((px[i] >> 24) & 0xFFu) != 0) { anyAlpha = true; break; }

    if (!anyAlpha)
    {
        void* maskBits = NULL;
        BITMAPINFO mbmi = bmi;
        HBITMAP hbmpMask = CreateDIBSection(NULL, &mbmi, DIB_RGB_COLORS, &maskBits, NULL, 0);
        if (hbmpMask && maskBits)
        {
            ZeroMemory(maskBits, pixelCount * 4);
            HDC hdcMask = CreateCompatibleDC(NULL);
            if (hdcMask)
            {
                HGDIOBJ oldMask = SelectObject(hdcMask, hbmpMask);
                if (oldMask && oldMask != HGDI_ERROR)
                {
                    DrawIconEx(hdcMask, 0, 0, hIcon, cx, cy, 0, NULL, DI_MASK);
                    DWORD* mp = (DWORD*)maskBits;
                    for (size_t i = 0; i < pixelCount; ++i)
                    {
                        DWORD m = mp[i] & 0x00FFFFFFu;
                        px[i] = (m != 0) ? 0 : (0xFFu << 24) | (px[i] & 0x00FFFFFFu);
                    }
                    SelectObject(hdcMask, oldMask);
                }
                DeleteDC(hdcMask);
            }
            DeleteObject(hbmpMask);
        }
    }

    // DrawIconEx 写入的是“非预乘”ARGB，而菜单绘制按预乘处理，这里补上预乘
    for (size_t i = 0; i < pixelCount; ++i)
    {
        DWORD v = px[i];
        DWORD a = (v >> 24) & 0xFF;
        if (a == 0) { px[i] = 0; continue; }
        if (a != 255)
        {
            DWORD r = ((v >> 16) & 0xFF) * a / 255;
            DWORD g = ((v >>  8) & 0xFF) * a / 255;
            DWORD b = ( v        & 0xFF) * a / 255;
            px[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    return hbmp;
}

// 每次都新建一张位图交给菜单，句柄由 XMenu 对象持有并在析构时释放。
// 诊断开关 IconFree=0 可退回旧的“永不删除”策略：若现场仍出现右键崩溃，
// 先把它设为 0 以排除“删除时机早于菜单销毁”这一类问题。
static bool FreeMenuBitmap()
{
    return Config(L"IconFree", L"1") != L"0";
}

static HBITMAP GetMenuBitmap()
{
    if (Config(L"Icon", L"1") == L"0") return NULL;   // 诊断开关：Icon=0 时不加图标

    std::wstring exe = ResolveExePath();
    if (exe.empty()) return NULL;

    HICON hSmall = NULL, hLarge = NULL;
    // 只用 ExtractIconEx，不调 SHGetFileInfo：后者会走 shell，在 QueryContextMenu
    // 阶段调用有重入风险（Explorer 死在 shell 锁上）。
    //
    // 大/小图标都取：原版 xTools.exe 的图标组只有一张 32x32（没有 16x16），
    // 只请求小图标时，部分系统/配置下 phiconSmall 会返回 NULL，结果就是“没图标”。
    // 谁拿到就用谁，后面 DrawIconEx 会缩放到菜单的 cx/cy。
    UINT nIcons = ExtractIconExW(exe.c_str(), 0, &hLarge, &hSmall, 1);
    HICON hIcon = hSmall ? hSmall : hLarge;
    if (nIcons == 0 || !hIcon)
    {
        if (hSmall) DestroyIcon(hSmall);
        if (hLarge) DestroyIcon(hLarge);
        return NULL;
    }

    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    if (cx <= 0) cx = 16;
    if (cy <= 0) cy = 16;

    HBITMAP bmp = IconToMenuBitmap(hIcon, cx, cy);
    if (hSmall) DestroyIcon(hSmall);
    if (hLarge) DestroyIcon(hLarge);
    return bmp;
}

class XMenu : public IShellExtInit, public IContextMenu
{
public:
    XMenu() : m_ref(1) {}

    ~XMenu()
    {
        // 析构里不做注册表 I/O、不做内存分配：C++11 起析构默认 noexcept，
        // 一旦抛出就是 std::terminate（表现为 Explorer 直接消失，catch 也接不住）。
        // 这里只释放已登记为“自有”的位图。
        for (size_t i = 0; i < m_menuBitmaps.size(); ++i)
            if (m_menuBitmaps[i]) DeleteObject(m_menuBitmaps[i]);
    }

    // ---- IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IContextMenu))
            *ppv = static_cast<IContextMenu*>(this);
        else if (IsEqualIID(riid, IID_IShellExtInit))
            *ppv = static_cast<IShellExtInit*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef()  { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    // ---- IShellExtInit
    STDMETHODIMP Initialize(LPCITEMIDLIST pidlFolder, IDataObject* pdtobj, HKEY /*hkeyProgID*/)
    {
        // C++ 异常一旦越过 COM 方法边界，Explorer 会直接终止；这里统一兜住。
        try
        {
        m_files.clear();
        m_folder.clear();          // 对象被复用时，别把上一次的文件夹带进来

        // pidlFolder 只是“被选中项所在的文件夹”。有 IDataObject 时它是父目录，
        // 不能当成本次投递的目标；只有背景菜单（拿不到选中项）才用它兜底。
        std::wstring folderFromPidl;
        if (pidlFolder)
        {
            // SHGetNameFromIDList 支持长路径与 UNC；SHGetPathFromIDListW 对
            // 超过 MAX_PATH 或非文件系统 pidl 会失败，且无法表达网络路径。
            PWSTR pszPath = NULL;
            if (SUCCEEDED(SHGetNameFromIDList(pidlFolder, SIGDN_FILESYSPATH, &pszPath)) && pszPath)
            {
                try { folderFromPidl.assign(pszPath); }
                catch (...) { CoTaskMemFree(pszPath); throw; }
                CoTaskMemFree(pszPath);
            }
        }

        if (!pdtobj)
        {
            m_folder.swap(folderFromPidl);   // 纯背景菜单：pidlFolder 就是目标目录
            return S_OK;
        }

        FORMATETC fe = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg;
        ZeroMemory(&stg, sizeof(stg));

        if (SUCCEEDED(pdtobj->GetData(&fe, &stg)))
        {
            ScopedStgMedium medium(&stg);    // 任何返回/异常路径都会 ReleaseStgMedium
            // GetData 成功不代表返回的 medium 就是请求的 TYMED_HGLOBAL。
            // 若拿到别的 tymed，stg.hGlobal 不是 HDROP，强转后交给 DragQueryFileW 会崩。
            if (stg.tymed != TYMED_HGLOBAL || !stg.hGlobal)
                return DV_E_TYMED;
            // CF_HDROP 的 hGlobal 本身就是 HDROP 句柄，不能 GlobalLock 后把
            // 指针强转成 HDROP 再用（真实 Explorer 里等于传非法句柄）。
            HDROP hdrop = (HDROP)stg.hGlobal;
            UINT n = DragQueryFileW(hdrop, 0xFFFFFFFF, NULL, 0);
            for (UINT i = 0; i < n; ++i)
            {
                // 先取长度再分配，避免 MAX_PATH*2 固定缓冲截断长路径
                UINT cch = DragQueryFileW(hdrop, i, NULL, 0);
                if (!cch) continue;
                std::vector<wchar_t> buf((size_t)cch + 1, 0);
                if (DragQueryFileW(hdrop, i, buf.data(), cch + 1))
                    m_files.push_back(std::wstring(buf.data()));
            }
        }

        // 选中对象菜单却拿不到 CF_HDROP 时退回 pidlFolder 兜底，
        // 保证“空白处右键”不会因为 CF_HDROP 不可用而丢掉菜单；
        // 但必须确认 pidlFolder 真解析出了路径，否则不投递空路径。
        if (m_files.empty() && !folderFromPidl.empty())
            m_folder.swap(folderFromPidl);
        return S_OK;
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }

    // ---- IContextMenu
    STDMETHODIMP QueryContextMenu(HMENU hMenu, UINT indexMenu, UINT idCmdFirst,
                                 UINT idCmdLast, UINT uFlags)
    {
        try
        {
        if (!hMenu) return E_INVALIDARG;
        if (uFlags & CMF_DEFAULTONLY) return S_OK;
        if (idCmdFirst > idCmdLast) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        if (m_files.empty() && m_folder.empty()) return S_OK;

        std::wstring text = Config(L"MenuText", DEFAULT_MENU);

        MENUITEMINFOW mii;
        ZeroMemory(&mii, sizeof(mii));
        mii.cbSize     = sizeof(mii);
        // IconFree=0 仅作诊断：此时位图不登记、不在析构时释放（回到“永不删除”）。
        // 判断放在这里而不是析构里，避免析构函数做注册表 I/O 或分配内存。
        bool ownBitmap = FreeMenuBitmap();
        HBITMAP hbmp = GetMenuBitmap();
        if (hbmp && ownBitmap)
        {
            try { m_menuBitmaps.push_back(hbmp); }
            catch (...) { DeleteObject(hbmp); return E_OUTOFMEMORY; }
        }
        mii.fMask      = MIIM_ID | MIIM_STRING | MIIM_STATE;
        mii.wID        = idCmdFirst + IDM_ADD;
        mii.dwTypeData = const_cast<LPWSTR>(text.c_str());
        mii.fState     = MFS_ENABLED;
        if (hbmp)                        // 菜单左侧显示 xTools.exe 的图标
        {
            mii.fMask |= MIIM_BITMAP;
            mii.hbmpItem = hbmp;
        }
        if (!InsertMenuItemW(hMenu, indexMenu, TRUE, &mii))
        {
            DWORD err = GetLastError();   // err=0 时 HRESULT_FROM_WIN32 会变成 S_OK
            if (hbmp)
            {
                if (ownBitmap) m_menuBitmaps.pop_back();
                DeleteObject(hbmp);       // 菜单项没插进去，这张位图不会被使用
            }
            return HRESULT_FROM_WIN32(err ? err : ERROR_GEN_FAILURE);
        }

        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }

    STDMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO pici)
    {
        try
        {
        if (!pici) return E_INVALIDARG;

        bool mine = false;
        if (HIWORD(pici->lpVerb) == 0)
            mine = (LOWORD(pici->lpVerb) == IDM_ADD);
        else
            mine = (lstrcmpiA(pici->lpVerb, "xToolsAdd") == 0);

        if (!mine) return E_FAIL;

        // nothrow 只覆盖 pair 自身的内存分配；pair 内 vector/string 的复制仍可能抛异常
        std::pair<std::vector<std::wstring>, std::wstring>* p = NULL;
        try
        {
            p = new (std::nothrow) std::pair<std::vector<std::wstring>, std::wstring>(m_files, m_folder);
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
        if (!p) return E_OUTOFMEMORY;
        InterlockedIncrement(&g_busy);
        HANDLE th = CreateThread(NULL, 0, HandoffThread, p, 0, NULL);
        if (!th)
        {
            DWORD err = GetLastError();
            delete p;
            InterlockedDecrement(&g_busy);
            return HRESULT_FROM_WIN32(err ? err : ERROR_NOT_ENOUGH_MEMORY);
        }
        CloseHandle(th);
        return S_OK;
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }

    STDMETHODIMP GetCommandString(UINT_PTR idCmd, UINT uFlags, UINT* /*reserved*/,
                                  LPSTR pszName, UINT cchMax)
    {
        try
        {
        if (idCmd != IDM_ADD || !pszName) return E_INVALIDARG;

        // GCS_VERBA/VERBW 与 GCS_HELPTEXTA/HELPTEXTW 的低 2 位区分类型，
        // bit2 (GCS_UNICODE) 表示调用方要宽字符
        UINT  base  = uFlags & 0x3;
        bool  wide  = (uFlags & 0x4) != 0;

        if (base == 0)                        // 动词名
        {
            if (wide)
                lstrcpynW((wchar_t*)pszName, L"xToolsAdd", (int)cchMax);   // cchMax 单位是字符
            else
                lstrcpynA(pszName, "xToolsAdd", cchMax);
            return S_OK;
        }
        if (base == 1)                        // 状态栏帮助文字
        {
            std::wstring help = Config(L"HelpText", DEFAULT_HELP);
            if (wide)
                lstrcpynW((wchar_t*)pszName, help.c_str(), (int)cchMax);   // cchMax 单位是字符
            else if (!WideCharToMultiByte(CP_ACP, 0, help.c_str(), -1,
                                          pszName, (int)cchMax, NULL, NULL))
            {
                DWORD err = GetLastError();
                return HRESULT_FROM_WIN32(err ? err : ERROR_INSUFFICIENT_BUFFER);
            }
            return S_OK;
        }
        return S_OK;                          // GCS_VALIDATE：表示支持该命令
        }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        catch (...) { return E_FAIL; }
    }

private:
    static const UINT IDM_ADD = 0;

    LONG m_ref;
    std::vector<std::wstring> m_files;
    std::wstring m_folder;
    std::vector<HBITMAP> m_menuBitmaps;  // 本对象创建并持有，析构时逐一释放
};

// --------------------------------------------------------- ClassFactory

class XFactory : public IClassFactory
{
public:
    XFactory() : m_ref(1) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv)
    {
        if (!ppv) return E_POINTER;
        *ppv = NULL;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IClassFactory))
        {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef()  { return (ULONG)InterlockedIncrement(&m_ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG n = InterlockedDecrement(&m_ref);
        if (n == 0) delete this;
        return (ULONG)n;
    }

    STDMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv)
    {
        if (outer) return CLASS_E_NOAGGREGATION;
        XMenu* m = new (std::nothrow) XMenu();
        if (!m) return E_OUTOFMEMORY;
        HRESULT hr = m->QueryInterface(riid, ppv);
        m->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL lock)
    {
        if (lock) InterlockedIncrement(&g_busy);
        else      InterlockedDecrement(&g_busy);
        return S_OK;
    }

private:
    LONG m_ref;
};

// ------------------------------------------------------------ 注册表

static void SetValue(HKEY root, const std::wstring& sub, const std::wstring& name,
                     const std::wstring& val, DWORD type = REG_SZ)
{
    HKEY rawKey = NULL;
    if (RegCreateKeyExW(root, sub.c_str(), 0, NULL, 0, KEY_WRITE, NULL, &rawKey, NULL) != ERROR_SUCCESS)
        return;
    ScopedRegKey key(rawKey);
    RegSetValueExW(key.get(), name.c_str(), 0, type, (const BYTE*)val.c_str(),
                   (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

// TrySet* 返回 Win32 错误码（LONG），便于区分 ERROR_ACCESS_DENIED 和其它失败。
// view 传 KEY_WOW64_64KEY / KEY_WOW64_32KEY 让系统按视图重定向，不手拼 "Wow6432Node"。
static LONG TrySetKeyDefault(HKEY root, const std::wstring& sub, const std::wstring& val,
                             REGSAM view = 0)
{
    HKEY rawKey = NULL;
    LONG r = RegCreateKeyExW(root, sub.c_str(), 0, NULL, 0, KEY_WRITE | view, NULL, &rawKey, NULL);
    if (r != ERROR_SUCCESS) return r;
    ScopedRegKey key(rawKey);
    return RegSetValueExW(key.get(), NULL, 0, REG_SZ, (const BYTE*)val.c_str(),
                          (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

static LONG TrySetValue(HKEY root, const std::wstring& sub, const std::wstring& name,
                        const std::wstring& val, REGSAM view = 0)
{
    HKEY rawKey = NULL;
    LONG r = RegCreateKeyExW(root, sub.c_str(), 0, NULL, 0, KEY_WRITE | view, NULL, &rawKey, NULL);
    if (r != ERROR_SUCCESS) return r;
    ScopedRegKey key(rawKey);
    return RegSetValueExW(key.get(), name.c_str(), 0, REG_SZ, (const BYTE*)val.c_str(),
                          (DWORD)((val.size() + 1) * sizeof(wchar_t)));
}

// 注册前对被覆盖值的快照：记录它是否存在、类型与原始字节。
// 失败时据此恢复，做到“回到注册前的状态”，而不是把整套注册删掉。
struct RegistryValueBackup
{
    std::wstring path;
    std::wstring name;
    bool keyExisted;   // 备份时这个键本身是否已存在。false = 整个键都是本次注册建出来的，
                       // 回滚收尾时删掉它（只删叶子键，不递归父级；RegDeleteKey 只看子键，不看值）
    bool existed;
    DWORD type;
    std::vector<BYTE> data;
};

static LONG BackupRegistryValue(HKEY root, const std::wstring& path, const std::wstring& name,
                                REGSAM view, RegistryValueBackup& backup)
{
    backup.path = path;
    backup.name = name;
    backup.keyExisted = false;
    backup.existed = false;
    backup.type = REG_NONE;
    backup.data.clear();

    HKEY rawKey = NULL;
    LONG r = RegOpenKeyExW(root, path.c_str(), 0, KEY_QUERY_VALUE | view, &rawKey);
    if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (r != ERROR_SUCCESS) return r;
    backup.keyExisted = true;
    ScopedRegKey key(rawKey);

    DWORD type = 0, size = 0;
    const wchar_t* valueName = name.empty() ? NULL : name.c_str();
    r = RegQueryValueExW(key.get(), valueName, NULL, &type, NULL, &size);
    if (r == ERROR_FILE_NOT_FOUND) return ERROR_SUCCESS;   // 值不存在：回滚时删掉即可
    if (r != ERROR_SUCCESS && r != ERROR_MORE_DATA) return r;

    backup.data.resize(size);
    r = RegQueryValueExW(key.get(), valueName, NULL, &type,
                         backup.data.empty() ? NULL : backup.data.data(), &size);
    if (r != ERROR_SUCCESS) return r;
    backup.data.resize(size);
    backup.type = type;
    backup.existed = true;
    return ERROR_SUCCESS;
}

static LONG RestoreRegistryValue(HKEY root, const RegistryValueBackup& backup, REGSAM view)
{
    // 回滚阶段只打开已存在的键，绝不用 RegCreateKeyExW 凭空创建一条路径。
    HKEY rawKey = NULL;
    LONG r = RegOpenKeyExW(root, backup.path.c_str(), 0, KEY_SET_VALUE | view, &rawKey);
    if (r != ERROR_SUCCESS)
    {
        // 键不存在有两种返回值：整条路径的中间层缺失给 PATH_NOT_FOUND，
        // 只有叶子层缺失给 FILE_NOT_FOUND。只要注册前这个键就不存在，
        // 说明已经回到原状，算成功——用 keyExisted 而不是 existed，
        // 它记录的是“整个键原本是否存在”，语义更准，也和备份侧的处理对称。
        // 原本存在的键现在却打不开，那就是真失败，不能假装恢复成功。
        if (!backup.keyExisted &&
            (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND))
            return ERROR_SUCCESS;
        return r;
    }
    ScopedRegKey key(rawKey);
    const wchar_t* valueName = backup.name.empty() ? NULL : backup.name.c_str();

    if (backup.existed)
        r = RegSetValueExW(key.get(), valueName, 0, backup.type,
                           backup.data.empty() ? NULL : backup.data.data(),
                           (DWORD)backup.data.size());
    else
    {
        r = RegDeleteValueW(key.get(), valueName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }

    // 删键不在这里做：同一个键常常有好几个备份值（默认值 + ThreadingModel），
    // 先删键会让后面的值无处可写。统一交给 RestoreRegistryValues 收尾。
    return r;
}
static LONG BackupAndPush(HKEY root, const std::wstring& path, const std::wstring& name,
                          REGSAM view, std::vector<RegistryValueBackup>& backups)
{
    RegistryValueBackup b;
    LONG r = BackupRegistryValue(root, path, name, view, b);
    if (r == ERROR_SUCCESS) backups.push_back(b);
    return r;
}

// 逆序恢复，返回第一个错误。
static LONG RestoreRegistryValues(HKEY root, const std::vector<RegistryValueBackup>& backups,
                                  size_t begin, size_t end, REGSAM view)
{
    LONG firstError = ERROR_SUCCESS;
    for (size_t i = end; i > begin; --i)
    {
        LONG r = RestoreRegistryValue(root, backups[i - 1], view);
        if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    }

    // 所有值恢复完，才清理“本次注册新建、原本不存在的键”。
    // RegDeleteKey 只要求无子键，不看键里还有没有值，所以必须等同一个键的
    // 全部备份项都处理完再删，并且该键下不能有任何一项是原本存在的。
    for (size_t i = end; i > begin; --i)
    {
        const RegistryValueBackup& b = backups[i - 1];
        if (b.keyExisted) continue;                 // 原本就存在的键，绝不能删

        bool rebuiltByUs = true;
        for (size_t j = begin; j < end; ++j)
        {
            if (backups[j].path != b.path) continue;
            if (backups[j].keyExisted || backups[j].existed) { rebuiltByUs = false; break; }
        }
        if (!rebuiltByUs) continue;

        bool seenLater = false;                     // 同一个 path 只删一次
        for (size_t j = i; j < end; ++j)
            if (backups[j].path == b.path) { seenLater = true; break; }
        if (seenLater) continue;

        LONG delErr = view ? RegDeleteKeyExW(root, b.path.c_str(), view, 0)
                           : RegDeleteKeyW(root, b.path.c_str());
        if (delErr != ERROR_SUCCESS &&
            delErr != ERROR_FILE_NOT_FOUND &&
            delErr != ERROR_PATH_NOT_FOUND &&
            delErr != ERROR_DIR_NOT_EMPTY &&
            firstError == ERROR_SUCCESS)
            firstError = delErr;
    }
    return firstError;
}

static const wchar_t* const g_regTargets[] = {
    L"*", L"Directory", L"Folder", L"Drive", L"Directory\\Background"
};

// 删除本 DLL 自己的一套注册（5 个 handler 键 + CLSID 键），用于失败回滚与注销。
// 键名用栈上固定缓冲拼接：回滚路径本身不分配内存，避免“回滚时又 OOM”。
static LONG DeleteRegistrationSet(HKEY root, bool wow32)
{
    // 用 WOW64 视图标志定位，不手拼 "Wow6432Node" 前缀：先打开 Software\Classes，
    // 再对相对子路径删树，避免绝对路径与视图标志混用对不上。
    const REGSAM view = wow32 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
    HKEY rawClasses = NULL;
    // 枚举/查询权限也必要：RegDeleteTreeW 需要能枚举子键
    LONG r = RegOpenKeyExW(root, L"Software\\Classes", 0,
                           KEY_WRITE | DELETE | KEY_ENUMERATE_SUB_KEYS |
                           KEY_QUERY_VALUE | view, &rawClasses);
    if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (r != ERROR_SUCCESS) return r;
    ScopedRegKey classesKey(rawClasses);

    LONG firstError = ERROR_SUCCESS;
    const wchar_t* handler = wow32 ? HANDLER_KEY86 : HANDLER_KEY;

    for (size_t i = 0; i < ARRAYSIZE(g_regTargets); ++i)
    {
        wchar_t key[512];
        lstrcpyW(key, g_regTargets[i]);
        lstrcatW(key, L"\\shellex\\ContextMenuHandlers\\");
        lstrcatW(key, handler);
        r = RegDeleteTreeW(classesKey.get(), key);
        if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) r = ERROR_SUCCESS;
        if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    }

    wchar_t clsidKey[256];
    lstrcpyW(clsidKey, L"CLSID\\");
    lstrcatW(clsidKey, CLSID_STR);
    r = RegDeleteTreeW(classesKey.get(), clsidKey);
    if (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND) r = ERROR_SUCCESS;
    if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    return firstError;
}

// 注册过程中的非致命问题：调试器可见，同时记到配置键的 LastWarn（install 脚本可读）。
static void ReportWarning(const std::wstring& text)
{
    OutputDebugStringW(text.c_str());
    OutputDebugStringW(L"\n");
    try { SetValue(HKEY_CURRENT_USER, CONFIG_KEY, L"LastWarn", text); } catch (...) {}
}

static void ClearWarning()
{
    HKEY rawKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, CONFIG_KEY, 0, KEY_SET_VALUE, &rawKey) != ERROR_SUCCESS)
        return;
    ScopedRegKey key(rawKey);
    RegDeleteValueW(key.get(), L"LastWarn");
}

extern "C" BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_hInst = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, CLSID_xToolsEx64)) return CLASS_E_CLASSNOTAVAILABLE;

    XFactory* f = new (std::nothrow) XFactory();
    if (!f) return E_OUTOFMEMORY;
    HRESULT hr = f->QueryInterface(riid, ppv);
    f->Release();
    return hr;
}

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
    // 永远报告"不能卸载"：Explorer 可能在菜单仍显示时就调用本函数，
    // 一旦卸载，菜单里引用的位图/字符串就成了悬空指针，右键必崩。
    // 代价只是 Explorer 会一直持有本 DLL（卸载时重启 explorer 即可）。
    (void)g_busy;
    return S_FALSE;
}

static HRESULT RegisterServerImpl()
{
    // 先分配所有字符串/容器，再碰注册表：分配异常不会发生在写入之后。
    std::wstring dll = GetModulePath();
    if (dll.empty()) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);

    const REGSAM view64 = KEY_WOW64_64KEY;
    const REGSAM view32 = KEY_WOW64_32KEY;
    const std::wstring classes   = L"Software\\Classes";
    const std::wstring clsidKey  = classes + L"\\CLSID\\" + CLSID_STR;
    const std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    const std::wstring desc      = L"拖把更名器 右键扩展 (64 位)";
    const std::wstring desc32    = L"拖把更名器 右键扩展 (32 位)";
    std::wstring dll86 = GetModuleDir() + L"\\xToolsEx86.dll";
    const bool have86 = FileExistsW(dll86);

    std::vector<std::wstring> keys64, keys32;
    keys64.reserve(ARRAYSIZE(g_regTargets));
    keys32.reserve(ARRAYSIZE(g_regTargets));
    for (size_t i = 0; i < ARRAYSIZE(g_regTargets); ++i)
    {
        keys64.push_back(classes + L"\\" + g_regTargets[i] + L"\\shellex\\ContextMenuHandlers\\" + HANDLER_KEY);
        keys32.push_back(classes + L"\\" + g_regTargets[i] + L"\\shellex\\ContextMenuHandlers\\" + HANDLER_KEY86);
    }

    // ---- 写之前先给所有将被覆盖的值做快照。失败时只把这些值恢复原状，
    // 不删除既有注册：重复 regsvr32 因权限失败时，上一次能用的注册不会被清掉。
    std::vector<RegistryValueBackup> backups;
    backups.reserve(have86 ? 16 : 8);

    LONG err = BackupAndPush(HKEY_LOCAL_MACHINE, clsidKey, L"", view64, backups);
    if (err == ERROR_SUCCESS) err = BackupAndPush(HKEY_LOCAL_MACHINE, inprocKey, L"", view64, backups);
    if (err == ERROR_SUCCESS) err = BackupAndPush(HKEY_LOCAL_MACHINE, inprocKey, L"ThreadingModel", view64, backups);
    for (size_t i = 0; err == ERROR_SUCCESS && i < keys64.size(); ++i)
        err = BackupAndPush(HKEY_LOCAL_MACHINE, keys64[i], L"", view64, backups);
    const size_t count64 = backups.size();
    if (err == ERROR_SUCCESS && have86)
    {
        err = BackupAndPush(HKEY_LOCAL_MACHINE, clsidKey, L"", view32, backups);
        if (err == ERROR_SUCCESS) err = BackupAndPush(HKEY_LOCAL_MACHINE, inprocKey, L"", view32, backups);
        if (err == ERROR_SUCCESS) err = BackupAndPush(HKEY_LOCAL_MACHINE, inprocKey, L"ThreadingModel", view32, backups);
        for (size_t i = 0; err == ERROR_SUCCESS && i < keys32.size(); ++i)
            err = BackupAndPush(HKEY_LOCAL_MACHINE, keys32[i], L"", view32, backups);
    }
    if (err != ERROR_SUCCESS)
        return HRESULT_FROM_WIN32(err);   // 备份阶段就失败：一个字节也没写，无需回滚

    // ---- 64 位主注册写 HKLM：第三方文件管理器（Everything / Directory Opus）只查这里，
    // Explorer 走 HKCR 合并视图。regsvr32 需管理员身份，否则返回 0x80070005。
    //
    // touched*: 实际“尝试过”写入的备份项数量。失败后必须恢复这些项——
    // 即使写入失败，RegCreateKeyEx 也可能已经把键建出来了。
    // 而尚未尝试的备份项不必去“恢复”：它们根本没被改过。
    // （恢复本身已改用 RegOpenKeyExW，不会凭空创建路径；少做无用的恢复仍更稳妥。）
    size_t touched64 = 0;
    size_t touched32 = 0;

    err = TrySetKeyDefault(HKEY_LOCAL_MACHINE, clsidKey, desc, view64);
    ++touched64;
    if (err == ERROR_SUCCESS) { err = TrySetValue(HKEY_LOCAL_MACHINE, inprocKey, L"", dll, view64); ++touched64; }
    if (err == ERROR_SUCCESS) { err = TrySetValue(HKEY_LOCAL_MACHINE, inprocKey, L"ThreadingModel", L"Apartment", view64); ++touched64; }
    for (size_t i = 0; err == ERROR_SUCCESS && i < keys64.size(); ++i)
    {
        err = TrySetKeyDefault(HKEY_LOCAL_MACHINE, keys64[i], CLSID_STR, view64);
        ++touched64;
    }

    // ---- 32 位镜像：同目录存在 xToolsEx86.dll 时才写 32 位视图（32 位宿主如 Everything 1.4）。
    // 两套写在同一个事务里，任一失败都连同 64 位的一起恢复。
    if (err == ERROR_SUCCESS && have86)
    {
        err = TrySetKeyDefault(HKEY_LOCAL_MACHINE, clsidKey, desc32, view32);
        ++touched32;
        if (err == ERROR_SUCCESS) { err = TrySetValue(HKEY_LOCAL_MACHINE, inprocKey, L"", dll86, view32); ++touched32; }
        if (err == ERROR_SUCCESS) { err = TrySetValue(HKEY_LOCAL_MACHINE, inprocKey, L"ThreadingModel", L"Apartment", view32); ++touched32; }
        for (size_t i = 0; err == ERROR_SUCCESS && i < keys32.size(); ++i)
        {
            err = TrySetKeyDefault(HKEY_LOCAL_MACHINE, keys32[i], CLSID_STR, view32);
            ++touched32;
        }
    }

    if (err != ERROR_SUCCESS)
    {
        LONG rollbackErr = RestoreRegistryValues(HKEY_LOCAL_MACHINE, backups, 0, touched64, view64);
        if (have86 && touched32)
        {
            LONG r32 = RestoreRegistryValues(HKEY_LOCAL_MACHINE, backups, count64, count64 + touched32, view32);
            if (rollbackErr == ERROR_SUCCESS) rollbackErr = r32;
        }
        if (rollbackErr != ERROR_SUCCESS)
        {
            wchar_t msg[160];
            wsprintfW(msg, L"xToolsEx64: registration failed (%ld), rollback failed (%ld)", err, rollbackErr);
            ReportWarning(msg);
        }
        return HRESULT_FROM_WIN32(err);
    }

    bool warned = false;

    // ---- 清掉旧的 HKCU 注册（HKCU 在 HKCR 合并视图里优先于 HKLM，残留会让
    // Explorer 加载旧 DLL，所以要报告出来）；但它不参与 HKLM 事务，
    // 失败不回滚已生效的 HKLM 注册，避免"明明装好了却报失败"。
    LONG userErr   = DeleteRegistrationSet(HKEY_CURRENT_USER, false);
    LONG userErr32 = DeleteRegistrationSet(HKEY_CURRENT_USER, true);
    if (userErr == ERROR_SUCCESS) userErr = userErr32;
    if (userErr != ERROR_SUCCESS)
    {
        wchar_t msg[160];
        wsprintfW(msg, L"xToolsEx64: stale HKCU registration not removed (%ld)", userErr);
        ReportWarning(msg);
        warned = true;
    }

    // ---- 没有 xToolsEx86.dll 时清掉失效的 32 位镜像，
    // 否则 32 位宿主会去加载一个不存在的 InprocServer32。
    if (!have86)
    {
        LONG cleanupErr = DeleteRegistrationSet(HKEY_LOCAL_MACHINE, true);
        if (cleanupErr != ERROR_SUCCESS)
        {
            wchar_t msg[160];
            wsprintfW(msg, L"xToolsEx64: stale 32-bit registration not removed (%ld)", cleanupErr);
            ReportWarning(msg);
            warned = true;
        }
    }
    if (!warned) ClearWarning();

    // ---- 默认配置（HKCU，只在缺失时写入，不覆盖用户改动）。
    // 注册主体已成功，这里即使失败也不回滚注册。
    try
    {
        std::wstring cfg = CONFIG_KEY;
        if (ReadRegString(HKEY_CURRENT_USER, cfg, L"MenuText").empty())
            SetValue(HKEY_CURRENT_USER, cfg, L"MenuText", DEFAULT_MENU);
        if (ReadRegString(HKEY_CURRENT_USER, cfg, L"HelpText").empty())
            SetValue(HKEY_CURRENT_USER, cfg, L"HelpText", DEFAULT_HELP);
        // 投递已固定为 -a + 固定文件协议；Method 仅为兼容旧配置保留。
        if (ReadRegString(HKEY_CURRENT_USER, cfg, L"Method").empty())
            SetValue(HKEY_CURRENT_USER, cfg, L"Method", L"args");
        std::wstring exe = ReadRegString(HKEY_CURRENT_USER, cfg, L"ExePath");
        if (exe.empty())
        {
            std::wstring cand = GetModuleDir() + L"\\xTools.exe";
            if (FileExistsW(cand)) SetValue(HKEY_CURRENT_USER, cfg, L"ExePath", cand);
        }
    }
    catch (...) { /* 默认配置不影响 Shell 扩展主体注册结果 */ }

    return S_OK;
}

static HRESULT UnregisterServerImpl()
{
    // 四个视图依次清理：HKCU/HKLM × 64 位/Wow6432Node
    LONG firstError = ERROR_SUCCESS;
    LONG r = DeleteRegistrationSet(HKEY_CURRENT_USER, false);
    if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    r = DeleteRegistrationSet(HKEY_LOCAL_MACHINE, false);
    if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    r = DeleteRegistrationSet(HKEY_CURRENT_USER, true);
    if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    r = DeleteRegistrationSet(HKEY_LOCAL_MACHINE, true);
    if (firstError == ERROR_SUCCESS && r != ERROR_SUCCESS) firstError = r;
    return firstError == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(firstError);
}

extern "C" HRESULT __stdcall DllRegisterServer()
{
    try { return RegisterServerImpl(); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}

extern "C" HRESULT __stdcall DllUnregisterServer()
{
    try { return UnregisterServerImpl(); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
