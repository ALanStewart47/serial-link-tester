# CLAUDE.md — 串口测试工具开发指南

> 本文件是项目的"主控文档"。任何接手的人（包括上下文被清空后的我）只读这一份，就能知道：要做什么、用什么技术、做到哪一步了、下一步做什么、不能踩哪些坑。
>
> **强制约束：每次有实质进度（完成一个阶段、改了关键设计、发现并修正了需求问题），必须在文末「更新记录」追加一行，并同步更新「当前进度」一节。先更新本文件，再认为工作完成。**

---

## 0. 一句话目标

用 Qt6（C++/Widgets）开发一个 **Windows 串口测试工具**：在普通串口助手（收发 / 时间戳 / HEX-ASCII / 可选 BCC）的基础上，增加**协议指令库**、**单条指令自动循环发送**、**丢包率 / 正确率检测统计**、**日志**四大定制功能。

第一版（V1）范围严格按 `00_Doc/串口测试工具软件功能需求表_V1.1.md`，并采纳本文件第 4 节的修订。

---

## 1. 关键决策记录（已和用户确认，不要随意推翻）

| 编号 | 决策 | 结论 | 原因 |
|---|---|---|---|
| D-01 | 指令"修改参数"的方式 | **原始字符串方式**：每条指令直接编辑「发送内容」文本框和「正确回复」文本框，软件**不解析协议语义**。BCC 可勾选自动追加。 | 一次覆盖新/旧/旧扩展全部协议，工作量可控，与需求表 6.3 字段定义一致。结构化参数表单留作后续版本。 |
| D-02 | 默认指令库内容 | **预置全部命令**：把 `00_Doc/serial_protocol_summary.md` 里新协议(CA/CB/CC)、旧协议(ASCII)、旧扩展协议($/@)的全部命令生成默认 JSON，开箱即用。 | 用户开箱即用，也是指令库分组显示功能的天然测试数据。 |
| D-03 | 自动发送的收发配对模型 | **一发一收 ping-pong**：发一条 → 等它的回复（或超时）→ 再发下一条。请求与回复严格 1:1。 | 丢包率/正确率统计绝对准确。代价：实际间隔 = 设定间隔 + 设备回复耗时，极高速(1ms)下达不到精确 1ms（这点写进 UI 提示，作为已知限制）。固定节拍压力测试模式留作后续版本。 |
| D-04 | 正确率：实时算 vs 事后算 | **后台线程实时计算**。 | 一次"发送内容==收到内容"的字节比较是微秒级，对发送节拍无影响。真正拖慢的是①每条都刷界面 ②每条都写 UI 文本框 ③全量日志同步落盘 —— 这些用「UI 限频刷新 + 界面只留最近 N 条 + 全量日志默认关且异步落盘」解决。 |
| D-05 | 工程来源与工具链 | 从 `01_Software/reference/upper_upgrade` **拷贝改造**到 `01_Software/serial_test_tool`，保持 **Qt 6.5.3 + CMake + MinGW 64 位** 不变。 | 复用已验证的 `SerialTransport` 分层和构建配置，用户无需重建工程。 |
| D-06 | 自动发送入口 | **独立第 3 页**：自动发送页有自己的指令下拉框（从指令库选一条），间隔/次数/启停/统计都在该页；指令库页保持纯管理。 | 结构清晰、两页低耦合，符合需求表 5 页规划。 |
| D-07 | 自动发送线程架构 | **UI 线程事件驱动**（修订原约束#4 的"工作线程"要求）。引擎用 QTimer + transport 信号驱动，不阻塞；统计在内存实时累加，界面按定时器(200ms)刷新快照。 | ping-pong 是事件驱动而非死循环，发送/匹配是微秒级操作，UI 线程不会被阻塞；避免 QSerialPort 跨线程隐患；对第一次写桌面软件最简单最安全。间隔1ms~5S、千万次均可胜任。**已知代价**：UI 被拖窗/弹框瞬间阻塞时发送会延后几毫秒——属可接受。工作线程化列为后续硬化项。 |

---

## 2. 技术栈与工具链

