# HANDOFF — cst-isp-flasher

给下一个接手的人（或 AI）：读完这份就能知道**做成了什么、改过什么、踩过什么坑、环境怎么配、哪些规矩不能破**。不必先翻整个 `TPS_FxxK_Remake`。

| 项 | 值 |
|---|---|
| 更新日期 | 2026-09-09 |
| 当前软件版本 | **v1.2**（窗口左下角，与右下角 `Author: ALanStewart` 同一行） |
| 唯一源码 | `D:\dev\cst-isp-flasher\cst_isp_flasher` |
| Git 仓库 | `https://github.com/ALanStewart47/cst-isp-flasher`（Private） |
| 分支 | `feature/initial-commit`（v1.0 已提交并打本地标签；**v1.1 / v1.1.1 / v1.2 源码尚未 commit / 未打 tag**） |
| 可执行文件名 | `isp_flasher.exe`（CMake target 仍叫 `cst_isp_flasher`） |

配套文档（本目录）：

| 文件 | 用途 |
|---|---|
| **`docs/KEY_ISSUES.md`** | 真机联调踩坑，超时 / CRC / 读不到版本先对号入座 |
| **`docs/PROTOCOLS.md`** | MCU / FPGA / CH9121 字节级协议 |
| `README.md` | 验收步骤、发布打包细节 |
| `AGENTS.md` | 给 AI 的硬约束（改代码前必读） |
| `pack_release.ps1` | 一键打便携发行包（不含源码） |

仓库根目录还有 `LEARNING_GUIDE.md`（读源码教案），节号以本文当前目录为准。

同 Git 仓库里还有 `fieldup/`（客户升级包工具），**不是本软件**，不要混进本工具的协议或 UI。

**维护约定**：对话里出现「测试没问题 / 项目先这样 / 搞好了 / 暂时结束」等收尾口吻时，接手的人（或 AI）应就地更新本文第 1–4 节和第 11 节，并改文首日期。不要另起一份 HANDOFF。

---

## 1. 完成了什么

这是 Windows 桌面**工厂/现场** Bootloader 升级上位机（Qt 6），用来给控制器刷 MCU 和 FPGA，不是通用串口助手。

| 页签 | 协议 | 写到哪里 |
|---|---|---|
| MCU 升级 | CST-ISP（`72 68` / `72 69` + XOR8） | MCU 内部 Flash |
| 安路 FPGA 升级 | FA/FB + Modbus CRC16（大端） | 外部 W25Q128 @ `0x0C0000` |
| 高云 FPGA 升级 | 同一套 FA/FB 线格式 | FPGA 内部 Flash（Boot 走 JTAG） |

**v1.1 起**：串口 / 网口连接在三个页签**上方共用**（一套 COM+波特率+打开，或一套网卡+扫描+设备表+TCP 6600）。页签里只剩协议操作（固件、版本、FPGA 触发/烧录波特率、开始/取消、日志）。提示文案是「先连上控制器再选升级页」。CH9121 扫描器仍是进程内单例（UDP 50000 不能双绑）。同一时刻只允许一种升级在跑。

已经做成、不要当「还没做」去重写的能力：

- 2026-09-09 MCU「等待上电烧录」状态栏按**当前通道 + 协议应答**提示，不再把 COM/`isOpen()` 或「TCP 曾经连上过」写成「已连接」。用户已确认提示清楚。细则见第 2 节。
- 2026-09-07 新增 MCU 网口「等待上电烧录」：选设备/固件后可离线启动，自动连接固定 IP/端口，直接发总包数握手；收到 `02` 后留 150ms 再走原有数据流程。等待时允许取消，握手前重连，写入掉线失败；全部包确认后的复位断网仅跳过版本读取。确认框弹出前会冻结已选 ID/IP/端口（或串口），避免扫描改选。MCU 源码未改，2s 窗口待真机验收。操作说明见 README 的同名章节。
- 网口设备表：0 台禁用操作；1 台自动选中；2 台及以上清空选择，必须点行。IP 只取 `selectedRows()`，不取 `currentRow()`。连接栏 TCP 提示只显示**连接状态 + 设备 ID**（不带 IP:端口），以免长 ID 把窗口撑歪。

