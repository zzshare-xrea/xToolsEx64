# 变更记录

`xToolsEx64.dll` 从第一版到目前的全部改动，按批次记录。批次编号只是开发顺序，
对外发布只有“当前版本”一个；每一批都可以独立回退。

当前产物：`release/xToolsEx64.dll`，1,042,130 字节，依赖
`ADVAPI32 / GDI32 / KERNEL32 / msvcrt / ole32 / SHELL32 / USER32`，
导出 `DllCanUnloadNow / DllGetClassObject / DllRegisterServer / DllUnregisterServer`。

> 早期批次里提到的 `xToolsEx64_modified.cpp`、`xToolsEx64_batch1_3.cpp` 等文件名，
> 是当时对照过的第三方修改稿，未包含在本仓库，仅作记录。

---

## 本次吸收的 5 项改进（基于用户修改版对比）

**1. `CF_HDROP` 句柄修复（最重要）**

```cpp
// 旧：HDROP hdrop = (HDROP)GlobalLock(stg.hGlobal);   ← 把指针当句柄
HDROP hdrop = (HDROP)stg.hGlobal;                     // 正确：hGlobal 本身就是 HDROP
```

`CF_HDROP` 的 `hGlobal` 就是 HDROP 句柄，`DragQueryFileW` 要的是句柄不是锁定指针。
旧写法在真实 Explorer 里等于传非法句柄，**很可能就是右键 Explorer 崩溃的主因**
（wine 对非法句柄容错高，所以之前测试没抓到）。

**2. `Initialize` 的路径处理**

- `m_folder.clear()`：对象被复用时不会把上一次的文件夹当成这次的目标（串数据 bug）。
- `SHGetNameFromIDList(pidl, SIGDN_FILESYSPATH, &p)` 替代 `SHGetPathFromIDListW`：
  支持超过 MAX_PATH 的路径和 UNC 路径，用完 `CoTaskMemFree`。
- `DragQueryFileW(hdrop, i, NULL, 0)` 先取长度、再动态分配：
  原来固定 `wchar_t path[MAX_PATH*2]` 会把长路径截断。

**3. 投递环节的并发与完整性**

- 命名互斥量 `Local\xToolsEx64_Handoff` 把「写列表 + 启动 + 等窗口」串行化，
  避免连续两次投递互相覆盖 `~XTLS50.tmp` 造成文件串单；并且**检查等待超时**
  （超时提示"上一次投递仍在进行"，不会硬闯）。
- `WriteListFile` 改为**动态长度 ANSI 转换 + `WriteAll` 分块写 + `FlushFileBuffers`**：
  原来 `char buf[MAX_PATH*4]` 固定缓冲会截断长路径，且一次 `WriteFile` 后直接关闭，
  `-a` 启动时可能读到不完整列表。
- `%TEMP%` 用 32768 字符缓冲取（临时目录被重定向到长路径时 `MAX_PATH` 会失败）。
- 锁范围优化：只有「写+启动+等主窗口出现」在锁内，窗口置前放到锁外，避免长时间占锁。

**4. 分配与返回值检查**

- `new (std::nothrow)` 用于 `XMenu` / `XFactory` / pair：`-fno-exceptions` 下原来的
  `new` 失败会直接 terminate，把 Explorer 一起带走。
- `InsertMenuItemW` 失败返回 `HRESULT_FROM_WIN32(GetLastError())`。
- `CreateThread` 失败返回 `HRESULT_FROM_WIN32(GetLastError())`（原来返回 `E_FAIL`）。
- `idCmdFirst > idCmdLast` 时提前返回。

**5. 去掉 WM_DROPFILES 兜底，但保留窗口置前**

原程序的可靠入口只有 `-a` + `~XTLS50.tmp`（它自己走 OLE `RegisterDragDrop`，
`WM_DROPFILES` 消息无效）。所以删掉了 `MakeDropHandle` / `SendDrop` 那一整套
（原实现会在后台线程里 `Sleep(150)` 轮询 + `EnumWindows` 全量枚举 + 每个窗口
`OpenProcess` 查映像名，最长能耗 8 秒）。

但**保留了 `WaitExeWindow` + `ActivateWindow`**：点完菜单 xTools 窗口仍会被带到前台，
不会让用户以为"点了没反应"。

### 关于 `Method` 配置

投递方式已固定为 `-a` 协议，`Method` 键不再影响行为，仅为兼容旧配置保留
（注册时默认写 `args`）。你原有 `Method=both` 不会被覆盖，也不会再被读取。

### 未采纳的三条（原因）

1. **"`~XTLS50.tmp` 已存在就取消并弹框"**：只要上一次留下残留（程序崩溃、权限问题），
   之后每次右键都会失败，用户还得手动去 `%TEMP%` 清理。原版是直接覆盖，体验更稳。
