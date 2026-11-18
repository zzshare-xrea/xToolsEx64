# xToolsEx64 —— 拖把更名器 64 位右键菜单扩展

让 **拖把更名器（xTools / SmartRename 1.98i，2004–2007 年的 Delphi 老软件）** 在 64 位 Windows 上恢复
「右键 → 拖把更名器」的菜单项。

原版自带的 `xToolsEx.dll` 是 **32 位** shell 扩展。64 位资源管理器（`explorer.exe`）是 64 位进程，
加载不了 32 位扩展，所以 64 位系统上这个菜单根本不出现（或在 Win7 上点了没反应）。

本仓库提供一个**独立实现** `xToolsEx64.dll`：新文件名、新 CLSID，与原版 DLL 并存、互不干扰，
不需要修改或替换原软件的任何一个文件。

| | 原版 | 本仓库 |
|---|---|---|
| 文件名 | `xToolsEx.dll` | `xToolsEx64.dll` |
| 位宽 | 32 位 | 64 位 |
| CLSID | `{B0AA8F05-5A43-411D-8D41-09C3299F0B60}` | `{9E7B4C21-3A55-4F0E-8D1A-2B3C4D5E6F70}` |
| handler 键 | `xToolsExtension` | `xToolsEx64`（32 位镜像 `xToolsEx86`） |
| 适用 | 32 位 Explorer | 64 位 Explorer |

## 交接方式：完全复刻原版协议

逆向原版 32 位 DLL 后确认，它加入文件列表的方式是：

1. 把选中的完整路径逐行写入 `%TEMP%\~XTLS50.tmp`（CRLF 换行，ANSI / 当前代码页）；
2. 用命令行 `"xTools.exe" -a` 启动主程序，主程序在 `-a` 模式下读取该文件并加入列表。

本扩展照这个协议投递，所以不需要任何猜测或私有开关。点击后的完整动作是：

```
写 %TEMP%\~XTLS50.tmp  →  启动 "xTools.exe" -a  →  等主窗口出现  →  把窗口带到前台
```

**已证实无效、不再尝试的方式**：`WM_DROPFILES`、`WM_COPYDATA`、
命令行直接传路径、OLE 拖放模拟。原因都记在 [`docs/技术笔记.md`](docs/技术笔记.md)。

## 安装

1. 准备好原版程序（见下方「原版程序从哪来」），解压到任意目录，例如
   `D:\tools\SmartRename1.98i\`。
2. 把 `release\xToolsEx64.dll` 复制到 **`xTools.exe` 所在目录**。
   （放别处也能用，安装脚本会问你 `xTools.exe` 的路径。）
3. 右键 `release\安装.bat` → 以管理员身份运行（UAC 会自己弹，点是）。
4. 装完如果菜单没立刻出现，注销一次，或在任务管理器里重启 `explorer.exe`。
5. 右键任意文件 / 文件夹 / 驱动器 / 文件夹空白处，应出现「拖把更名器」。

> 注册的是 `HKLM`（机器级），因为 Everything、Directory Opus 等第三方文件管理器只读机器级注册，
> 只写 `HKCU` 的话在它们里面看不到菜单。因此**安装需要管理员权限**，这是必要的代价。

想知道安装到底写了哪些键、五个 base 分别对应哪种右键场景、一次点击的完整链路，
看 [`docs/安装图示.md`](docs/安装图示.md)。

## 卸载

右键 `release\卸载.bat` → 以管理员身份运行。它会注销 COM 注册、删掉 5 处 handler 键、
清理 `HKCU\Software\xTools\ShellEx64` 配置。

卸载后如果菜单还残留在某个第三方文件管理器里，重启该程序（或重启 Explorer）即可——
shell 扩展的菜单是各宿主进程在启动时枚举的，它有缓存。

## 原版程序从哪来

原版「拖把更名器」已停止维护，官网 `aorland.51.net` 基本只剩存档。国内还能下到的地址：

- 华军软件园：<https://www.onlinedown.net/soft/1665.htm>

第三方下载站普遍带捆绑安装器，**装的时候把所有勾选项都取消掉**；下完建议核对文件大小：
`xTools.exe` 应为 **381,952 字节**（PE32，10 个节）。对不上就别用。

本仓库**不包含**原版程序的任何文件，只包含 64 位扩展。

## 首次运行 xTools.exe 会弹窗？

> 现象：第一次或每次双击 `xTools.exe` 都弹一个框，内容大意是注册 `xToolsEx.dll` 失败。

原因：`xTools.exe` 启动时会尝试用 `regsvr32` 注册自带的 32 位 `xToolsEx.dll`。非管理员权限下
写 `HKLM` 会被拒绝，注册失败它就弹窗提示。这是原软件的行为，与本扩展无关。

**解法**：用管理员权限运行一次 `xTools.exe`（右键 → 以管理员身份运行），让它把注册做完；
之后用普通权限启动就不会再弹了。

## 配置（可选）

注册表 `HKCU\Software\xTools\ShellEx64`：

| 值 | 默认 | 说明 |
|---|---|---|
| `MenuText` | `拖把更名器` | 右键菜单显示的文字 |
| `HelpText` | `添加到拖把更名器的文件列表` | 状态栏提示文字 |
| `ExePath` | 空 | 手动指定 `xTools.exe` 完整路径。改了 exe 文件名时必须设它 |
| `IconFree` | `1` | 诊断开关。设 `0` 可关闭菜单位图的释放逻辑（怀疑图标引起崩溃时用） |
| `Method` | `args` | **已废弃**，仅为兼容旧配置保留；投递已固定为「临时列表 + `-a`」 |
| `LastWarn` | — | 安装脚本写入的非致命警告，正常应为空 |

`MenuText` / `HelpText` 改完立即生效（下次右键就按新文字插入），不需要重装。

## 常见问题

**Everything / Directory Opus 里看不到菜单**

- Everything 1.4 是 32 位程序，需要 32 位版本的扩展（见下方「32 位版本」）；Everything 1.5 起有 64 位版。
- Directory Opus 需要在「设置 → 其它 → 高级」里开启 shell 扩展菜单（`Shell extensions` 相关项），
  它默认不显示第三方 shell 扩展。

**菜单有了但没图标**

本扩展同时请求大/小两种图标，并对「24bpp 无 alpha 的老式图标」做了透明度重建——
`xTools.exe` 里的图标资源只有一张 32×32、24bpp、没有 alpha 通道，某些 GDI 实现下会被整张画成全透明。


**为什么 `.ps1` 里全是英文**

PowerShell 5.1 读无 BOM 的 UTF-8 文件时按 GBK 解码，中文会把后面的引号一起吞掉，
直接报一堆假的语法错误（`MissingCatchOrFinally`、乱码文本）。为了在任何机器上都能跑，
脚本正文全 ASCII，只保留必要的中文提示——那些中文用 Unicode 码位拼出来，不经过文件编码。

## 自己编译

Windows 上用 MSYS2（MINGW64 终端）：

```bash
pacman -S --needed mingw-w64-x86_64-gcc make
cd src
x86_64-w64-mingw32-g++ -std=c++11 -Os -fno-rtti \
  -ffunction-sections -fdata-sections -shared \
  -o xToolsEx64.dll xToolsEx64.cpp xToolsEx64.def \
  -Wl,--gc-sections,--strip-all \
  -lole32 -luuid -lshell32 -lshlwapi -ladvapi32 -luser32 -lgdi32 \
  -static -static-libgcc -static-libstdc++