- 全异步、UI 不卡死：串口/TCP 走信号，状态机用 `QTimer`，可随时取消。
- 每步超时 + 有限重试；RX 累积缓冲按帧解析。
- MCU / FPGA 协议层与 GUI 分离，带 Qt Test（Mock 传输，不需要真串口）。
- 浅色工厂 UI：标题「Bootloader 升级工具」，主色薄荷绿 `#0ca678`；目标页 MCU / 安路 / 高云是分段条（安路 `#0b7285`、高云 `#f08c00`），连接方式 **串口 / 网口** 是同一套灰底分段条。
- 六个 Controller 仍在（MCU/安路/高云 × 串口/网口），但共用一条 `IspSerialPort` 和一条 `IspNetworkTransport`；切页/切模式时 `setRxEnabled`，只有当前页+当前通道收 RX，避免协议串扰。
- 已从 `TPS_FxxK_Remake` **独立成 GitHub 私有仓**，本目录自包含（CMake + 源码 + 测试 + 打包脚本）。
- 窗口页脚左下角 **v1.2**（双击弹出彩蛋1 图片）；右下角 Author 连点 8 次会弹出致谢（不要改回普通 `QLabel`）。
- 选固件：三页按钮仍弹 `QFileDialog`；也可把文件/文件夹拖到窗口，按**当前页**校验（MCU / 安路 `.bin`，高云 `.bin` / `.fs`）。读文件、空文件、过大、高云超注释上限警告都走 `FirmwareFile`。升级进行中不能换固件。
- 发行包脚本可用：解压即用，不含源码。当前便携包：`release/isp_flasher-v1.2-win64.zip`。上一份现场包已备份为 `backup/isp_flasher-v1.1.1-win64.zip`（另有 `.7z`）。

**不是什么**

- 不是新一代 `upper_upgrade`（CRC32 / 槽位 / W5500）。两套协议互不兼容。
- 不是 MCU / FPGA 固件工程。固件仍在原仓库 `00_Reference/legacy/`，本仓库只放上位机。
- 不是给客户自助升级的 `fieldup`。

---

## 2. 最近修改了什么

**v1.2（2026-09-09，已打便携包，源码仍未 commit）**：拖放选固件、页脚 `v1.2`、TCP 状态只显示 ID、双击版本号弹出彩蛋1。用户已看过拖放入口和彩蛋图，并要求发布安装包。

| 位置 | 改动 |
|---|---|
| `FirmwareFile.*` / `tests/FirmwareFileTest.cpp` | 拖放路径解析与读文件校验从窗口拆出；按钮和拖放共用。 |
| `IspMainWindow.*` | 整窗接受拖放（按当前 MCU / 安路 / 高云页）；日志框 `setAcceptDrops(false)` 以免截走文件。双击页脚 `v1.2` 弹出彩蛋1 图片。 |
| `resources.qrc` / `easter/egg1.png` | 编入彩蛋1（约 622KB PNG）。源码目录另有一份用户放的 `彩蛋1.png`，资源用的是 `easter/egg1.png`。 |
| `NetworkDeviceTarget.h` / `IspMainWindow.cpp` | TCP 状态旁只显示连接状态和设备 ID，不再带 IP:端口；标签 `Ignored` 水平策略，避免撑宽窗口。 |
| `pack_release.ps1` / `readme.txt` / `README.md` / `AGENTS.md` | 版本改为 `v1.2`。 |
| `release/isp_flasher-v1.2-win64.zip` | 2026-09-09 按用户「发布 v1.2」打便携包（含 `platforms/qwindows.dll`）。v1.1.1 zip/7z 已挪到 `backup/`。 |