2. **"以原程序删除 `~XTLS50.tmp` 为成功判据、等 15 秒"**：前提是原程序读完会**删除**该文件。
   目前实测只确认它会**重写**，若它其实不删，这逻辑会导致每次右键卡 15 秒再弹错误框。
   要用这条必须先实测一次"读取后 tmp 是否消失"。
3. **`try/catch`**：与 `-fno-exceptions` 冲突。采纳就得改编译参数、体积上涨；
   DLL 里若只为了兜异常，代价偏高。

## 第二批改进（`xToolsEx64_batch1_3.cpp` 中可借鉴的部分）

**1. `LaunchExe`：命令行用可写缓冲 + 指定工作目录（两条都重要）**

```cpp
// 旧：CreateProcessW(exe, (LPWSTR)cmd.c_str(), ..., NULL, ...)
BOOL ok = CreateProcessW(exe.c_str(), &cmd[0], ..., workDir.c_str(), &si, &pi);
```

- `CreateProcessW` 会**就地改写**命令行缓冲区，把只读的 `c_str()` 强转 `LPWSTR` 传进去是
  写只读内存的未定义行为，可能改坏 `std::wstring` 内部状态。
- 旧代码工作目录传 `NULL` → 新进程继承 Explorer 的当前目录（通常是 `System32`）。
  拖把更名器是绿色版，要读**同目录**的 `Language\*.ini`（菜单文案、界面语言）。
  从错误目录启动可能读不到语言文件。现在工作目录固定为 `xTools.exe` 所在目录。

**2. `GetModulePath()` 动态扩容**

原来固定 `MAX_PATH*2`（520 字符），DLL 装在深层目录时会被截断，`ResolveExePath`
就找不到同目录的 `xTools.exe`。现在从 260 起倍增到 32768，直到取得完整路径。

**3. 互斥量拿不到就放弃投递**

`CreateMutexW` 失败时旧代码会"没锁也照写"，等于放弃串行化；现在直接报错返回。

**4. 其他小项**

- `EnumWindows` 回调里删掉白调用的 `GetWindowTextLengthW(hwnd);`（返回值被丢弃），
  少一次 API 调用；`GetWindowLongW` → `GetWindowLongPtrW`。
- `InsertMenuItemW` / `CreateThread` 失败时用 `err ? err : ERROR_GEN_FAILURE /
  ERROR_NOT_ENOUGH_MEMORY` 兜底：`GetLastError()` 返回 0 时 `HRESULT_FROM_WIN32(0)`
  equals `S_OK`，会变成"假成功"。
- `GetCommandString` 的 ANSI 分支检查 `WideCharToMultiByte` 返回值。
- `DeleteFileW(listfile)` 只在「写成功但启动失败」时调用。
- `WriteListFile` 增加 `size > INT_MAX` 防御；`#include <climits>`。
- 删掉三处死代码 `SameFile` / `BuildArgs` / `SetKeyDefault`。

### 第二批明确未采纳的两条

1. **`WriteListFile` 改成 `CreateFileW(..., CREATE_NEW, ...)`**：只要 `~XTLS50.tmp`
   有残留（程序崩溃、权限问题、原程序没删），之后每次右键都会写失败并弹错误框，
   用户还得手动清理 `%TEMP%`。原版 DLL 是直接覆盖的，保持覆盖更稳。
2. **`WC_NO_BEST_FIT_CHARS` + `usedDefault` 时整批失败**：探测编码有价值
   （emoji 文件名转 CP_ACP 会变成 `?`），但一个文件就废掉整批投递，且只弹一句
   "写入文件列表失败"，用户无法判断是哪个文件。需要时改为"跳过该条并统计提示"。

## 第三批改进（`xToolsEx64_exception_gdi.cpp`）

**1. 菜单图标：GDI 绘制失败路径全部清理**

原来 `CreateCompatibleDC` 成功但 `DrawIconEx` 失败时，会返回一张 **alpha 全 0** 的位图——
菜单项图标位是空的。现在三处失败（`CreateCompatibleDC` / `SelectObject` 返回 `HGDI_ERROR` /
`DrawIconEx`）都 `DeleteObject(hbmp)` 并返回 `NULL`，菜单退化为"无图标"，既不显示空白也不泄漏。
这些删除都发生在位图**从未交给 Shell** 的阶段，无风险。

**2. 位图所有权收回 `XMenu`，析构时释放**

`MIIM_BITMAP` 的 `hbmpItem` 所有权在扩展这边（Shell 不负责释放），所以位图句柄改为存在
`m_menuBitmaps` 里，`~XMenu()` 统一 `DeleteObject`；`InsertMenuItemW` 失败时立即
`pop_back` + `DeleteObject`。不再"每弹一次菜单漏 1KB"。

