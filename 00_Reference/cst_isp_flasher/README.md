# Bootloader 升级工具（isp_flasher）

本目录就是工程根（CMake + 源码）。

- 交接 / 导出 GitHub：[HANDOFF.md](HANDOFF.md)
- 联调关键问题：[docs/KEY_ISSUES.md](docs/KEY_ISSUES.md)
- 各机升级协议：[docs/PROTOCOLS.md](docs/PROTOCOLS.md)

独立的 Qt 上位机，当前 **v1.2**。窗口顶部共用 **串口 / 网口** 连接（先连上控制器再选升级页），三个页签只做协议操作：

1. **MCU 升级**：旧 CST-ISP bootloader（`72 68`/`72 69` + XOR8）。串口直连，或 CH9121 网络透传到 USART1 @ 19200。
2. **安路 FPGA 升级**：FA/FB + Modbus CRC16，写外部 W25Q128；串口直连（USART3，Boot 115200）与 CH9121 网络透传（USART1 @ 19200）。触发/烧录波特率在本页，不在顶栏。
3. **高云 FPGA 升级**：同一套 FA/FB 线格式，Boot（`Boot_GD32F3-V1.1`）经 JTAG 写 FPGA 内部 Flash。
   串口 USART1（波特率档位，默认 19200）与 CH9121 网络 USART0（硬编码 19200）。

固件可用「选择固件」按钮，或从资源管理器拖到窗口：按**当前页**校验（MCU / 安路只要 `.bin`，高云 `.bin` / `.fs`）。拖文件夹时只接受里面恰好一个匹配文件。升级进行中不能换固件。

与 `tools/upper_upgrade`（新 CRC32/槽位协议）**完全独立**，互不影响。

## 网口等待上电烧录（MCU）

1. 设备通电时扫描。只有一台会自动选中；多台必须在列表里点选目标，按钮才会可用。
2. 选择 MCU `.bin` 固件（按钮或拖入当前页），点击「等待上电烧录」（无需预先连接 TCP，也无需更改目标版本号）。确认框会显示设备 ID 和 IP。
3. 给控制器断电再上电；软件自动连接选定 IP/端口，先查 Boot 版本（`72 68 F0 16`，须 Boot `src=02`），再握手烧录一次并退出等待。
4. 等待期间点击「取消」会停止连接和探测；写入期间掉线报失败，不自动续传。

适用于 `reference/stm32f103_bootloader_v1.2` 这类支持开机总包数握手、并能应答 F0 查询的 Boot。
连接超时 1000ms，失败后最多 200ms 再试；连上先发 F0，Boot 确认后再发总包数，每 250ms 重试，
连续 6 次无确认则重建 TCP。收到 `72 68 02 16` 后等待 150ms，再沿用现有分包流程。
App 仍在跑（F0 `src=01`）会继续等上电；Boot 答 `ver=00` 弹窗「不支持网口升级」，不切串口。
网口固定 19200。全部数据包已确认后，若设备复位断网，提示烧录完成、版本待重连读取。

此功能不修改 MCU 的 2 秒窗口；网络必须在窗口内恢复。错过窗口会继续等待下一次上电，
不会自动发送 App 触发帧。是否能稳定赶上窗口仍需真机测试。

## 设计要点（解决老工具三大毛病）
- **全异步、零阻塞**：串口/TCP 收发走信号驱动，状态机用 `QTimer` 驱动，UI 永不卡死。
- **每步超时 + 重试**：常规升级的握手/确认/数据使用有限重试；「等待上电烧录」在握手前持续等待，可手动取消。
- **RX 帧重组 + 边界保护**：累积缓冲按帧解析，越界先判长度 → 读版本/升级都不会崩。
- **随时安全取消**。

## 协议（旧 CST-ISP，UART 19200 8N1，IDLE 分帧，XOR8 校验）
| 阶段 | PC→设备 | 设备→PC |
|---|---|---|
| 纯读版本（App） | `72 68 BB 16` | `72 68 BB <主> <次> 16` |
| Boot 版本查询（仅 MCU 网口） | `72 68 F0 16` | `72 68 F0 <src> <ver> 16` |
| 触发升级（App） | `72 68 AA <目标主> <目标次> 16` | 同版本→回版本不升级；不同版本→重启进 Boot |
| 进入 Boot | — | `72 68 01 16` |
| 总包数 | `72 68 <总H> <总L> 16` | `72 68 02 16` |
| 数据包（262B，序号1起） | `72 69 <序H> <序L> <256B> <XOR8> 16` | `72 69 <序H> <序L> 16`（失败 `72 68 03 16`） |