**2026-09-09（未升版本号，已并入 v1.2 工作区）**：修 MCU 等待上电状态栏错乱。用户验收「显示效果不错」。仍未 commit。

| 位置 | 改动 |
|---|---|
| `IspUpgradeController.cpp` | 掉线立刻回到「正在连接」；没收到协议应答不写「已连接」。网口通道开了只写「正在查询版本，等待设备应答」，串口写「等待设备应答」。收到 App F0 才写「已连接，请断电再上电」；Boot `src==02` 才写「已进入 Bootloader」。30s 无应答计时仍跨 TCP 重连累计，收到 F0 后停。重连后恢复 WaitingReboot/Handshake，不把「请断电」打回「查询版本」。 |
| `NetworkDeviceTarget.h` / `IspMainWindow.cpp` | `tcpLinkStateForUi()`：等待上电时顶栏 TCP 跟当前 socket（已连通 / 连接中 / 尚未连上），不再一律「等待上电，自动连接」。 |
| `tests/IspProtocolTest.cpp` | 补串口/网口状态文案、掉线回退、重连保 App 提示、TCP 状态跟 socket 的单测。 |
| `docs/PROTOCOLS.md` | 等待上电状态文案与上面一致。 |

**v1.1.1（2026-09-08）**：页脚/打包版本改为 `v1.1.1`，按用户「就这样发布」打便携包。仍未 commit、未打 tag。

| 位置 | 改动 |
|---|---|
| `IspMainWindow.cpp` | 页脚版本 `v1.1` → `v1.1.1`。 |
| `pack_release.ps1` / `readme.txt` / `README.md` | 发行版本与 zip 名改为 `v1.1.1`。 |
| `IspMainWindow.*` / `NetworkDeviceTarget.h` | 多设备必须点选；等待上电烧录在确认框前冻结目标；TCP 状态显示已选设备。 |
| 同上 | 目标页分段条 + 串口/网口灰底分段条；主色 `#0ca678`；去掉窗口级字体 QSS（缺字体时整树重绘会卡）。 |
| `IspUpgradeController.*` / `FpgaUpgradeController.*` | `setState` 相同状态不重复 emit。 |
| `.gitignore` | 补 `.cmake/` `.qt/` `.qtc/` `*_autogen/`，避免源码目录误配再提交。 |
| 源码目录清理 | 删掉过时 `build-learn/`、`build-mcu-net/`，以及源码根目录误配的 CMake/Qt 缓存。留下 `build/`、`build-release/`、`release/`。 |

v1.1（2026-09-07）方案仍是「连接上置、三页共用」。v1.0 标签在本地。v1.1 / v1.1.1 / v1.2 **都还没 commit、没打 tag**。

未改、也不该顺手改的：

- MCU「读版本」文案和 `QLabel#version` 样式。
- 协议字节、状态机步骤、波特率默认值（MCU 19200；安路触发 19200 / 烧录常 115200 仍在 FPGA 页上，没有藏进二级菜单）。
- 六个 Controller 类本身：共用的是传输，不是把 MCU/FPGA 状态机合成一个。

---

## 3. 遇到什么问题

按「以后最容易再踩」排序。联调细节以 `docs/KEY_ISSUES.md` 为准。

### 3.1 Qt Creator 还在编旧目录（会表现为「代码改了界面没变」）

`.qtcreator/CMakeLists.txt.user` 里构建目录曾指向、并且**经常被 Qt Creator 写回**：

`D:\OneDrive\TPS_FxxK_Remake\00_Reference\legacy\tools\cst_isp_flasher\build\Desktop_Qt_6_5_3_MinGW_64_bit-Debug`

那份 CMake cache 的 `CMAKE_HOME_DIRECTORY` 是 OneDrive 旧源码（停在 **v1.0**，没有共用连接栏）。Cursor 改的是 `D:\dev\cst-isp-flasher`，按 Qt Creator 的 Run 却启动旧 exe。