**诊断开关 `IconFree`**：
```
reg add "HKCU\Software\xTools\ShellEx64" /v IconFree /t REG_SZ /d 0 /f
```
设为 `0` 时回到旧的"永不删除"策略。若现场再出现右键崩溃，先用它排除
"删除时机早于菜单销毁"这一类原因，不必重新编译。恢复默认：删除该值或设为 `1`。

**3. 异常边界：线程入口与 COM 方法**

- `HandoffThread` 用 `try` 包住 `HandoffFiles`，异常时弹提示，且 `delete p` 与
  `InterlockedDecrement(&g_busy)` 一定执行（原来异常会跳过这两步，`g_busy` 永远 >0）。
- `Initialize` / `QueryContextMenu` / `InvokeCommand` / `GetCommandString` 全部加
  `try/catch`，`bad_alloc` → `E_OUTOFMEMORY`，其他 → `E_FAIL`。C++ 异常一旦越过
  COM 方法边界，Explorer 会直接终止进程。
- 细节：`Initialize` 里 `m_folder.assign()` 若抛出，先 `CoTaskMemFree(pszPath)` 再重抛；
  `CF_HDROP` 的读取循环若抛出，先 `ReleaseStgMedium(&stg)` 再返回，避免 STGMEDIUM 泄漏。
- 说明：`new (std::nothrow)` 只覆盖 pair **自身**的内存分配，pair 内 `vector`/`wstring`
  的复制仍可能抛，所以 `InvokeCommand` 那层 `try` 是必要的。

**4. 编译参数变化：去掉 `-fno-exceptions`**

```
x86_64-w64-mingw32-g++ -std=c++11 -Os -fno-rtti -ffunction-sections -fdata-sections -shared \
  -o xToolsEx64.dll xToolsEx64.cpp xToolsEx64.def -Wl,--gc-sections \
  -lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luser32 -lgdi32 \
  -static -static-libgcc -static-libstdc++
```

`-fno-rtti` 保留（与异常不冲突）。代价是体积：1,017,019 → 1,023,655 字节（+6.6 KB），
换来的是 `bad_alloc` 不再变成 `std::terminate`。

## 第四批改进（`xToolsEx64_batch1_2_fixed.cpp` 中采纳的 1–4 项）

**1. 析构函数不再碰注册表（修掉一个真实崩溃点）**

原来 `~XMenu()` 第一行是 `if (!FreeMenuBitmap()) return;`，而 `FreeMenuBitmap()` 要读注册表、
返回 `std::wstring`（会分配内存）。C++11 起析构函数默认 `noexcept`，一旦这里抛出
（`bad_alloc`、注册表访问异常）就是 `std::terminate` —— Explorer 直接消失，连加的
`catch (...)` 都接不住（catch 只覆盖函数体，不覆盖析构）。

现在 `IconFree` 判断移到 `QueryContextMenu`（正常调用上下文），析构只做 `DeleteObject`，
不 I/O、不分配。

**2. RAII 接管资源生命周期**

新增四个小类，替换手写的 `CloseHandle` / `ReleaseStgMedium` / `ReleaseMutex`：

- `ScopedHandle`：文件句柄、互斥量句柄
- `ScopedStgMedium`：`IDataObject::GetData` 成功后自动 `ReleaseStgMedium`，
  替代原来"外层 try + 内层 try + 两处 ReleaseStgMedium"的写法（不会漏放也不会双放）
- `ScopedMutexLock`：只 `ReleaseMutex`，且**必须先 `unlock()` 再 `reset()` 句柄**——
  持锁时 `CloseHandle` 会把互斥量变成 abandoned
- `ScopedDeleteFile`：列表文件写成功后暂由本线程负责删除，`LaunchExe` 成功才
  `release()` 放弃责任（异常路径也会清理）

`WriteListFile` 的异常路径也按正确顺序处理：

```cpp
catch (...) { h.reset(); DeleteFileW(path); return false; }  // 先关文件，否则删不掉
```

**3. `m_folder` 语义修正（并保留背景菜单的兜底）**

`pidlFolder` 只是"被选中项所在的文件夹"。选中文件/文件夹时它是**父目录**，不能当投递目标；
原来若 `CF_HDROP` 取不到而 `files` 为空，会把父目录整个加进列表（加错对象）。
现在选中对象场景不再使用 `pidlFolder`，但保留兜底：

```cpp
if (m_files.empty()) m_folder.swap(folderFromPidl);
```

这样"文件夹空白处右键"即使拿不到 `CF_HDROP`，菜单也不会消失。

**4. 两处动态缓冲**