升级数据写完后从机跳转新 App，工具自动再发 `72 68 BB 16` 读新版本验证。
> 进 Boot 的前提：**目标版本必须与控制器当前版本不同**（见控制器固件
> `software/MDPS24120C-4TDP_V0.1.3/Protocol/protocol_transfer.c` 的 `upguade_cmdhandle`）。

> **⚠️ App 与 Bootloader 波特率不同**：App 模式 19200，Bootloader 复位后是 **115200**
> （参考 `G4_BootLoader_V2.0/Core/Src/usart.c`）。工具自动在两者间切换：App 波特率发触发 → 切 Boot 波特率
> 收 `72 68 01 16` 并刷写 → 切回 App 波特率读新版本。UI「Bootloader 波特率」默认 115200，可改。
> 若一直收不到 `72 68 01 16`，先确认这个值与你的 bootloader 一致（勾「显示收发日志」看 RX 原始字节）。

## FPGA 升级协议（FA/FB + Modbus CRC16）

来源：
- App 触发：`CST-TPS481500C-S4-8T-MCU-V2.0/User_C/protocol_public.c` 的 `fpga_upguade_cmdhandle()`
- Bootloader：`Boot_STM32_anlu/MDK-ARM/APP/Src/protocol_public.c` 的 `CMD_40_handle()`
  与 `uart.c` 的 `uart_fpga_updata()`

- **CRC16**：Modbus（多项式 0xA001，初值 0xFFFF），**大端在前**（MSB 先发，与 MCU 一致）。
- **自动触发**：勾选「自动触发进入 FPGA 升级模式」后，工具先发
  `FA 68 AA 00 00 <CRC16>`（App 波特率，默认 19200）。App 收到后回 `73`、写 `0x55`
  到 Flash `0x08003400`、复位 FPGA 并系统复位，Bootloader 随即进入 FPGA 升级模式
  （发一次 `FA 68 01 16` 就绪信号）。工具自动切到 Bootloader 波特率后开始烧录。
  若设备已在 FPGA 升级模式，触发帧会被当作块大小 0 拒绝（回 `65`），工具直接开始烧录。
- **就绪信号**：MCU 进入 FPGA 升级模式时发一次 `FA 68 01 16`（仅提示，收到可提前开始烧录）。
- **设置块大小**：`FA 68 AA <blockH> <blockL> <CRC16>` → 应答 `73`（成功）/ `65`（非法）。
- **设置总块数**：`FA 00 <size[23:16]> <size[15:8]> <size[7:0]> <CRC16>` → 应答 `73`。
  注意：**参数是块数，不是字节数**；MCU 收到后自动擦除 W25Q128 12×64KB @ 0x0C0000。
- **写数据块**：`FA <addr[31:24]> ... <addr[7:0]> <data[N]> <CRC16>` → 应答 `73`。
  addr 为 **1-based 块索引**，MCU 映射到 Flash 偏移 `(addr-1)*blockSize + 0x0C0000`。
- **状态轮询**：PC 发 `FB FF FB` → MCU 回 `FB 00 FB`（空闲）/ `FB 01 FB`（忙）/ `FB 02 FB`（校验失败）。
- **时序要点**：MCU 的 `73` 在中断里立即回（不代表处理完成），随后主循环擦除/写 Flash；
  忙时收到的数据帧会被丢弃。因此 SetTotalSize 后必须轮询到 `FB 00 FB` 再发第一个块，
  块与块之间若收到 `FB 01 FB` 需重发同一块。最后一块确认后 MCU 自动发 FPGA 多启动命令并复位。