处理：打开 **`D:\dev\cst-isp-flasher\cst_isp_flasher\CMakeLists.txt`**，Kit 必须是 **6.5.3 MinGW**（不要 6.6.2），构建目录必须是本仓库下例如 `...\cst_isp_flasher\build\Desktop_Qt_6_5_3_MinGW_64_bit-Debug`。可用 `CMakePresets.json` 的 `qt653-mingw-debug`。Application Output 里**不能出现** `OneDrive` 或 `legacy`。不要两头改。

### 3.2 `build-release` 带着旧工程的 CMakeCache

`pack_release.ps1` 第一次失败，报 cache 是在 OneDrive 路径生成的。清掉 `build-release/` 再跑脚本即可。从别处拷过来的 `build*` 目录不要直接复用。

### 3.3 发行包不能只拷 exe

Windows 上缺 `platforms/qwindows.dll` 或 MinGW 运行库会闪退。必须 Release 编译 + `windeployqt --compiler-runtime --no-translations`。

**MinGW 版不要加 `--release`**：插件没有 `d` 后缀，工具会当成 debug 全部跳过，启动报找不到 platform plugin。MSVC 版才用 `--release` / `--debug`。

### 3.4 看起来像上位机、其实是 Boot

安路 FPGA 日志出现 `RX FA FF FF FF FF 80 FD`：新 STM32 Boot 曾把 DMA 剩余计数存进 `unsigned char numb`，7 字节帧被算成长包后 CRC 失败。要改 **Boot 固件** 的 `numb` → `uint16_t` 并重烧 Boot，不是改上位机帧。板子若卡在 FPGA Boot（`0x08003400 = 0x55`），断电或清标志。

### 3.5 协议写错一次会全线失败

- FPGA CRC16：Modbus `0xA001` / `0xFFFF`，**MSB 先发**。
- `SetTotalSize` 参数是 **块数**，不是字节数。
- 写块地址是 **1-based 块索引**。
- `73` 在中断里立刻回，**不代表擦写完成**，必须 `FB FF FB` 轮询。
- MCU 目标版本必须 **≠ 当前版本**，否则只回版本、不进 Boot。
- `FpgaProtocol` 能解析 `CrcErrorAck`，但 Controller **还没按它失败**，界面可能只显示超时。日志有 RX、界面说超时，先对 hex。

### 3.6 通道和波特率

- 网口改不了波特率：`IspNetworkTransport::setBaudRate()` 是空操作，全程 19200。
- 安路 **直连 USART3**：触发 19200、烧录常 115200。把 115200 套到网口，写块会全无应答。
- 本系列 MCU App/Boot 都是 **19200**。README 里 G4 Boot 115200 是另一款产品，不要把 MCU 页默认改回 115200。
- 高云现用 App 若 USART0 改成串口屏，网口自动触发不会成功。

### 3.7 共用传输后协议会串扰

六个 Controller 都挂在同一条串口或同一条 TCP 上。若切页时没调 `bindActiveController()` / `setRxEnabled`，MCU 帧可能被 FPGA 状态机吃掉（或反过来）。升级过程中不要切页、不要切串口/网口（UI 已用 `sessionBusy()` 锁住）。

### 3.8 切到网口窗口被撑高

设备表曾经把窗口最小高度锁死，切回串口也缩不回去。现用 `CurrentPageStack` + 设备表固定高度 140 + 切网口前保存串口窗口尺寸。不要再给设备表无限增高。

### 3.9 Qt Creator Issues 里的 `appman-controller.exe`

`Process failed: The program "appman-controller.exe" does not exist` 是 Application Manager 插件在找车机工具，本工程用不到。不影响编译/运行。Help → About Plugins 关掉 Qt Application Manager，或确认 Run 目标是桌面 `isp_flasher`。`clazy-qstring-arg` 若仍指 838/1026，多半是旧诊断；那两行已无链式 `.arg()`。

### 3.10 源码目录被当成构建目录