- `REG_EXPAND_SZ` 的 `ExePath`：`ExpandEnvironmentStringsW(x, NULL, 0)` 先取长度再分配
  （展开结果可能超过 `MAX_PATH*2`）
- `SearchPathW` 找 `xTools.exe`：缓冲不足时按返回的所需长度扩容

**编译结果**：1,025,375 字节（比上一版 +1,720），依赖仍是 7 个系统 DLL。

### 尚未采纳：注册逻辑重构

`RegisterServerImpl` / `UnregisterServerImpl` / `DeleteRegistrationSet` 那一整套
（先分配后写、失败回滚、`TrySet*` 返回 `LONG`、`xToolsEx86.dll` 不存在时清失效镜像）
方向正确，但一次动了注册/注销的全部路径，需要单独一轮 + 完整回归
（安装 → 验证 → 卸载 → 再安装 → 第三方验证），避免和投递逻辑的问题混在一起。

## 第五批改进：注册逻辑重构

把 `DllRegisterServer` / `DllUnregisterServer` 拆成 `RegisterServerImpl` /
`UnregisterServerImpl` + `DeleteRegistrationSet`，改了四件事：

**1. 先分配、后写注册表**

所有 `std::wstring` / `std::vector`（DLL 路径、CLSID 键、5 个 handler 键）在**动注册表之前**
就构造完。避免"写到第 3 个键时 `bad_alloc`"留下半截注册。

**2. 失败整体回滚**

任一写入失败 → `DeleteRegistrationSet` 把本次写的键全部删掉，然后返回
`HRESULT_FROM_WIN32(err)`。回滚过程用**栈上 512 字符固定缓冲**拼键名，不分配内存，
所以不会"回滚时又 OOM"。

**3. `TrySet*` 返回 `LONG` 错误码**

原来是 `bool`，失败一律返回 `E_ACCESSDENIED`，看不出真实原因。现在返回 Win32 错误码：

| 返回值 | 含义 |
| --- | --- |
| `0x80070005` | `ERROR_ACCESS_DENIED`——没有管理员权限 |
| `0x8007000D` / 其它 | 键/值写入失败的具体错误 |

**4. `xToolsEx86.dll` 不存在时清掉失效的 Wow6432Node 镜像**

旧版本装过 32 位镜像、后来把 `xToolsEx86.dll` 删掉，注册表里仍留着指向它的
`InprocServer32`，32 位宿主（Everything 1.4）加载时就会报找不到模块。
现在 `have86 == false` 时主动 `DeleteRegistrationSet(HKLM, true)`。

### 语义变化（重要）

- **"全有或全无"**：注册失败会清掉本次写入的整套键（包括 CLSID）。重复执行 `regsvr32` 时
  若因权限失败，不会保留上一次的可用注册。
- **HKCU 清理失败会回滚 HKLM**：`HKCU\Software\Classes` 里的残留优先于 HKLM（HKCR 合并视图），
  留着会让 Explorer 加载旧 DLL，所以清不掉就整体失败。这种失败极少见（HKCU 用户可写）。
- 配置项（`MenuText` / `HelpText` / `Method` / `ExePath`）写入失败**不回滚**注册——
  它们只影响菜单显示，不影响扩展加载。

### 建议的回归顺序

1. 管理员 `regsvr32 xToolsEx64.dll` → 应提示成功（若提示 `0x80070005` 就是没提权）
2. 注册表检查：`HKLM\Software\Classes\*\shellex\ContextMenuHandlers\xToolsEx64` 存在
3. 右键文件 / 文件夹 / 驱动器 / 空白处 → 四个位置都出现菜单
4. `regsvr32 /u xToolsEx64.dll` → 四个 handler 键与 CLSID 全部消失
5. 再重新 `regsvr32` → 仍正常（验证可重复安装）
6. 同目录放 `xToolsEx86.dll` 后再 `regsvr32` → `Wow6432Node` 下出现镜像键

**编译结果**：1,027,454 字节（比上一版 +2,079），依赖仍是 7 个系统 DLL。

## 第六批改进：注册事务改为“先备份、后写、失败只恢复被覆盖的值”

这一批修正了第五批留下的一个破坏性副作用。

**第五批的问题**：失败时调用 `DeleteRegistrationSet` 把**整套注册删掉**。后果是
重复执行 `regsvr32` 时，若因为权限原因失败，**上一次本来能用的注册也会一起消失**。

**现在的做法**：写之前先给每个将被覆盖的值做快照（记录是否存在、类型、原始字节）：

```cpp
BackupAndPush(HKEY_LOCAL_MACHINE, clsidKey, L"", view64, backups);
...
if (err != ERROR_SUCCESS)
    RestoreRegistryValues(HKEY_LOCAL_MACHINE, backups, 0, count64, view64);
```

