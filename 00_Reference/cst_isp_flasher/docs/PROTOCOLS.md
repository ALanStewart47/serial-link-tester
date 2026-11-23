# 升级协议说明（MCU / FPGA）

本文是 `cst_isp_flasher` 的协议源。实现分别在 `IspProtocol.*`、`FpgaProtocol.*`。踩坑见 [KEY_ISSUES.md](KEY_ISSUES.md)。

这里只写 **升级**。应用层配置（灯光/相机、BCC 命令 `31`/`32` 等）在父仓库 `04_Docement/legacy_software_code_analysis.md`，本工具不实现。

---

## 0. 先看哪一台

| 产品 | MCU | FPGA | 上位机页签 |
|---|---|---|---|
| TPS481500C 系列 | STM32F103CBT6 | 安路 EG4，Bitstream 进 **W25Q128** | MCU + **安路 FPGA** |
| BPS24200CIP6 等 | GD32 | 高云，Bitstream 进 **FPGA 内部 Flash（JTAG）** | MCU + **高云 FPGA** |
| 新维护的 STM32 Boot | 同一颗 F103CBT6 | 线格式同 FA/FB，落盘看该 Boot 编进去的后端 | 同上，通道多半是 USART1 @ 19200 |

MCU 升级线格式各机相同（CST-ISP）。FPGA 线格式各机相同（FA/FB），**落盘、波特率、beacon、超时**不同。

另一套工具 `upper_upgrade`（CRC32 / 槽位 / W5500）与本文无关。

---

## 1. 公共约定

### 1.1 物理层

- UART **8N1、无流控**。一帧靠 **IDLE 空闲** 分界，没有长度字段、没有固定结束符（MCU 数据帧的 `16` 只是该协议自己的尾字节）。
- 必须一次发完整帧。中间出现空闲，对端会切成两帧。
- 两路 UART 若共享一个 RX 缓冲，不要同时往两口发命令（上位机同时只开一条通道）。

### 1.2 通道

| 通道 | 实际路径 | 波特率 |
|---|---|---|
| 串口直连 MCU | PC → USART1 | 本系列 19200（App=Boot） |
| 串口直连安路 FPGA | PC → USART3 | App 触发 19200；**旧 Boot 烧录 115200** |
| 网口（三页共用同一条 TCP） | TCP → CH9121 → USART1 | **固定 19200**，TCP 改不了设备波特率 |

上位机 **v1.1** 只有一套串口和一套网口；切页不会另开第二条物理连接。

TCP 默认端口 **6600**。扫描见第 6 节。

### 1.3 升级标志（MCU Flash）

地址 **`0x08003400`**，半字：

| 值 | 上电 |
|---|---|
| `0x00AA` | 进 MCU 升级（CST-ISP） |
| `0x0055` | 进 FPGA 升级（FA/FB） |
| 其它 | 跳 App，向量/入口 **`0x08003800`** |

App 触发 MCU 升级：写 `0xAA` 后 `NVIC_SystemReset()`。  
App 触发 FPGA 升级：写 `0x55` 后复位（安路还会先复位 FPGA 多启动寄存器）。

失败后标志经常还在，板子会一直进 Boot。断电或把该半字清掉才能再走 App 触发。

`0x08003402` 在部分 Boot 里是 USART1 波特率档位（3 = 19200）。网口用的就是这路，被改过会和 19200 对不上。

---

## 2. MCU 升级（CST-ISP）

实现：`IspProtocol.*` + `IspUpgradeController.*`。  
固件对照：

- App：`CST-TPS481500C-S4-8T-MCU-V2.0` 的 `upguade_cmdhandle`；高云 `upguade_cmdhandle`（同格式）。
- Boot：`BootLoader` / `Boot_STM32_anlu` / `Boot_GD32F3-V1.1` 的 `ACK_cmd` / `BootLoaderCmd_handle`。

校验是 **XOR8，不是 CRC**。默认 19200。