Qt Creator 曾经在 `cst_isp_flasher/` 源码根目录 configure，留下 `.cmake/`、`CMakeFiles/`、`*_autogen/`、`isp_flasher.exe`。已删过时 `build-learn/`、`build-mcu-net/` 和这些缓存。日常用 `build/` 或 `release/` 里的 exe，不要在源码根再编一份。若根目录 exe 删不掉，先关掉正在跑的 `isp_flasher`。

### 3.11 等待上电状态栏把通道打开当成设备已连接

曾用 `m_sawPowerOnLink` 锁死 Linked：TCP 每 1.5s 拆线重连、日志已是「TCP 已断开」，页内仍写「已连接，正在查询版本」。串口只要 COM `isOpen()`（没板子也能开）同样立刻写已连接，且串口发的是总包数握手却写成「查询版本」。顶栏在 `powerOnWaiting()` 时一律「等待上电，自动连接」，和页内/日志对不上。

**不要改回去。** 没收到协议应答不能写「已连接」；掉线必须立刻回到「正在连接」。30s 无 F0 提示可以跨重连累计，但计时不要绑在 Linked 文案上。TCP 顶栏用 `tcpLinkStateForUi()` 跟当前 socket。

### 3.12 正在运行的 exe 会锁住源码目录的 `isp_flasher.exe`

源码根目录若被当成构建目录，链接阶段会 `Permission denied`。先关掉正在跑的窗口再编。Qt Creator 应用 `CMakePresets.json` 的 `build/Desktop_Qt_6_5_3_MinGW_64_bit-Debug/isp_flasher.exe`，不要和源码根那份混着点。

### 3.13 日志框会抢走拖放的固件

`QTextEdit` 默认 `acceptDrops`。三个收发日志已关掉拖放，文件才会落到主窗口按当前页加载。不要再给日志框开拖放。

---

## 4. 继续工作需要注意什么

1. **只在 `D:\dev\cst-isp-flasher` 改。** OneDrive / `TPS_FxxK_Remake` 里的旧 `cst_isp_flasher` 当只读对照，不要再当主工程。
2. CMake 源目录是 **`cst_isp_flasher/` 子目录**，不是 Git 仓库根。在该子目录执行 `cmake -S .` 或打开那里的 `CMakeLists.txt`。
3. 改协议：先改 `*Protocol.*` 和 `tests/`，再动 Controller，最后才动 UI。
4. 改 UI：只动 `IspMainWindow.*`。不要把 MCU「藏波特率」套到 FPGA 页；安路直连的触发/烧录双波特率不要藏进二级菜单。
5. 不要把 `TPS_FxxK_Remake` 的固件、硬件、客户 FPGA 拷进本仓。联调对照路径见第 9 节。
6. 不要提交 `build*` / `release/` / `*.user` / `*.zip`。发行包用脚本现打。
7. 需要 1.0 源码时用标签 `v1.0`，不要另建版本文件夹。v1.1 定稿后应在同一分支 commit + 打 `v1.1`（用户明确要求再做）。
8. 升版本时同时改 `IspMainWindow.cpp` 里的版本字符串（目前硬编码 `v1.2`，没有从 CMake `PROJECT_VERSION` 注入）。双击该标签会弹彩蛋图，不要改成不可点的纯展示。
9. 工厂电脑上不要把自定义波特率写入 QSettings。
10. 重编前先关掉正在跑的 `isp_flasher`。改完用 `ctest`（现含 `firmware_file_tests`）。
11. 仓库根目录的 `palette_review.png` / `palette_review.py` / `tab_design_proposals.py` 是界面试验草稿，**不是本工具源码**，不要编进 exe。
12. `fieldup` 与本工具同仓不同产品，发客户包走 `fieldup`，产线刷 Boot/FPGA 走本工具。
13. **不要把连接栏拆回三个页签。** 安路触发/烧录双波特率仍留在 FPGA 页，不要藏进二级菜单，也不要套 MCU 的「与连接相同」。
14. 不要拿掉 `setRxEnabled`，也不要把六个 Controller 合成一个「图省事」。
15. MCU 等待上电状态栏：通道打开 ≠ 设备在线。没有协议 RX 不要写「已连接」；串口等待上电不要写成「查询版本」。不要用 `m_sawPowerOnLink` 把掉线文案锁死。