- 备份阶段就失败 → 直接返回，**一个字节都没写**，无需回滚；
- 写入阶段失败 → 逆序恢复这些值：原来存在的写回旧值，原来不存在的删掉该值。
  键本身保留，**不会误删既有注册**。

**视图定位改为 WOW64 标志**

不再手拼 `Software\Classes\Wow6432Node\...`，改用 `KEY_WOW64_32KEY` / `KEY_WOW64_64KEY`
让系统自己重定向；`DeleteRegistrationSet` 也改为先打开 `Software\Classes`、再对
**相对子键**调 `RegDeleteTreeW`，避免绝对路径与视图标志混用对不上。

**HKCU 清理失败不再回滚 HKLM**

`HKCU\Software\Classes` 在 HKCR 合并视图里优先于 HKLM，残留会让 Explorer 加载旧 DLL，
所以必须报告；但它不参与 HKLM 事务，失败**只警告、不回滚**，避免“明明装好了却报失败”。

警告有两个出口：

1. `HKCU\Software\xTools\ShellEx64\LastWarn`（字符串，值为错误码），安装脚本会读并显示；
2. `OutputDebugStringW`，调试器可见。

全部干净时 `LastWarn` 会被删除。

**`ScopedRegKey`**

`ReadRegString` / `SetValue` / `TrySet*` 全部改用 RAII 关闭句柄，提前 `return` 的路径
不再漏 `HKEY`。

### 没采纳的一条

参考版删掉了 `if (m_files.empty()) m_folder.swap(folderFromPidl);` 这个兜底。
理由是“有 IDataObject 就不是背景菜单”。但这个前提没法保证：真实 Explorer 的背景菜单
经常也传 IDataObject，一旦 `CF_HDROP` 取不到，`m_files` 和 `m_folder` 都空，
`QueryContextMenu` 直接返回 S_OK → **文件夹空白处右键没有菜单**。
所以保留兜底。

### 回归

1. 管理员 `regsvr32 xToolsEx64.dll` → 成功
2. 删掉 `HKLM\Software\Classes\*\shellex\ContextMenuHandlers\xToolsEx64` 的一个值后
   再 `regsvr32` → 应恢复
3. 故意用非管理员执行 `regsvr32` → 返回 `0x80070005`，且**原来可用的注册仍在**
   （这是本批与上一版的关键差别）
4. 四处右键（文件 / 文件夹 / 驱动器 / 空白处）都出菜单
5. `regsvr32 /u` 全部消失

**编译结果**：1,037,508 字节（比第五批 +10,054），依赖仍是 7 个系统 DLL。

## 第七批改进：兜底条件精确化 + 回滚清空键 + 删树权限

三处小修，都是给第六批收尾的。

**1. 兜底加条件，避免投递空路径**

```cpp
if (m_files.empty() && !folderFromPidl.empty())
    m_folder.swap(folderFromPidl);
```

原来只要 `m_files` 空就 swap。若 `pidlFolder` 没解析出文件系统路径（`folderFromPidl` 为空），
就会把**空字符串**当成投递目标写进 `~XTLS50.tmp`，原程序读到空行行为不可控。
加上 `!folderFromPidl.empty()` 后：解析得出才兜底，解析不出就不 swap——
这种情况下菜单不出现，比投递一个空路径干净。

**2. 回滚时删掉本次新建的空键**

第六批的回滚只恢复“值”，如果某个 handler 键本来不存在，回滚后会把**空键**留在注册表里。
Shell 遇到空 handler 键仍会尝试加载，属于垃圾状态。

现在 `RegistryValueBackup` 多记一个 `keyExisted`；恢复完值后，若该键原本不存在，
就把它删掉：

```cpp
key.reset();          // 先关句柄，正被本进程打开的键删不掉
if (r == ERROR_SUCCESS && !backup.keyExisted)
    delErr = RegDeleteKeyExW(root, backup.path.c_str(), view, 0);
```

注意顺序：**必须先 `reset()` 关掉句柄再删**，否则自己打开的键删不掉。
同一键可能有多个备份项（例如 `CLSID` 键下同时有默认值和 `ThreadingModel`），
逆序恢复时前面的会因键非空而删不掉（`ERROR_DIR_NOT_EMPTY` 忽略），最后一个负责删。
`keyExisted == true` 的键一律不删，避免误伤既有注册。

**3. `RegDeleteTreeW` 前补齐权限**

打开 `Software\Classes` 时的访问掩码从 `KEY_WRITE | DELETE` 扩到：

```cpp
KEY_WRITE | DELETE | KEY_ENUMERATE_SUB_KEYS | KEY_QUERY_VALUE | view
```