### 2.1 帧一览

PC → 设备

| 名称 | 字节 | 说明 |
|---|---|---|
| 读版本 | `72 68 BB 16` | 无副作用 |
| Boot 版本查询 | `72 68 F0 16` | **仅 MCU 网口**，开始升级前查询；串口 / FPGA 不发 |
| 触发升级 | `72 68 AA <主> <次> 16` | 版本相同只回版本；不同则写 `0xAA` 并复位 |
| 总包数 | `72 68 <cntH> <cntL> 16` | `cnt` 为大端，等于 256 字节包个数 |
| 数据包 | `72 69 <idxH> <idxL> <256B> <XOR8> 16` | `idx` 从 **1** 起；XOR 只异或 256B |

设备 → PC

| 名称 | 字节 |
|---|---|
| Boot 就绪 | `72 68 01 16` |
| 总包数确认 | `72 68 02 16` |
| 校验失败（重发该包） | `72 68 03 16` |
| 版本 | `72 68 BB <主> <次> 16` |
| Boot 版本查询应答 | `72 68 F0 <src> <ver> 16`（`src`：`01` App / `02` Boot；`ver`：`(major<<4)\|minor`，`00` 为旧/未知） |
| 数据包确认 | `72 69 <idxH> <idxL> 16` |

末包不足 256B 时用 `0xFF` 补齐（按擦除态）。协议层包数上限 65535（约 16MB），具体芯片 Flash 另限。

### 2.2 XOR8

```
xor = 0
for b in payload_256:
    xor ^= b
```

**不要**把 `72 69 idxH idxL` 或尾 `16` 算进去。

### 2.3 各机 App 触发差异

**TPS481500C（安路机 App）** `upguade_cmdhandle`：

- 触发：`buf[1]==0x68 && buf[2]==0xAA && buf[5]==0x16 && length==6`
- `buf[3]==VERSION[0] && buf[4]==VERSION[1]` → `read_version()`，不复位
- 否则 `BootLoader_Reset()` → 写 `0xAA` @ `0x08003400`，复位
- 读版本：`72 68 BB 16`（length==4）

**高云 / BPS App** `upguade_cmdhandle`：

- 同样 `72 68 AA` / `72 68 BB`，用 `version[]` 比较
- 长度判定略松（按偏移扫），语义相同
- `BOOT_RST(0xAA)` 进 MCU Boot

**Boot 收到总包数**：`72 68 cntH cntL 16` → 记下 `package_sum`，回 `72 68 02 16`。  
部分 Boot 若再收到 `72 68 AA … 16` 会回 `01`（当成就绪），不应当成 App 触发。

### 2.4 上位机状态机（实际行为）

串口升 MCU 仍按下面 1–7。**MCU 网口**在触发前多两步 F0 查询（FPGA 网口不发）：

0. 连上后发 `72 68 F0 16`，等 500 ms。
   - 超时 / `ver == 00`：弹窗「不支持网口升级」，不切串口、不发 `AA`。
   - `src == 02` 且 `ver != 00`：已在 Boot，跳过 `AA`，直接发总包数。
   - 其它 `ver != 00`（通常 App `src == 01`）：再发触发帧。
1. App 波特率发触发 `72 68 AA 目标主 目标次 16`。
2. 若 Boot 波特率不同则立刻切换（网口不会真切）。
3. 等 `m_bootSettleMs`（约 1.5s），**不依赖**设备主动发 `01`。网口此时再发一次 F0，只认 `src == 02` 且 `ver != 00`；否则弹窗「不支持网口升级」，不发总包数。
4. 主动发总包数；收到 `02`（或 settle 期间就收到 `02`，仅串口）开始从第 1 包写。
5. 期间若收到版本帧 → 判定目标版本相同，失败退出。
6. 某包 `03` → 重发该包。
7. 全部 `72 69 idx 16` 齐了：切回 App 波特率，延时后发 `72 68 BB 16` 读新版本。