---

## 5. 必须遵守的准则

摘自 `AGENTS.md`，违反会直接搞坏产线或编不过：

- 构建只用 **Qt 6.5.3 mingw_64**。Qt 6.10 官方包 **没有 SerialPort**。
- 工厂软件：不要加主题切换、账号系统、自动更新、多语言框架（除非产品明确要求）。
- MCU 页默认波特率 **19200**；「与连接相同」默认勾选。
- 网口（CH9121→USART1）App/Boot 都不能改波特率，固定 19200。
- FPGA 直连与 MCU 不同：安路触发 19200、烧录常 115200。
- 协议字节序、块索引、应答语义以本文件和 `docs/PROTOCOLS.md` 为准，**不要凭「常见 Modbus」改 CRC 或地址**。
- 不写密钥、不把客户固件打进 Git。
- 可见性：仓库保持 **Private**。帧格式、Flash 标志、CH9121 扫描包都是产线协议。
- 改协议或状态机：先补测试，再改 UI。`ctest` 必须绿。

UI 约定（不要无故改回去）：

- 浅色主题，主色 `#0ca678`；成功绿 / 失败红保持语义色，不要和主色混成一种绿。不要给串口/网口另配一套颜色，跟目标条一样用灰底分段。
- 文案是 **串口 / 网口**，不是「串口直连 / CH9121 网络」。
- 连接控件在页签**上方共用**；提示「先连上控制器再选升级页」。
- MCU：`Bootloader 波特率` 下拉 + 同一行 **与连接相同**（勾选时下拉禁用并跟随连接波特率）。
- 页脚右下角 Author 是可点的扁平按钮（彩蛋），不要改回 `QLabel`。

---

## 6. 开发环境

本机已验证路径（Windows）：

| 项 | 值 |
|---|---|
| OS | Windows 10/11 |
| Qt | **6.5.3 mingw_64**：`D:\Qt\6.5.3\mingw_64`（Widgets / SerialPort / Network / Test） |
| 编译器 | MinGW 11.2.0：`D:\Qt\Tools\mingw1120_64` |
| 生成器 | Ninja：`D:\Qt\Tools\Ninja` |
| CMake | `D:\Qt\Tools\CMake_64\bin`（打包脚本用） |
| IDE | Qt Creator 18，Kit：**Desktop Qt 6.5.3 MinGW 64-bit** |
| 不要用 | Qt 6.10.x 官方包（无 SerialPort）；不要用 MSVC 那套 Kit 去对 MinGW 的 DLL |

### 6.1 命令行构建与单测

在 **`cst_isp_flasher` 目录**下：

```powershell
$env:PATH = "D:\Qt\Tools\mingw1120_64\bin;D:\Qt\Tools\Ninja;D:\Qt\6.5.3\mingw_64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="D:/Qt/6.5.3/mingw_64" -DBUILD_TESTING=ON
cmake --build build --target cst_isp_flasher
ctest --test-dir build --output-on-failure
```

Debug 日常开发可用 `build/`；发行必须 **Release**（见下）。

### 6.2 Qt Creator

1. File → Open → `D:\dev\cst-isp-flasher\cst_isp_flasher\CMakeLists.txt`
2. Kit 选 Desktop Qt **6.5.3** MinGW 64-bit（不要 6.6.2）
3. 构建目录必须在本仓库下，禁止仍指向 OneDrive / `TPS_FxxK_Remake`。优先用 `CMakePresets.json` 的 `qt653-mingw-debug`
4. 运行配置选 target `cst_isp_flasher`（生成 `isp_flasher.exe`）
5. 改完先编再 Run。Cursor 里改源码不会自动刷新已经打开的旧 exe。Application Output 路径必须在 `D:\dev\cst-isp-flasher` 下