| 项 | 选型 | 说明 |
|---|---|---|
| 语言 | C++17 | 与参考工程一致 |
| 框架 | Qt 6.5.3 | Widgets（桌面 UI）、SerialPort（串口）、Core（QJsonDocument/QFile/QSettings/QThread） |
| 构建 | CMake (≥3.19) + Ninja | 参考工程已有 `CMakeLists.txt`，照搬扩展 |
| 编译器/Kit | MinGW 64 位（Desktop_Qt_6_5_3_MinGW_64_bit） | 参考工程的 build 目录即此 Kit |
| 平台 | 仅 Windows | V1 不做跨平台 |
| 配置存储 | JSON（指令库）+ QSettings（软件设置） | 见第 6 节 |

**编译命令（命令行验证用）：**
```powershell
# 在 01_Software/serial_test_tool 下
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="<Qt6.5.3 mingw 安装路径>"
cmake --build build
```
（IDE 用 Qt Creator 打开 `CMakeLists.txt` 选 MinGW Kit 直接构建也可。）

---

## 3. 参考工程可复用资产

位置：`01_Software/reference/upper_upgrade`（**只读参考，不在里面改**）

| 文件 | 可复用点 |
|---|---|
| `CMakeLists.txt` | Qt6 Widgets+SerialPort 的 CMake 写法，直接照搬并加新源文件 |
| `SerialTransport.{h,cpp}` | `QSerialPort` 封装：open/close/sendFrame、readyRead 信号、错误与离线处理。**直接复用**，但要把"按协议帧解析"改成"裸字节流 + 可配置成帧规则"（测试工具要支持任意协议，不能写死升级协议的 FrameParser）。 |
| `main.cpp` | 标准入口，照搬 |
| `mainwindow.{h,cpp,ui}` | UI 在代码里 buildUi 的写法、信号槽连接、中文 `QStringLiteral` 日志风格 —— 借鉴风格，但本工具改成多页（QTabWidget） |
| `UpgradeController/Protocol` | 升级业务专用，**不复用**，但其"状态机 + 信号驱动 UI"的模式值得借鉴到 AutoSendEngine |

---

## 4. 优化后的功能需求（对 V1.1 文档的采纳与修订）

需求主体见 `00_Doc/串口测试工具软件功能需求表_V1.1.md`。该文档整体**合理**，以下是必须落地的**修订/澄清**（开发以本节为准）：

### 4.1 统计口径修订（重要）
原文档统计公式有歧义，按下表执行：

| 指标 | 定义（V1 最终口径） |
|---|---|
| `send_count` | 已发出的指令数（ping-pong 下 = 已完成的轮次数） |
| `recv_count` | 在超时窗口内收到了**任意**回复的次数（不管对不对） |
| `match_count` | 收到回复且**完全等于**正确回复的次数 |
| `mismatch_count` | 收到回复但内容不等于正确回复的次数 = `recv_count - match_count` |
| `timeout_count` | 超时窗口内没收到任何回复的次数 = `send_count - recv_count` |
| **丢包率** | `timeout_count / send_count`（分母只算**已完成**的轮次，在途未判定的不计入） |
| **回复正确率** | `match_count / recv_count`（只在收到回复的范围内看对不对；recv_count=0 时显示 N/A） |
| **总成功率** | `match_count / send_count`（端到端真正成功的比例，这是最该看的指标） |
| 响应时间 | 每轮"发送时刻→收到首字节时刻"，统计 min/max/avg，只统计 match 成功的轮次 |

> 说明：丢包率回答"有没有收到回复"，正确率回答"收到的对不对"，总成功率回答"最终成功多少"。三者都显示。

### 4.2 "一次完整回复"的判定规则（D-03 配套）
ping-pong 模型下，一轮的回复判定：
1. 发送后启动该轮的超时计时（超时时间见 4.3）。
2. 累积接收缓冲区。每次有新数据到达就检查：**当前累积内容是否已包含/等于该指令配置的「正确回复」**（精确匹配——按配置的 HEX/ASCII 解析后逐字节比较）。
3. 匹配成功 → 该轮 match，记录响应时间，立即进入下一轮。
4. 超时仍未匹配：若期间收到过任何字节 → mismatch；若一个字节都没收到 → timeout。然后进入下一轮，**丢弃**本轮残留缓冲（避免污染下一轮）。
5. 检测功能关闭时：只发送、只计 `send_count`，不等待不统计（按固定节拍发，达到固定节拍上限能力即可）。