## 源码结构
- `IspProtocol.*` — MCU 协议纯逻辑层（建帧 / XOR8 / 分包 / RX 成帧解析），无 GUI/串口依赖。
- `FpgaProtocol.*` — FPGA 协议纯逻辑层（Modbus CRC16 / 建帧 / 分包 / 响应解析）。
- `IspTransport.h` — 字节级传输接口（`IIspTransport`）。
- `IspSerialPort.*` — `QSerialPort` 实现（MCU 升级与 FPGA 串口直连共用）。
- `IspNetworkTransport.*` — `QTcpSocket` 实现（CH9121 透传，全异步）。
- `Ch9121Scanner.*` / `W5500ConfigProtocol.*` — CH9121 UDP 扫描发现与配置
  （285 字节固定包，UDP 50000/60000，命令 0x04/0x02/0x01）。
- `IspUpgradeController.*` — MCU 升级状态机，依赖 `IIspTransport`（串口/TCP 复用同一套）。
- `FpgaUpgradeController.*` — FPGA 升级状态机，依赖 `IIspTransport`（串口/TCP 复用同一套）。
- `FirmwareFile.*` — 选固件共用：拖放路径解析、扩展名、空文件/过大校验。
- `IspMainWindow.*` / `main.cpp` — 顶栏共用连接 + 三页签协议 UI（MCU / 安路 FPGA / 高云 FPGA）。
- `tests/IspProtocolTest.cpp` / `tests/FpgaProtocolTest.cpp` / `tests/FirmwareFileTest.cpp` — Qt Test，Mock 传输跑完整流程
  （无需真串口/网络）。

## 构建（需 Qt6 Widgets + SerialPort + Network）
```
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.5.3/mingw_64
cmake --build build
```
Qt Creator 用本目录 `CMakePresets.json` 的 `qt653-mingw-debug`，构建目录必须在本仓库下，不要编到 OneDrive / `legacy` 旧拷贝。
> 协议层单测只需 `Qt6::Test`：`cmake -DBUILD_TESTING=ON ...` 后 `ctest`。
> 注意：Qt 6.10 官方安装包**不含 SerialPort 模块**，本机用 Qt 6.5.3（模块齐全）构建。

## 发布便携版（解压即用，不含源码）

Windows 上 **不能只拷贝 exe**。Qt 程序还依赖一堆 DLL 和插件（尤其是 `platforms/qwindows.dll`），缺了双击会直接闪退。

本工程一键打包：

```powershell
powershell -ExecutionPolicy Bypass -File .\pack_release.ps1
```

产物（已 gitignore，不入库）：

- 目录：`release/isp_flasher/`（当前包）和 `release/isp_flasher-v1.2/`
- 压缩包：`release/isp_flasher-win64.zip` 和 `release/isp_flasher-v1.2-win64.zip`

手工步骤（其它 Qt Widgets 工程同样适用）：

1. **Release 编译**（不要用 Debug：Debug 的 exe 配不上 Release 的 Qt DLL）
   ```
   cmake -S . -B build-release -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.5.3/mingw_64 -DCMAKE_BUILD_TYPE=Release
   cmake --build build-release
   ```
2. 把 `isp_flasher.exe` 拷到空目录。
3. 用**同一套 Qt** 的 `windeployqt` 收集依赖（PATH 里要有 MinGW 的 bin，才能带上编译器运行库）：
   ```
   $env:PATH = "D:\Qt\Tools\mingw1120_64\bin;D:\Qt\6.5.3\mingw_64\bin;$env:PATH"
   windeployqt --compiler-runtime --no-translations isp_flasher.exe
   ```
   > MinGW 版 Qt 不要加 `--release`：插件没有 `d` 后缀，工具会把它们误判成 debug 并全部跳过，报 `Unable to find the platform plugin`。MSVC 版才用 `--release` / `--debug`。
4. 把整个目录打成 zip。目标电脑解压后双击 exe，**不必装 Qt**。

常见翻车：

- Debug 的 exe 配了 `--release` 的 DLL，或反过来 → 启动即崩。
- 漏了 `platforms/qwindows.dll` → 提示 “This application failed to start because no Qt platform plugin could be initialized”。
- 漏了 MinGW 的 `libstdc++-6.dll` / `libgcc_s_seh-1.dll` / `libwinpthread-1.dll` → 缺 DLL 报错。`--compiler-runtime` 就是干这个的。
- 用了 Qt 6.10 官方包 → 没有 SerialPort 模块，本工具编不过。继续用 6.5.3。
- 只发 exe、不发旁边的 dll/plugins 文件夹。