```

Linux 上交叉编译同理（`g++-mingw-w64-x86-64`）。

几条必须知道的约束：

- **不要加 `-fno-exceptions`**：源码里有 `try/catch`，加了编不过。
- 依赖必须保持这 7 个，不要引入新的：`ADVAPI32 / GDI32 / KERNEL32 / msvcrt / ole32 / SHELL32 / USER32`。
  多一个依赖就意味着某些机器上加载失败、右键菜单静默消失。
- `-Os` + `--gc-sections` + `--strip-all` + 静态 libstdc++ 是为了控制体积。曾经用 `std::to_wstring` 拼后缀，
  它把 libstdc++ 的数字格式化整套拉进来，白多 50 KB；现在改用 `wsprintfW`。

验证产物：`objdump -p xToolsEx64.dll | grep "DLL Name"` 应只列出上面 7 个，
导出应为 `DllCanUnloadNow / DllGetClassObject / DllRegisterServer / DllUnregisterServer`。

## 32 位版本（给 Everything 1.4 之类）

同目录放一个 `xToolsEx86.dll`，安装脚本会自动补写 32 位视图的注册（同一 CLSID，靠
`KEY_WOW64_32KEY` 分开）。构建方式相同，把编译器换成 32 位的：

```bash
pacman -S --needed mingw-w64-i686-gcc
i686-w64-mingw32-g++ ...（参数同上，输出 xToolsEx86.dll）
```

目前仓库里还没有 32 位产物，这一条是给需要的人留的路。

## 目录结构

```
xToolsEx64/
├─ README.md              本文件
├─ CHANGELOG.md           改动的完整记录
├─ LICENSE                许可文件
├─ docs/
│   ├─ 安装图示.md         五个 base / 四个视图 / 一次点击的链路，一页看懂
│   └─ 技术笔记.md         逆向结论、已否决的方案、踩过的坑
├─ src/
│   ├─ xToolsEx64.cpp     全部实现（单文件）
│   └─ xToolsEx64.def     导出表
└─ release/               直接可用的成品：DLL + 安装卸载脚本
    ├─ xToolsEx64.dll
    ├─ install-xToolsEx64.ps1
    ├─ uninstall-xToolsEx64.ps1
    ├─ 安装.bat
    └─ 卸载.bat
```

## 说明与免责

- 只支持 **64 位** Windows（Win7 x64 / Win10 / Win11）。32 位系统继续使用原版 `xToolsEx.dll` 即可。
- 本扩展不修改原软件的 exe / dll，不改变它的任何功能，只是把「右键 → 加进文件列表」这条路补上。
- 「拖把更名器」版权归原作者 Torble 所有。本仓库只包含 64 位扩展代码，不包含原版程序的任何文件。

## 许可证

- 本仓库中的 `xToolsEx64.cpp` 源代码及编译后的 `xToolsEx64.dll` 均采用 **MIT License** 开源许可证。
- 允许个人或商业用途，允许复制、修改、再发布及二次开发，但需要保留原有版权声明和许可证声明。
- 许可证详见仓库根目录下的 [`LICENSE`](LICENSE) 文件。
- **注意：** MIT License 仅适用于本仓库中由本项目作者提供的代码及编译文件，不适用于原版「拖把更名器」及其相关文件。原软件的版权和使用许可仍归原作者所有。
</content>
</type>
</parameters>
</invoke>