### 4.3 自动超时计算（采纳原文档 6.5.2）
```
auto_timeout_ms = clamp(send_interval_ms * 2, 10, 5000)
```
手动超时模式：用户直接填，范围同样 clamp 到 [10, 5000]。

### 4.4 BCC 规则（采纳原文档 13.3）
- 仅 HEX 发送模式可勾选自动追加 BCC。
- 算法：对当前要发送的全部 HEX 字节做逐字节异或，结果 1 字节追加到末尾。
- 指令库每条指令有 `enable_bcc` 字段，自动发送时按该字段决定是否追加。

### 4.5 范围约束
- 发送间隔：1ms ~ 5000ms（超范围报错并阻止）。
- 发送次数：1 ~ 10,000,000（用 64 位整数计数，见 NFR-007）。
- 界面接收区只保留最近 N 条（默认 1000）。
- 全量日志默认**关**，开启后**异步写文件**，绝不全部驻留内存或 UI 文本框。

---

## 5. 目标工程结构与模块划分

```
01_Software/serial_test_tool/
├── CMakeLists.txt
├── main.cpp
├── app/
│   ├── MainWindow.{h,cpp}          # QTabWidget 容器 + 全局状态栏
├── core/
│   ├── SerialTransport.{h,cpp}     # 串口收发（复用参考工程，去掉写死的协议解析）
│   ├── PacketBuilder.{h,cpp}       # HEX/ASCII 文本 ⇄ 字节；BCC 计算追加
│   ├── CommandLibrary.{h,cpp}      # 指令库 加载/保存/增删改查；JSON 持久化
│   ├── CommandItem.h               # 指令数据结构（见第 6 节字段）
│   ├── AutoSendEngine.{h,cpp}      # ping-pong 自动发送状态机（跑在工作线程）
│   ├── ResponseMatcher.{h,cpp}     # 精确匹配判定
│   ├── TestStatistics.{h,cpp}      # 64 位计数 + 各比率计算（线程安全）
│   └── LogManager.{h,cpp}          # 统计/异常/全量日志，异步落盘
├── ui/
│   ├── BasicSerialPage.{h,cpp}     # Page1 串口基础收发
│   ├── CommandLibraryPage.{h,cpp}  # Page2 指令库（表格，增删改查）
│   ├── AutoSendPage.{h,cpp}        # Page3 自动发送测试
│   ├── ResultPage.{h,cpp}          # Page4 结果与日志
│   └── SettingsPage.{h,cpp}        # Page5 软件设置
└── resources/
    └── default_commands.json       # D-02 预置的默认指令库
```

**线程模型（NFR-004）：**
- UI 主线程：界面、按钮、表格编辑、定时刷新统计。
- 串口工作线程：`SerialTransport` + `AutoSendEngine` + `ResponseMatcher` + `TestStatistics` 实时更新。
- 日志线程：全量/异常日志异步写文件。
- 跨线程只用 Qt 信号槽（`Qt::QueuedConnection`）传递，不直接碰对方数据。

**UI 刷新策略（NFR-005）：** 统计在后台实时更新；UI 用一个 QTimer 每 100~300ms 把最新统计快照刷到界面，绝不"收一条刷一次"。

---

## 6. 指令库数据格式（JSON）

每条指令字段（对应需求表 6.3，命名用英文 key 便于代码）：

```json
{
  "command_id": "NEW_SET_BRIGHTNESS_CH1",
  "protocol_type": "new",          // new | old | old_ext
  "function_group": "digital",     // digital | strobe | common | program
  "command_name": "设置1通道亮度",
  "send_format": "hex",            // hex | ascii
  "send_data": "CA 01 01 00 FF",
  "enable_bcc": true,
  "expected_reply_format": "hex",  // hex | ascii
  "expected_reply": "CA 01 01 00",
  "match_rule": "exact",           // V1 固定 exact
  "timeout_mode": "auto",          // auto | manual
  "timeout_ms": 100,
  "description": "设置1通道亮度为255",
  "enabled": true,
  "builtin": true,                 // 内置指令保护：true 不可删，只能复制后改
  "remark": "亮度范围 0~255 或 0~999"
}
```