`RegDeleteTreeW` 内部要枚举子键，缺 `KEY_ENUMERATE_SUB_KEYS` 在个别受策略收紧的机器上会失败。

### 没采纳

`#include <utility>`：全文件没有用 `std::move` / `std::pair`，加了只是噪音。

**编译结果**：1,037,708 字节（比第六批 +200），依赖仍是 7 个系统 DLL。

## 第八批改进：TYMED 校验 + 回滚范围精确化

**1. `GetData` 之后校验 TYMED（重要）**

`IDataObject::GetData` 返回 `S_OK` **不代表**拿到的就是请求的 `TYMED_HGLOBAL`。
Shell 可能给出别的 medium（`TYMED_ISTREAM`、`TYMED_NULL`），此时 `stg.hGlobal`
根本不是 HDROP。原来直接 `(HDROP)stg.hGlobal` 强转再喂给 `DragQueryFileW`，
等于传一个非法句柄——这正是第一批修过的崩溃模式的另一半。

```cpp
if (stg.tymed != TYMED_HGLOBAL || !stg.hGlobal)
    return DV_E_TYMED;
```

提前返回时 `ScopedStgMedium` 已接管，`ReleaseStgMedium` 仍会执行，不泄漏。

**2. 回滚只恢复“实际尝试过写入”的项**

第六批的回滚是 `RestoreRegistryValues(0, count64)`——把**全部备份项**都恢复。
问题在于：`RestoreRegistryValue` 内部用 `RegCreateKeyEx`，如果失败发生在第一个写入，
后面那些从未写过的备份项在“恢复”时**会凭空创建注册表键**。

现在用 `touched64 / touched32` 记录实际尝试过的项数：

```cpp
err = TrySetKeyDefault(...);
++touched64;                  // 即使失败也计数：RegCreateKeyEx 可能已建出键
...
RestoreRegistryValues(HKEY_LOCAL_MACHINE, backups, 0, touched64, view64);
if (have86 && touched32)
    RestoreRegistryValues(HKEY_LOCAL_MACHINE, backups, count64, count64 + touched32, view32);
```

**3. 进程映像路径缓冲改 32768**

`IsProcessPath` 里 `QueryFullProcessImageNameW` 的缓冲原来是栈上 `MAX_PATH * 2`（520 字符），
长路径会查询失败；而失败时函数返回 `TRUE`（当作“可能是目标窗口”），
于是可能把别人的窗口误判成 xTools 主窗口。改成 `std::vector<wchar_t> path(32768, 0)`。

**编译结果**：1,037,954 字节（比第七批 +246），依赖仍是 7 个系统 DLL。

## 第九批改进：图标兼容（只关心显示，不动投递）

原版 `xTools.exe` 的图标资源只有**一张 32x32 / 24bpp**，既没有 16x16，也没有 256 的 PNG。
这带来两个在不同机器上表现不一致的问题，都已修：

**1. 只请求“小图标”会拿不到**

```cpp
ExtractIconExW(exe, 0, NULL, &hIcon, 1)   // 旧写法：只要 small
```

图标组里没有 16x16 时，`phiconSmall` 在部分系统/配置下直接返回 NULL，于是无图标。
现在大小图标都要，谁有就用谁（`DrawIconEx` 会缩放到菜单的 `SM_CXSMICON`）：

```cpp
UINT nIcons = ExtractIconExW(exe.c_str(), 0, &hLarge, &hSmall, 1);
HICON hIcon = hSmall ? hSmall : hLarge;
```

**2. 老式 24bpp 图标没有 alpha 通道**

`DrawIconEx(DI_NORMAL)` 画到 32bpp DIB 时，某些系统/GDI 实现**不写 alpha**（整张 0）。
预乘处理把 `a == 0` 的像素清成 0 → 整张图全透明 → 菜单上看起来就是“没有图标”。

现在先扫描 alpha：若全为 0，用 `DI_MASK` 再画一遍取遮罩，按
“mask 白 = 透明、mask 黑 = 不透明”重建透明度。这样老图标也能正确显示。

### 关于“重命名 xTools.exe 就提示找不到”

这是**设计如此**，不是 bug。`ResolveExePath()` 的查找顺序：

1. 配置 `HKCU\Software\xTools\ShellEx64\ExePath`（安装脚本会写）
2. 与 `xToolsEx64.dll` 同目录的 `xTools.exe`
3. 上一级目录的 `xTools.exe`、上一级下的 `拖把更名器\xTools.exe`
4. `App Paths\xTools.exe`
5. `PATH`

文件名一旦改掉，2-5 全部落空，就弹“没有找到 xTools.exe”。
要支持改名：把新文件名写进 `ExePath` 即可（`ExePath` 优先且只检查文件是否存在）。