验证：到一台没装 Qt 的电脑，或临时把 `D:\Qt` 从 PATH 里拿掉，解压后双击应能开窗。

## 高云 FPGA 升级（FA/FB + JTAG，GD32 Boot）

来源：
- App 触发：`CST-BPS24200CIP6-6TDP-MCU-V1.6_origin` 的 `upfpga_cmdhandle()`（`FA` 先回 `73`，`68 AA` 则 `BOOT_RST(0x55)`）。
- Bootloader：`Boot_GD32F3-V1.1` 的 `CMD_40_handle()` + `program_internal_flash()`（JTAG `programXPage`）。

与安路的差异（上位机按源码处理，不照抄安路文案）：

- 线格式仍是 FA/FB + CRC16 大端；块大小 256..2048、总块数、1-based 写块、忙时 `FB 01 FB` 均相同。
- **落盘是 FPGA 内部 Flash（JTAG）**，不是 W25Q128。末块后 Boot 发 `0x3A/0x3C` 再复位 MCU。
- 高云 Boot **不发** `FA 68 01 16` beacon；工具用设块大小命令探测就绪。
- 串口默认触发=烧录=**19200**（Flash `0x08003402` 是档位索引，3=19200）。网口 Boot USART0 **固定 19200**。
- 末块 `73` 只表示已收下；工具再等约 2.5s（期间通道掉线视为成功），避免 MCU 复位被判失败。
- 文件可选 `.bin` / `.fs`。超过 Boot 注释上限 `0x1B200`（约 108.5KB）会警告但仍下发；硬上限 512KB。

**网口限制（事实）**：当前目录 `CST-BPS24200CIP6-6TDP-MCU-V1.6` 把 USART0 改成串口屏 115200，不进主机协议，**网口自动触发不会成功**。网口双通道以 `V1.6_origin`（USART0=CH9121@19200）为准。新面板机请用串口触发。

## 整机验收（MCU 升级）
1. **串口直连**：顶栏选「串口」，打开串口（19200）→ 切到 MCU 页 →「读版本」显示当前版本。
2. 选对应 `.bin`、设**不同**目标版本 →「开始升级」→ 进度跑满 → 成功弹窗 → 自动显示新版本。
3. **CH9121 网络**：顶栏选「网口」→ 选网卡 →「扫描设备」→ 选中设备 →「连接设备」（TCP 默认 6600）→
   再进 MCU 页读版本 / 开始升级。网口 App 与 Boot **均固定 19200**（CH9121 透传 USART1，TCP 不能改波特率）。
4. 故意断线/控制器迟回 → 工具超时报错而非卡死/闪退；升级后反复「读版本」不崩。升级进行中不能切页、不能切串口/网口。

## 整机验收（安路 FPGA 升级）

1. **串口直连**：顶栏打开串口 → 安路页把烧录波特率设为 115200（触发仍 19200）→ 选 FPGA `.bin`（≤768KB）→
   「开始升级」（默认勾选自动触发）→ 工具发触发帧 → MCU 自动复位进升级模式 → 进度跑满 → 成功弹窗。
2. **CH9121 网络**：顶栏选网卡 →「扫描设备」→ 选中设备 →「连接设备」（TCP 端口默认 6600）→
   切到安路页开始升级（触发与烧录均走网络透传，波特率默认 19200）。
3. 升级完成后 MCU 自动重启 FPGA，无需额外操作。
4. 不勾「自动触发」时，假定控制器已在 FPGA 升级模式，直接按烧录波特率开始。
5. 控制器未在运行/波特率不对 → 工具超时报错而非卡死。

## 整机验收（高云 FPGA 升级）

1. **串口直连**（USART1，默认 19200）：顶栏打开串口 → 高云页选 `.bin`/`.fs` → 勾选自动触发 → 开始升级。
2. **CH9121 网络**（仅 `V1.6_origin` / USART0=CH9121@19200）：顶栏扫描 → 连接 TCP 6600 → 切到高云页同上。新面板 V1.6 网口触发应失败（USART0 是串口屏）。
3. 日志应出现 JTAG 擦除文案，而不是 W25Q128；末块后会短暂等待再报成功。
4. 默认块大小 256、触发/烧录波特率 19200。