- 文件：用户库存 `%AppData%` 或程序目录下 `commands.json`；`resources/default_commands.json` 为出厂默认，支持「恢复默认」。
- 软件设置（串口参数、显示格式、日志路径等）用 QSettings 持久化。

---

## 7. 分阶段实现计划与验证方法

> 每个阶段做完，必须按"验证"一列实际跑通，并在「更新记录」登记。未验证不算完成。

| 阶段 | 内容 | 关键产出 | 验证方法 |
|---|---|---|---|
| **S0 工程搭建** | 拷贝参考工程→`serial_test_tool`，改名、改 CMake，能编译出空主窗口（QTabWidget 五页占位） | 可运行的空壳 | `cmake --build` 成功；双击 exe 出现五个 Tab |
| **S1 基础串口** | 串口扫描/参数/开关、手动发送、HEX/ASCII 收发、时间戳、清空、可选 BCC、异常提示（FR-001~012） | BasicSerialPage + SerialTransport + PacketBuilder | 用虚拟串口对（com0com）或真实设备：发 `CST` 收到回显；HEX 发 `CA 01 01 00 FF` 勾 BCC 末尾自动追加；拔串口不崩溃有提示 |
| **S2 指令库** | 指令库表格分组显示、增删改查、JSON 持久化、内置保护、默认库预置全部命令（FR-101~114, D-02） | CommandLibraryPage + CommandLibrary + default_commands.json | 启动看到新/旧/旧扩展分组的全部指令；新增/改/删一条后重启仍在；内置指令不可直接删 |
| **S3 自动发送** | 选一条指令、设间隔/次数、开始/停止、状态显示、参数锁定、ping-pong 引擎（FR-201~211, D-03） | AutoSendPage + AutoSendEngine（工作线程） | 设间隔 100ms 次数 100，发送计数稳定增长；点停止立即停；运行中串口/指令/间隔被锁定 |
| **S4 检测统计** | 检测开关、精确匹配、自动/手动超时、各计数与丢包率/正确率/总成功率实时显示（FR-301~311, 4.1/4.2） | ResponseMatcher + TestStatistics + ResultPage | 设备正常回复→正确率100%、丢包0；故意配错 expected_reply→正确率下降、mismatch 增长；拔设备→超时增长、丢包率上升；界面不卡 |
| **S5 日志与稳定性** | 统计日志、异常日志、全量日志开关+异步落盘、CSV/TXT 导出、界面限 N 条、断开保护、长稳（FR-401~408, NFR 全部） | LogManager + 软件设置持久化 | 跑 100 万次（小间隔）内存不爆、UI 不卡；全量日志正确落盘可导出；测试中拔串口自动停止并提示 |

---

## 8. 开发约束（防止最终实现"变形"，硬性遵守）

1. **范围冻结**：V1 只做需求表 + 本文件第 4 节的内容。第 7 节「第一版不实现」清单（多指令脚本、通配/正则匹配、多串口、TCP/UDP、跨平台、图表曲线）一律不做。想加新功能先更新本文件并说明，不得静默扩张。
2. **决策不回退**：第 1 节 D-01~D-05 是已确认决策。要改必须先在本文件记录原因并征得用户同意。
3. **统计口径唯一**：所有比率计算只用第 4.1 节定义，不在代码里另立公式。
4. **线程纪律（按 D-07 修订）**：V1 自动发送引擎与串口收发在 UI 线程事件驱动，不得用阻塞循环/sleep 占住 UI；统计内存实时累加，界面按定时器(100~300ms)刷新快照，禁止"收一条刷一次界面"。后续若改工作线程化，须跨线程信号槽、禁止裸指针共享可变状态。
5. **计数用 64 位**：所有次数计数器用 `quint64`/`qint64`，不得用 int。
6. **不写死协议语义**：D-01 决定了工具按"原始字符串"工作。核心收发/匹配代码不得内置任何具体命令的语义，保持协议无关。
7. **中文面向用户**：界面文案、日志、指令说明一律中文（用户群是调试/测试人员），代码标识符用英文。
8. **异常不崩溃**：串口打开失败、写失败、设备断开、非法输入、日志写失败，全部要有明确提示且程序存活（NFR-101~106）。
9. **每阶段可独立运行验证**：不允许堆到最后才编译。每个 S 阶段结束都要能编译运行并通过该阶段验证。
10. **参考工程只读**：`reference/upper_upgrade` 不改动，只拷贝/借鉴。