**编译结果**：1,038,466 字节（比第八批 +512），依赖仍是 7 个系统 DLL。

## 第十批改进：tmp 被目录占用修复 + 真正的 OLE 拖放兜底

**1. `%TEMP%\~XTLS50.tmp` 是文件夹时必然失败（真 bug）**

`CreateFileW(path, ..., CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, ...)` 打开一个**目录**永远失败，
所以只要临时目录里存在同名文件夹，每次右键都会“写入文件列表失败”，主程序也起不来。

现在写之前先整理目标路径：

```cpp
static bool PrepareListFilePath(const std::wstring& path)
{
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { /* 只读则清只读位 */ return true; }
    // 是目录：改名让位（同卷 rename，不复制内容），.bak 冲突就换 .bak2/.bak3...
}
```

失败时（例如目录无法改名）提示会直接指出这是文件夹并给出完整路径，让用户手动处理。

**2. 兜底改成真正的 OLE 拖放**

旧版本那条 `WM_DROPFILES` 是无效的——原程序没有 `WS_EX_ACCEPTFILES`，走的是
`RegisterDragDrop`。现在直接调它的 `IDropTarget`：

- `GetPropW(hwnd, L"OleDropTargetInterface")` 拿到窗口注册的拖放目标
  （主窗口、子控件都查一遍，因为 `TPanel`/`TListView` 可能各自注册）；
- 自实现一个最小 `IDataObject`，只提供 `CF_HDROP` / `TYMED_HGLOBAL`
  （`DROPFILES` + `fWide=TRUE` 宽字符路径列表），`EnumFormatEtc` 用
  `SHCreateStdEnumFmtEtc` 提供标准枚举；
- 依次 `DragEnter` → `DragOver` → `Drop`，效果为 `DROPEFFECT_COPY | DROPEFFECT_MOVE`；
- 线程先 `OleInitialize`（`S_FALSE` 也要配对 `OleUninitialize`）。

**触发条件**：只有 `-a` 协议这条路失败（写入失败 / 启动失败）时才走兜底；
此时若主程序还没起来，先不带 `-a` 把它拉起来，再投递。
兜底成功就静默返回，不再弹错误框。

**编译结果**：1,105,628 字节（+67,162，主要是 COM 虚表与拖放代码）。
依赖仍是那 7 个系统 DLL（`ADVAPI32 / GDI32 / KERNEL32 / msvcrt / ole32 / SHELL32 / USER32`），
没有引入新的运行时依赖。

## 第十一批：撤销 OLE 拖放兜底（实测导致 Explorer 崩溃）

**崩溃原因（重要，以后不要再尝试）**

`xTools.exe` 是**独立进程**。OLE 在 `RegisterDragDrop` 时把拖放目标存进窗口属性
`OleDropTargetInterface`，但那存的是**它自己进程地址空间里的指针**。
在 Explorer 进程里 `GetPropW` 读到的只是同一个数值，指针指向的是**另一个进程的
内存**——在这里完全无效。强转成 `IDropTarget*` 再调虚方法 = 访问非法地址，
Explorer 必崩。

窗口属性不是跨进程共享的机制，跨进程拿不到可用的 `IDropTarget`（OLE 内部靠 marshal
处理，没有公开 API）。所以“模拟拖放”这条路在架构上不成立，不是参数调错的问题。

**现在的行为**

- 保留 `PrepareListFilePath`：`~XTLS50.tmp` 是**目录**时改名让位（`.bak` / `.bak2`…），
  是只读文件时清掉只读位。
- 改名不成功 → `WriteListFile` 返回失败 → 弹明确提示，说明那是个文件夹并给出完整路径，
  由用户手动删除/移走。
- 不做任何拖放兜底尝试。

**编译结果**：1,039,884 字节（去掉拖放代码后回落到第九批附近），依赖仍是 7 个系统库。

## 第十二批：回滚不再凭空建键 + 回调异常保护 + 只读位检查 + 头注释修正

**1. `RestoreRegistryValue` 改 `RegOpenKeyExW`，删键改为按“键”收尾**

原来是先读 `RegCreateKeyExW`，回滚时会重建整条路径。现在：

- `RegOpenKeyExW` 打开；打不开时，`原值也不存在` → 视为成功，`原值本该存在` → 返回错误（不假装恢复成功）。
- **删键从“每个值”移到“每个键”的收尾阶段**，原因：一个键通常有多个备份值
  （`InprocServer32` 的默认值 + `ThreadingModel`），而 `RegDeleteKey` 只要求无子键、
  不看有没有值。按值删键会把键提前删掉，后面的值就无处可写。
- 收尾只删“该 path 的所有备份项都满足 `原本无键 + 原本无值`”的键，且同一 path 只删一次。