MCU 网口「等待上电烧录」用 F0 轮询，直到 Boot `src == 02` 且 `ver != 00` 才发总包数；`src == 01` 状态改为「请断电再上电」（日志只打一次）；Boot 明确 `ver == 00` 弹窗说明是旧 Boot、请改用串口。通道未打开时显示「正在连接，请给控制器重新上电」；通道开了但还没收到协议应答只显示「正在查询版本，等待设备应答」（TCP/串口打开 ≠ 设备在线，不能写已连接/已进 Boot）；掉线立刻回到「正在连接」。收到 App F0 才写「已连接，设备当前在运行程序」；收到 Boot `src == 02` 后才改为「已进入 Bootloader，正在握手」。串口等待上电发的是总包数握手，文案是「等待设备应答」，不要写成查询版本。

超时（量级）：F0 查询 500 ms×1；读版本 1s×3；settle 后总包数探测最多 6 次；每包 1.5s×3。

### 2.5 各机 MCU 波特率（不要记混）

| 机型 | App | Boot（MCU 升级） | 网口 |
|---|---|---|---|
| TPS481500C + 新 STM32 Boot | 19200 | **19200** | 只能 19200 |
| TPS481500C 旧资料 / 部分 G4 | 19200 | 有的 Boot 是 115200 | 不能切到 115200 |
| 高云 GD32 | 默认 19200（档位在 Flash） | 同档 | `V1.6_origin` 可用；新 V1.6 USART0 是屏，网口无效 |

MCU 页 UI：连接波特率 + Boot 波特率，「与连接相同」默认勾选。

---

## 3. FPGA 升级（FA/FB + Modbus CRC16）

实现：`FpgaProtocol.*` + `FpgaUpgradeController.*`。  
固件对照：各 Boot 的 `CMD_40_handle`（安路 `Boot_STM32_anlu`、GD32 `Boot_GD32F3-V1.1`、新 `BootLoader/fpga_update.c`）线格式一致。

### 3.1 CRC16

```
crc = 0xFFFF
for each byte:
    crc ^= byte
    8 times:
        if crc & 1: crc = (crc >> 1) ^ 0xA001
        else:       crc >>= 1
```

帧尾：**高字节先发** `crc>>8`，再 `crc & 0xFF`。MCU 比较写成 `buf[len-2]*256 + buf[len-1]`。

触发帧实例：`FA 68 AA 00 00` → CRC `54 C0`（已用真机日志核对）。  
块大小 256：`FA 68 AA 01 00` → CRC `C4 C1`。  
CRC 失败应答：`FA FF FF FF FF 80 FD`。

### 3.2 命令（PC → MCU）

CRC 覆盖从 `FA` 到参数末字节，不含 CRC 自身。

#### 触发 FPGA 升级（只在 App 发）

```
FA 68 AA 00 00  CRC_H CRC_L     共 7 字节
```

| 机型 | App 收到后 |
|---|---|
| 安路 TPS App | 校验前 5 字节 CRC；要求 `buf[3]<=8 && buf[4]==0x00`；USART1+USART3 各回 `73`；写 `0x55`；`FPGA_Data_send(0xFFF2,0)` / `(0xFFF1,0)` 复位 FPGA；延时后系统复位 |
| 高云 App | **先回 `73`**（任意 `FA`），再看是不是 `68 AA`；**不校验 CRC**；`BOOT_RST(0x55)` |
| 已在 FPGA Boot | 当「块大小 0」→ `65` |

安路 `buf[3]<=8 && buf[4]==0` 会把「设块大小 256..2048」也当成触发。因此设块大小只能在 Boot 里发。

#### 设块大小（Boot）

```
FA 68 AA  blkH  blkL  CRC_H CRC_L     共 7 字节
```

`block = blkH<<8 | blkL`，须 **256..2048 且整除 256**。成功 `73`，非法 `65`。