`.qtcreator/` 已 gitignore。换机器或重新打开后若 Unconfigured，按上面 Kit 重新 Configure。`.user` 文件会被 Qt Creator 改回旧路径，改完要再核对一遍。

### 6.3 打发行版（不含源码）

```powershell
cd D:\dev\cst-isp-flasher\cst_isp_flasher
powershell -ExecutionPolicy Bypass -File .\pack_release.ps1
```

产物（均 gitignore）：

- 目录：`release/isp_flasher/`、`release/isp_flasher-v1.2/`
- zip：`release/isp_flasher-win64.zip`、`release/isp_flasher-v1.2-win64.zip`

脚本会：Release 配置 → 编 `isp_flasher.exe` → 拷到空目录 → `windeployqt` 收 Qt DLL / `platforms/qwindows.dll` / MinGW 运行库 → 从 `readme.txt` 写成 GBK 的发行说明 → 打 zip。

发给现场：只发 zip。对方解压双击 exe，不必装 Qt。不要只发 exe，不要把 `.cpp` / CMake / `docs/` 打进去。

若 configure 报 CMakeCache 路径不一致：删 `build-release/` 再跑。

---

## 7. 源码地图

```
main.cpp                     入口
IspMainWindow.*              顶栏共用连接 + 三页签协议 UI（QSS 在 buildUi()）
FirmwareFile.*               选固件：拖放解析 + 读文件校验（无 GUI）
easter/egg1.png              页脚版本双击彩蛋图（resources.qrc 编入）
IspUpgradeController.*       MCU 状态机；`setRxEnabled` 供共用传输
FpgaUpgradeController.*      FPGA 状态机（安路 / 高云 profile）；同样有 `setRxEnabled`
IspProtocol.*                MCU 协议：建帧 / XOR8 / 分包 / RX 解析（无 GUI）
FpgaProtocol.*               FPGA 协议：Modbus CRC16 / 建帧 / 分包 / 解析
IspTransport.h               IIspTransport（字节流）
IspSerialPort.*              QSerialPort
IspNetworkTransport.*        QTcpSocket（CH9121 透传；setBaudRate 空操作）
Ch9121Scanner.*              UDP 扫描，单例
W5500ConfigProtocol.*        CH9121 285 字节配置包（UDP 50000/60000）
CMakePresets.json            Qt Creator 预设：本仓库 Debug 构建目录
tests/                       Qt Test + Mock 传输（含 FirmwareFileTest）
pack_release.ps1             便携发行包
app_icon.rc / app.ico        Windows 图标
```

---

## 8. 协议速记（实现以代码和 PROTOCOLS.md 为准）

### MCU（CST-ISP，XOR8，默认 19200 8N1）

- 读版本：`72 68 BB 16` → `72 68 BB maj min 16`
- 触发：`72 68 AA 目标主 目标次 16`。目标必须与当前不同。
- 进 Boot：设备发 `72 68 01 16`（状态机不依赖这帧，settle 后发总包数，收到 `02` 就写）
- 数据包：`72 69` + 序号 + 256B + XOR8（只异或 payload）+ `16`

### FPGA（FA/FB + CRC16 大端）

- 自动触发：`FA 68 AA 00 00 <CRC>`（App @ 19200）→ `73`，写 `0x08003400 = 0x55`，复位进 FPGA Boot
- Flash 标志：`0x00AA` = MCU 升级，`0x0055` = FPGA 升级
- `73` ≠ 完成；轮询 `FB FF FB`：`00` 空闲 / `01` 忙则重发同一块 / `02` 校验失败
- 安路直连：触发 19200、Boot 烧录常 115200（USART3）；网络全程 19200
- 高云：无 `FA 68 01 16` beacon；默认 19200；末块后 MCU 可能复位，掉线在 settle 窗口内视为成功