---

## 9. 当前进度

- **阶段**：S0~S5 **核心功能均已验收通过** → **第一版功能全部完成**。
  - S5 已验收：summary.csv/exception_*.csv/full_*.csv 落盘、结果页导出 CSV（WPS 可正常打开）。
  - 待后续验收（用户暂缓）：S5 配置记忆重启恢复、千万次长稳压力测试；S1 的拔串口、S3 的关检测固定节拍此前也标记为暂缓。
  - 备注：测试运行中 Windows 资源管理器可能把仍打开的日志文件显示为 0KB（文件句柄未关闭，大小元数据滞后），关闭软件后大小正常，非缺陷。
- **已做 code-review 并修复**（2026-06-05）：
  - 真 bug：#1 PacketBuilder 用 ASCII 范围判断替换 `isxdigit(toLatin1())` 的 UB；#3 `~LogManager` 中断时补写 summary；#4 CommandLibrary 首次 save 失败上报；#5 LogWriter 缓存改 per-instance + 每会话 `closeFile` 释放句柄；#6 `recordSend()` 移到 `send()` 成功之后。
  - 清理：#7 比率格式化收敛到 `TestStatistics::lossRateText/correctRateText/successRateText/respText`；#10 CSV 转义抽到 `core/CsvUtil.h`（含换行）；#9 Tab 运行锁定改用页面指针(m_autoPage/m_resultPage)不依赖下标；#8 全量日志开关唯一来源=QSettings，`LogManager::beginSession` 时读取（移除 setFullLogEnabled 缓存），与引擎 start 时读取同源。
  - 已知/可接受：#2 ping-pong 无序号关联（仅设备回复接近超时时显现，属设计边界，未改）。
- **文档**：已出 `00_Doc/代码导读与CppQt实战指南.html`（面向只懂 C 的单片机工程师：工程结构/各模块逐讲/信号槽/C→C++Qt 学习路线/练习）。
- **后续可选项**：长稳压力测试收尾、暂缓验收项（配置记忆重启/拔串口/关检测固定节拍）的回归。
- **S5 实现**：
  - `core/AppConfig.h`：QSettings 的 org/app + 全部配置键（独立于指令库 AppDataLocation）。
  - `core/LogManager`（+内部 `LogWriter`）：**唯一的工作线程**——日志文件 IO 在后台线程，UI 线程缓存行、300ms 成批投递（NFR-004/006/407）。统计汇总 `summary.csv` + 异常 `exception_*.csv` 默认写；全量 `full_*.csv` 仅勾选时每轮写。CSV 字段转义。
  - 引擎加 `Config.fullLog` + `roundCompleted(每轮，仅fullLog时发)` + `config()`；MainWindow 连引擎信号到 LogManager(beginSession/logRound/logException/endSession)。
  - `ui/SettingsPage`（第5页）：日志目录(浏览/打开)、全量日志开关，QSettings 持久化、实时应用到 LogManager。
  - `ui/ResultPage` 加「导出明细」→ CSV/TXT(QFileDialog)。
  - **配置记忆**：BasicSerialPage(波特率/数据位/校验/停止位/显示格式/时间戳/发送格式/BCC)、AutoSendPage(间隔/次数/检测/超时模式与值/上次指令) 均 QSettings 即改即存、重启恢复(FR-501~503)。
  - 全量日志默认目录 `%AppData%/串口测试工具/logs`。