#### 设总块数（Boot）

```
FA 00  size[23:16]  size[15:8]  size[7:0]  CRC_H CRC_L     共 7 字节
```

`size` 是 **块数**（1-based 写到该值即结束），不是字节数。回 `73`，然后 `up_cmd = 1` 开始擦除。

#### 写块（Boot）

```
FA  addr[31:24] addr[23:16] addr[15:8] addr[7:0]  data[blockSize]  CRC_H CRC_L
```

总长必须是 **`7 + blockSize`**。`addr` 是 **从 1 开始的块号**。回 `73`，`up_cmd = 2`，主循环再写 Flash。

安路映射：`offset = (addr - 1) * blockSize + 0x0C0000`（W25Q）。  
高云：交给 `programXPage` / JTAG，不是 W25Q。

#### 状态轮询

```
FB FF FB     无 CRC
```

固件条件写成（原样，第三项重复判断了 `buf[0]`）：

```
(buf[0]==0xFB && buf[1]==0xFF && buf[0]==0xFB) || up_cmd != 0
```

因此 **只要在忙，任何帧都当查询**，数据帧会被丢掉。

| 应答 | 含义 |
|---|---|
| `FB 00 FB` | 空闲 |
| `FB 01 FB` | 忙（擦或写） |
| `FB 02 FB` | 校验失败（`up_cmd==0xFF`） |

### 3.3 应答（MCU → PC）

| 字节 | 含义 |
|---|---|
| `73` | 命令已接受（ISR 立刻回） |
| `65` | 参数错，或 App 已不在时把触发当块大小 0 |
| `FB xx FB` | 状态 |
| `FA 68 01 16` | 安路进入 FPGA Boot 时发一次的 beacon，**高云不发**；错过就没有 |
| `FA FF FF FF FF CRC` | 本帧 CRC 不对（或长度被算错导致 CRC 不对） |

无法识别的字节（命令回显、`FF` 填充）解析器丢弃后重扫。

### 3.4 上位机流程

```
若自动触发:
    切到 App 波特率（即使和烧录相同也要切，避免沿用打开串口时的错档）
    发 FA 68 AA 00 00 CRC
    73  → 切 Boot 波特率，等 ~3s（或提前收到 beacon）
    65  → 已在 Boot，直接设块大小
若未自动触发:
    假定已在 Boot，按烧录波特率直接设块大小

FA 68 AA blkH blkL CRC  → 73
FA 00 块数 CRC          → 73，MCU 开始擦
循环 FB FF FB           → 直到 FB 00 FB
for addr = 1 .. N:
    FA addr data CRC    → 73
    若 FB 01 FB         → 重发同一 addr
末块后:
    安路：MCU 发多启动并复位 FPGA
    高云：再等 ~2.5s；期间通道掉线视为成功
```

块大小非法、超时次数：见 `FpgaUpgradeController`（安路块超时 3s；高云 12s，给 JTAG 擦写留时间）。

### 3.5 固件切块

`splitFirmware(bin, blockSize)`：按块切开，最后一块 `0xFF` 补到 `blockSize`。短帧 Boot **静默丢弃**。

---

## 4. 安路 FPGA（TPS481500C）

| 项 | 值 |
|---|---|
| 参考 App | `CST-TPS481500C-S4-8T-MCU-V2.0` `fpga_upguade_cmdhandle` |
| 参考 Boot | `Boot_STM32_anlu` `CMD_40_handle` + `uart_fpga_updata` |
| 直连口 | USART3 |
| 直连波特率 | 触发 19200 → Boot **115200**（旧 Boot 写死） |
| 网口 | CH9121 → USART1 @ 19200，触发和烧录都是 19200 |
| 落盘 | W25Q128，擦 **12×64KB** 从 `0x0C0000` 起，上限 **768KB** |
| 完成后 | MCU 写 FPGA 多启动（`0xFFF2=0x0C`，`0xFFF1=0x00`）并复位 |
| beacon | 进模式时发一次 `FA 68 01 16` |
| profile | `anluProfile()`：settle 3s，块超时 3s，无末块额外等待 |
| 文件 | `.bin` |