**2. `FindEnumProc` 缓冲复用 + 异常保护**

- 32768 的缓冲移到 `FindCtx` 里，`FindExeWindow` 分配一次，回调复用。
- 回调整体包 `try/catch(...) return TRUE`：异常绝不允许越过 `EnumWindows` 的 C 回调边界。

**3. `PrepareListFilePath` 检查 `SetFileAttributesW` 返回值**

- 只读文件清位失败 → 直接 `false`，上层立刻给“写入失败 + 目标路径”，不再拖到 `CreateFileW` 才失败。
- 目录分支也先清只读位再改名（只读目录 `MoveFileW` 会失败）。

**4. 文件头注释**

改成写 `~XTLS50.tmp` + `-a` + 置前的真实实现，并写明两条已否决的路（`WM_DROPFILES`、OLE 拖放）
及各自原因。编译命令注释同步更新（补 `-std=c++11 -Os -fno-rtti`，并注明不能加 `-fno-exceptions`）。

**编译结果**：1,042,106 字节，依赖仍是 7 个系统库，导出 4 个入口齐全。

**补充（第十二批内）：回滚补上 `ERROR_PATH_NOT_FOUND`**

`RegOpenKeyExW` 的“键不存在”有两种返回值：中间层缺失给 `ERROR_PATH_NOT_FOUND`，
只有叶子层缺失给 `ERROR_FILE_NOT_FOUND`。回滚只认了后者，于是“整条路径原本都不存在”
这种最常见的情况反而被报成回滚失败（写 `LastWarn`、弹警告）。现已改为：

```cpp
if (!backup.keyExisted &&
    (r == ERROR_FILE_NOT_FOUND || r == ERROR_PATH_NOT_FOUND))
    return ERROR_SUCCESS;
```

用 `keyExisted` 而不是 `existed`，与备份侧 `BackupRegistryValue` 的判断保持对称
（那边本来就同时接受两个返回码）。

**注释校正（无功能改动，四条）**

- `ActivateWindow` 上方残留的分节标题还写着“构造 WM_DROPFILES 数据”“把 wchar 列表拼成命令行”，
  已改为“把主窗口带到前台”，并说明为什么不再发消息。
- `FindEnumProc` 里 `QueryFullProcessImageNameW` 失败的行为是**跳过当前窗口继续枚举**，
  原注释“当作可能是目标”是反的，已改正。
- `RegistryValueBackup::keyExisted` 原注释说“删除本次新建的空键”，
  但 `RegDeleteKey` 只要求无**子键**、根本不看有没有值，已改正。
- 注册事务里“恢复用的是 RegCreateKeyEx，那会凭空创建键”已与实现相反——
  现在恢复用 `RegOpenKeyExW`，正是为了不凭空创建；注释已重写。

**修掉一个投递竞态（启动失败时误删下一次的文件）**

`ScopedDeleteFile` 原来只在析构里删，而 `HandoffFiles` 是“先放开互斥量、再弹错误框”。
错误框会挂到用户点掉为止，这期间第二次右键完全可以拿到锁、重写同名 `~XTLS50.tmp`；
等第一次的提示框关掉、函数退出，析构就把**第二次刚写的文件**删了，第二次启动的主程序
于是读不到列表。

改法：给 `ScopedDeleteFile` 加幂等的 `cleanup()`（析构改为调用它），并在**锁内**分派：

```cpp
if (launched) cleanupList.release();   // 交给主程序读，不删
else cleanupList.cleanup();            // 没交出去，立刻删
```

提示框仍在锁外弹，用户看提示期间不占锁，但删除权已经在锁内交还了。
`wrote == false` 时 `active` 本来就是 false，`cleanup()` 不会去动磁盘上原有的同名文件。

**`cleanup()` 加锁内有限重试（但不放回锁外）**

杀软/索引器常会短暂占用刚写的文件，单次 `DeleteFileW` 容易失败。现在锁内重试 4 次、
每次间隔 20ms（最多 80ms），覆盖这类瞬态占用；`ERROR_FILE_NOT_FOUND /
PATH_NOT_FOUND` 直接视为已完成。

**关键取舍**：无论重试结果如何，`m_active` 都必须清掉。如果把 `m_active` 留着让析构
“再删一次”，那次删除发生在 `mutexLock.unlock()` 之后（错误提示框期间锁就已放开），
正好会删掉下一次投递刚写好的同名文件——那就把上一轮修好的竞态又放回来了。

删除彻底失败只会残留一个 `~XTLS50.tmp`，本身无害：下一次投递用 `CREATE_ALWAYS`
直接覆盖它；若它变成目录，`PrepareListFilePath` 会先改名让位。