- **已完成**：
  - 需求分析、D-01~D-05 决策、本 CLAUDE.md。
  - **S0**：`01_Software/serial_test_tool/` 工程骨架（CMake + main + MainWindow 五页 QTabWidget + .gitignore），编译通过。
  - **S1**：`core/SerialTransport`（裸字节流改造）、`core/PacketBuilder`（HEX/ASCII/BCC）、`ui/BasicSerialPage`（串口扫描/参数/开关、手动 HEX/ASCII 发送、收发监视带时间戳、显示格式切换、可选 BCC、异常提示、界面限 1000 行）。编译通过、exe 产出。
  - **S2**：`core/CommandItem`（指令数据结构+JSON 序列化）、`core/CommandLibrary`（加载/保存/CRUD/分组/内置保护/恢复默认，用户文件存 `%AppData%/串口测试工具/commands.json`，首次运行从内置资源初始化）、`ui/CommandEditDialog`（编辑表单）、`ui/CommandLibraryPage`（协议→功能分组树、搜索、新增/复制/修改/删除、内置保护）。默认指令库 132 条由 `tools/gen_default_commands.py` 生成到 `resources/default_commands.json`，经 `resources/resources.qrc` + `CMAKE_AUTORCC` 内置到 exe。编译通过。
    - 注意：要让 .qrc 生效必须 `set(CMAKE_AUTORCC ON)`（qt_standard_project_setup 默认只开 AUTOMOC/AUTOUIC）。
    - 默认库为"命令字粒度"：通道型命令用通道 1/A 作示例；read 类命令回复含实时数据无法精确匹配，expected_reply 留空并在 remark 注明；占位/不可用命令 enabled=false 并在 remark 注明。改默认库须改 `gen_default_commands.py` 重新生成，不要手改 JSON。
  - **S3**：`core/TestStatistics`（64 位计数+丢包率/正确率/总成功率，§4.1 口径）、`core/AutoSendEngine`（UI 线程事件驱动 ping-pong，检测开=发→等回复/超时→隔 interval 发下一条；检测关=固定节拍只计发送）、`ui/AutoSendPage`（指令下拉/间隔1~5000/次数1~1e7/检测开关/超时模式/启停/状态/进度/实时统计，UI 每 200ms 刷新快照）。MainWindow 持有 `m_engine{&m_transport}`，运行期 `onAutoRunningChanged` 禁用其它页（FR-208）。编译通过。
    - 匹配规则（§4.2）：检测轮内累计 RX，`expected` 非空时按"包含 expected 字节序列"判匹配；`expected` 为空（读命令）时收到任意回复即视为匹配。超时窗口内收到过字节但未匹配=回复错误(mismatch)，一字节没收到=超时(丢包)。
    - 已知点：自动测试期间通过禁用其它 Tab 防误操作；引擎与 BasicSerialPage 共用同一 SerialTransport。
  - **S4**：`ui/ResultPage`（第4页）= 结果汇总(实时/最终统计) + 异常明细表(第几次/时间/类型/实际回复HEX+ASCII，界面限 5000 行)。引擎新增 `roundFailed(roundIndex,timeout,actual)` 信号，**仅失败轮发出**（成功轮不发，避免高速刷屏）；ResultPage 连该信号填表，Running 时清表并 300ms 刷新汇总。MainWindow 运行期锁页改为保留第2(自动发送)+第3(结果只读)页可用。**已通过真实设备验收**。
  - **S4 显示增强**：`ui/RateBar`（自绘百分比矩形条，左标题+填充条+居中百分比/NA）。ResultPage 新增 3 条：丢包率(红)/回复正确率(绿)/总成功率(蓝)，实时+结束都刷新。纯显示，未改 S4 统计/明细逻辑（注：需求文档把"图表曲线"列为不做，但这是百分比条不是曲线图表、不引图表库，属用户明确要求的显示增强）。编译通过。
- **构建方式（已验证可用）**：Qt 自带工具链——
  ```powershell
  $env:Path = "D:\QT\Tools\CMake_64\bin;D:\QT\Tools\Ninja;D:\QT\Tools\mingw1120_64\bin;" + $env:Path
  cmake -S <proj> -B <proj>\build -G Ninja -DCMAKE_PREFIX_PATH="D:/QT/6.5.3/mingw_64" -DCMAKE_CXX_COMPILER="D:/QT/Tools/mingw1120_64/bin/g++.exe"
  cmake --build <proj>\build
  ```
  （或用 Qt Creator 打开 `CMakeLists.txt` 选 MinGW Kit 构建。）
- **下一步（第一版已完成，均为可选）**：①用户回归暂缓验收项（S5 配置记忆重启、千万次长稳、拔串口、关检测固定节拍）；②如需可出 Markdown 版指南进 Git。
- **待办/风险（非阻塞）**：
  - 默认指令库 132 条仍建议逐条核对 `serial_protocol_summary.md`（占位/不建议命令已在 remark 注明、enabled=false）。
  - 收发监视的 HEX/ASCII 显示切换只对"切换后的新数据"生效（不回溯重渲染已显示内容）。
  - #2 ping-pong 无序号关联属设计边界（见上）。