新 STM32 Boot 若只开 USART1、波特率 19200，则**直连也不再是 115200**。以当前烧进板子的 Boot 为准：旧安路 Boot = USART3/115200；新 Boot = USART1/19200。

---

## 5. 高云 FPGA（GD32 + JTAG）

| 项 | 值 |
|---|---|
| 参考 App | `CST-BPS24200CIP6-6TDP-MCU-V1.6` / `V1.6_origin` 的 `upfpga_cmdhandle` |
| 参考 Boot | `Boot_GD32F3-V1.1` `CMD_40_handle` + `program_internal_flash` / `jtag_gaoyun` |
| 直连口 | USART1，档位默认 19200 |
| 网口 | 仅 `V1.6_origin`（USART0=CH9121@19200）。现用 V1.6 的 USART0 是串口屏，网口触发失败 |
| 落盘 | FPGA **内部 Flash**，不是 W25Q |
| 完成后 | Boot 发 JTAG `0x3A`/`0x3C` 一类重配置，再复位 MCU |
| beacon | **无**。用设块大小当探测 |
| 硬上限 | 512KB；Boot 注释里还有 `0x1B200`（约 108.5KB）警告线，超了只警告仍下发 |
| profile | `gowinProfile()`：块超时 12s，末块再等 2.5s，掉线算成功 |
| 文件 | `.bin` 或 `.fs` |

高云 App：任意 `FA` 先回 `73`，再解析 `68 AA`。不要用「没 CRC」当失败。

---

## 6. CH9121 发现（不是升级协议）

实现：`W5500ConfigProtocol.*` + `Ch9121Scanner.*`。从 `upper_upgrade` 抄的 285 字节固定包，只用来找设备、连 TCP。

| 项 | 值 |
|---|---|
| 包长 | 285（头 30 + 数据 255） |
| 魔数 | `CH9121_CFG_FLAG\0`（16 字节） |
| 设备 UDP | 50000 |
| 本机 UDP | 60000 |
| 命令 | Search=`0x04` Get=`0x02` Set=`0x01`；应答或 `0x80` |
| TCP 透传 | 默认 **6600** |

进程内只能有一个扫描器。绑定**所选网卡的 IP** 再广播，否则多网卡扫不到。

升级数据走 TCP 透传，字节就是第 2、3 节的 UART 帧。CH9121 不理解 CST-ISP / FA/FB。

---

## 7. 上位机模块对照

| 文件 | 职责 |
|---|---|
| `IspProtocol.*` | MCU 建帧 / 解析 / 切 256B |
| `FpgaProtocol.*` | FPGA 建帧 / CRC / 切块 / 解析应答 |
| `IspUpgradeController.*` | MCU 状态机，认 `IIspTransport` |
| `FpgaUpgradeController.*` | FPGA 状态机 + anlu/gowin profile |
| `IspSerialPort.*` | 串口 |
| `IspNetworkTransport.*` | TCP；`setBaudRate` 空操作 |
| `tests/IspProtocolTest.cpp` | MCU 流程，无需真串口 |
| `tests/FpgaProtocolTest.cpp` | 安路/高云 profile，含同波特率也要切一档 |

改字节格式：先改 Protocol + 测试，再动 Controller。

---

## 8. 和「应用协议」的边界

TPS481500C 运行时 MCU↔上位机还有一套 **BCC（异或）命令**（模式、脉宽等），IDLE 分帧，与 CST-ISP / FA/FB 并存于同一 UART。本工具只认升级帧：

- `72 …` → MCU 升级
- `FA` / `FB` → FPGA 升级

不要把应用命令的 BCC 套到升级帧上，也不要在升级工具里实现 `31`/`32`。