### CH9121

- 扫描：UDP 广播，绑定所选网卡；数据 TCP 默认 **6600**
- 透传的是 UART 字节，TCP 改不了设备波特率

---

## 9. 本仓库没有、联调才会用到的东西

这些仍在 `TPS_FxxK_Remake`，**不要拷进 GitHub**：

| 路径（相对 TPS_FxxK_Remake） | 用途 |
|---|---|
| `00_Reference/legacy/software/CST-TPS481500C-S4-8T-MCU-V2.0/` | 安路机 MCU App，FPGA 触发 |
| `00_Reference/legacy/software/Boot_STM32_anlu/` | 安路机 MCU Boot，FA/FB |
| `00_Reference/legacy/BootLoader/` | 正在维护的新 STM32 Boot（含 RX 长度修复） |
| `00_Reference/legacy/software/BOOT_FPGA_anlu/` | 安路 FPGA 多启动 |
| `00_Reference/legacy/software/Boot_GD32F3-V1.1/` | 高云机 Boot（JTAG） |
| `00_Reference/legacy/tools/upper_upgrade/` | 另一套协议，不要混 |
| `00_Reference/legacy/tools/cst_isp_flasher/` | **旧拷贝**，不要再当主工程 |
| `04_Docement/fpga_upgrade_feature.md` | 上位机 FPGA 功能工作笔记 |
| `04_Docement/legacy_software_code_analysis.md` | 协议对照分析 |

芯片：TPS481500C = **STM32F103CBT6**，Flash 页 **1KB**。Keil 设备选成 F103xG 只是旧工程方便，不要改成 2048。

---

## 10. 验收怎么算过

协议层：第 6.1 节的 `ctest` 必须绿。

真机 MCU：

1. 顶栏选 **串口**，打开 19200 → 再进 MCU 页 → 读版本 → 选 `.bin` → 目标版本与当前不同 → 升级满进度 → 再读到新版本。
2. 顶栏选 **网口**：扫描 → TCP 6600 → 连上后再进 MCU 页，同样流程（全程 19200）。
3. 断线 / 迟回应超时失败，不能卡死或闪退。升级中切页/切通道应被拦住。

真机 FPGA：见 `README.md` 安路 / 高云两节。直连安路注意烧录 115200（在 FPGA 页上改，不在顶栏）；网络仍是 19200。

发行包：到没装 Qt 的机器，或临时把 `D:\Qt` 移出 PATH，解压后双击应能开窗；左下角为 **v1.2**，顶栏是共用连接（不是每页一套）。可点按钮或拖入固件。

---

## 11. 还没做完 / 不要擅自做的

- **v1.2 便携包已打好（2026-09-09）。** 发给现场用 `release/isp_flasher-v1.2-win64.zip`。v1.1 / v1.1.1 / v1.2 **源码仍未 commit、未打 tag**。用户要求再提交再做。
- MCU 等待上电状态栏用户已验收；不要改回「已连接，正在查询版本」。
- 拖放 / TCP 短状态 / 彩蛋图用户已看过；不要撤掉拖放或把 IP 写回 TCP 提示。
- 安路 / 高云 FPGA 页仍以真机验收为准（高云网口受 App USART0 接线限制）。
- `FpgaUpgradeController` 遇到 `CrcErrorAck` 仍只表现为超时，提示可以更明确。
- 应用版本号仍是 UI 硬编码 `v1.2`，未接入 CMake 工程版本。
- Qt Creator `.user` 仍可能把构建目录写回 OneDrive；每次 Run 前看 Application Output。
- 不要加用户登录、云升级、多语言、主题切换。
- 不要把 FPGA 的触发/烧录双波特率藏进二级菜单。
- 不要把连接栏拆回三页，也不要把六个 Controller 合成一个。
- 不要把本工具和 `fieldup` / `upper_upgrade` 合成一个安装包「图省事」。