---

## 10. 更新记录

> 规则：每次实质进度追加一行，最新在上。格式：日期 | 阶段 | 改动摘要。

| 日期 | 阶段 | 改动摘要 |
|---|---|---|
| 2026-06-05 | 文档 | 输出 `00_Doc/代码导读与CppQt实战指南.html`（面向 C/单片机工程师的代码导读 + C++/Qt 针对性学习路线，单文件 HTML）。 |
| 2026-06-05 | code-review 修复 | 修真 bug #1(isxdigit UB)/#3(中断写summary)/#4(首次save检查)/#5(日志句柄per-instance+按会话关闭)/#6(recordSend顺序);清理 #7(比率文本helper)/#8(全量日志单一来源)/#9(Tab锁定用页指针)/#10(CsvUtil共享转义)。编译通过。 |
| 2026-06-05 | S5 验收 | S5 核心验收通过：summary/exception/full 日志落盘、结果页导出 CSV(WPS 打开正常)。第一版功能全部完成。配置记忆重启恢复与长稳压力测试由用户后续验证。 |
| 2026-06-05 | S5 | 日志与设置收尾：LogManager(后台线程异步落盘:summary/exception默认+full可选每轮)、AppConfig(QSettings键)、SettingsPage(第5页:日志目录/全量开关)、ResultPage 导出 CSV/TXT、引擎 fullLog+roundCompleted、串口页/自动发送页配置记忆持久化。编译通过，待功能验证。第一版功能全部落地。 |
| 2026-06-05 | S4 验收 + 显示增强 | S4 真实设备验收通过(明细表/清空/运行期保留结果页)。按用户要求加 RateBar 图形化百分比条(丢包率/正确率/总成功率)到结果页，纯显示不改逻辑。编译通过。 |
| 2026-06-04 | S3 验收 + S4 | S3 真实设备验收通过(ping-pong/丢包/响应/回复错误)。S4：ResultPage(结果汇总+异常明细表)，引擎加 roundFailed 仅失败轮信号；运行期保留结果页可看。编译通过。 |
| 2026-06-04 | S3 | 自动发送：TestStatistics(§4.1 口径)、AutoSendEngine(UI线程事件驱动 ping-pong，检测开/关两模式)、AutoSendPage(选指令/间隔/次数/检测/超时/启停/实时统计 200ms 刷新)；MainWindow 接入第3页+运行期锁其它页(D-07/FR-208)。编译通过，待真实设备验证。 |
| 2026-06-04 | S2 验收 | GUI 验收 S2 通过：132 条分组显示、搜索过滤、内置指令修改/删除置灰、复制副本、新增持久化、恢复默认。 |
| 2026-06-04 | S2 | 指令库：CommandItem/CommandLibrary(JSON 持久化+CRUD+内置保护+恢复默认)、CommandEditDialog、CommandLibraryPage(分组树+搜索+增删改查)；默认库 132 条(gen_default_commands.py 生成，.qrc 内置，需 CMAKE_AUTORCC)；接入 MainWindow 第2页。编译通过，待功能验证。 |
| 2026-06-04 | S1 验收 | 真实设备验收 S1 通过：COM3 开关、ASCII `CST#`→`CST`、HEX `CA 01 01 00 FF`+BCC(35)、HEX/ASCII 显示切换、拔串口弹窗不崩溃。 |
| 2026-06-04 | S0/S1 | 搭建 `serial_test_tool` 工程（CMake+5页QTabWidget空壳）；实现 SerialTransport(裸字节流)、PacketBuilder(HEX/ASCII/BCC)、BasicSerialPage(基础串口收发页 FR-001~012)。Qt6.5.3 MinGW + Ninja 编译通过、exe 产出。S1 功能验证待真实设备。 |
| 2026-06-04 | — | 创建 CLAUDE.md：完成需求分析、确认 5 项关键决策(D-01~05)、修订统计口径与收发判定规则、定义工程结构/JSON 格式/分阶段计划/开发约束。 |
