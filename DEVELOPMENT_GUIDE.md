# DEVELOPMENT_GUIDE.md — 串口测试工具 AI 开发指引

> **本文件是给 AI 助手读的"项目上下文文件"。**
> 任何 AI（Claude、Copilot、ChatGPT、Cursor、Qoder 等）接手本项目时，**先完整读取本文件**，即可知道：项目是什么、做到哪了、下一步做什么、有哪些坑不能踩。
>
> **维护规则：** 每次有实质进度（完成一个阶段、修改关键设计、发现并解决问题），必须在本文末尾「更新记录」追加一行，并同步更新「当前进度」一节。**先更新本文件，再认为工作完成。**

---

## 0. 项目一句话

用 **Qt 6.5.3 (C++/Widgets)** 开发 Windows 桌面串口测试工具，在普通串口助手基础上增加：**协议指令库**、**单条指令自动循环发送**、**丢包率/正确率检测统计**、**异步日志**四大定制功能。

---

## 1. 技术栈

| 项 | 选型 |
|---|---|
| 语言 | C++17 |
| 框架 | Qt 6.5.3（Widgets + SerialPort + Core） |
| 构建 | CMake ≥ 3.19 + Ninja |
| 编译器 | MinGW 64 位（Desktop_Qt_6_5_3_MinGW_64_bit） |
| 平台 | 仅 Windows（V1 不做跨平台） |
| 配置存储 | JSON（指令库）+ QSettings（软件设置） |

**编译命令（命令行）：**
```powershell
# 在 01_Software/serial_test_tool 下
$env:Path = "D:\QT\Tools\CMake_64\bin;D:\QT\Tools\Ninja;D:\QT\Tools\mingw1120_64\bin;" + $env:Path
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="D:/QT/6.5.3/mingw_64" -DCMAKE_CXX_COMPILER="D:/QT/Tools/mingw1120_64/bin/g++.exe"
cmake --build build
```

---

## 2. 工程结构

```
01_Software/serial_test_tool/
├── CMakeLists.txt
├── main.cpp                    # 入口
├── app/
│   └── MainWindow.{h,cpp}      # QTabWidget 容器 + 全局状态栏
├── core/                       # 业务核心，与 UI 解耦
│   ├── SerialTransport.{h,cpp} # 串口收发封装（裸字节流，协议无关）
│   ├── PacketBuilder.{h,cpp}   # HEX/ASCII ⇄ 字节转换，BCC 异或校验
│   ├── CommandItem.{h,cpp}     # 指令数据结构 + JSON 序列化
│   ├── CommandLibrary.{h,cpp}  # 指令库 CRUD、JSON 持久化、内置保护
│   ├── AutoSendEngine.{h,cpp}  # ping-pong 自动发送状态机（UI 线程事件驱动）
│   ├── TestStatistics.{h,cpp}  # 64 位统计累加器（丢包率/正确率/总成功率）
│   ├── LogManager.{h,cpp}      # 后台线程异步日志（summary/exception/full CSV）
│   ├── AppConfig.h             # QSettings 键定义
│   └── CsvUtil.h               # CSV 字段转义工具
├── ui/                         # 5 页 QTabWidget
│   ├── BasicSerialPage.{h,cpp}     # Page1 基础串口收发
│   ├── CommandLibraryPage.{h,cpp}  # Page2 指令库管理
│   ├── CommandEditDialog.{h,cpp}   # 指令编辑对话框
│   ├── AutoSendPage.{h,cpp}        # Page3 自动发送测试
│   ├── ResultPage.{h,cpp}          # Page4 结果汇总 + 异常明细
│   ├── RateBar.{h,cpp}             # 自绘百分比条形图
│   ├── TrendChart.{h,cpp}          # 自绘趋势曲线（不引 Qt Charts）
│   └── SettingsPage.{h,cpp}        # Page5 软件设置
└── resources/
    ├── default_commands.json   # 预置 132 条默认指令（由 gen_default_commands.py 生成）
    └── resources.qrc           # Qt 资源文件（需 CMAKE_AUTORCC=ON）
```

---

## 3. 核心设计决策（已确认，勿随意推翻）

| 编号 | 决策 | 结论 | 原因 |
|---|---|---|---|
| D-01 | 指令参数方式 | **原始字符串**：每条指令直接编辑发送内容/正确回复文本框，软件不解析协议语义 | 一次覆盖全部协议，结构化表单留作后续版本 |
| D-02 | 默认指令库 | **预置全部 132 条命令**（新/旧/旧扩展协议），开箱即用 | 用户无需手动录入 |
| D-03 | 收发配对模型 | **ping-pong**：发一条 → 等回复/超时 → 再发下一条 | 统计绝对准确；固定节拍压测模式已通过"检测关闭"实现 |
| D-04 | 正确率计算时机 | **UI 线程实时计算**，界面 200ms 限频刷新 | 字节比较是微秒级，不构成瓶颈 |
| D-05 | 工程来源 | 从 `reference/upper_upgrade` 拷贝改造，复用 `SerialTransport` 分层 | 保持工具链一致 |
| D-06 | 自动发送入口 | **独立第 3 页**，与指令库页（第 2 页）低耦合 | 结构清晰 |
| D-07 | 线程架构 | **UI 线程事件驱动**（QTimer + 信号），不启工作线程 | 避免 QSerialPort 跨线程隐患；已知代价：拖窗时发送延后几毫秒，可接受 |
| D-08 | V1.1 增强范围 | 监视回溯重渲染、指令库导入导出、ETA、单发、趋势曲线（自绘）、ASCII 转义、固定节拍模式 | 用户明确要求，不改核心统计/收发逻辑 |

---

## 4. 协议背景

项目服务的设备有 3 套串口协议（详见 `00_Doc/serial_protocol_summary.md`）：

| 协议 | 帧头/标志 | 特点 |
|---|---|---|
| **新协议** | `0xCA` 单通道 / `0xCB` 多通道 / `0xCC` 可编程 | 二进制 HEX，BCC 异或校验 |
| **旧协议** | ASCII 字符串，`#` 结束 | 通道用 A~H 字母 |
| **旧扩展协议** | `$`（写）/ `@`（读）+ 2 位功能号 | 更规整的 ASCII 格式 |

> ⚠️ **约束**：核心收发代码不得内置任何具体命令语义（开发约束 #6），保持协议无关。

---

## 5. 统计口径（唯一权威定义）

| 指标 | 公式 |
|---|---|
| `send_count` | 已发出的指令数 |
| `recv_count` | 超时窗口内收到任意回复的次数 |
| `match_count` | 收到回复且完全等于正确回复的次数 |
| `mismatch_count` | `recv_count - match_count` |
| `timeout_count` | `send_count - recv_count` |
| **丢包率** | `timeout_count / send_count` |
| **回复正确率** | `match_count / recv_count`（recv=0 时显示 N/A） |
| **总成功率** | `match_count / send_count` |
| 响应时间 | 发送时刻→收到首字节时刻，仅统计 match 成功的轮次 |

---

## 6. 当前进度

### 6.1 已完成（V1.0 + V1.1 增强）

| 阶段 | 内容 | 状态 |
|---|---|---|
| S0 工程搭建 | CMake + 5 页 QTabWidget 骨架 | ✅ 完成 |
| S1 基础串口 | 串口扫描/参数/收发/HEX·ASCII/时间戳/BCC/异常提示 | ✅ 真实设备验收通过 |
| S2 指令库 | 分组树/搜索/增删改查/JSON 持久化/内置保护/恢复默认 | ✅ 真实设备验收通过 |
| S3 自动发送 | 选指令/间隔/次数/检测/超时/启停/实时统计 | ✅ 真实设备验收通过 |
| S4 检测统计 | 精确匹配/丢包率/正确率/总成功率/异常明细/RateBar | ✅ 真实设备验收通过 |
| S5 日志与稳定 | 异步日志/CSV 导出/全量日志开关/配置记忆/界面限条 | ✅ 验收通过 |
| V1.1 增强 | 回溯重渲染/导入导出/ETA/单发/趋势曲线/ASCII 转义/固定节拍 | ✅ 编译通过，待功能验证 |
| Code Review | 6 个真 bug + 4 项清理已修复 | ✅ 完成 |
| 打包 | 绿色版 `dist/serial_test_tool_v1.0_win64.zip`（21.7MB） | ✅ 完成 |
| 文档 | `00_Doc/代码导读与CppQt实战指南.html` | ✅ 完成 |

### 6.2 暂缓验收（用户确认暂缓，非阻塞）

- S5 配置记忆重启恢复验证
- 千万次长稳压力测试
- S1 拔串口保护
- S3 关检测固定节拍模式

### 6.3 已知遗留问题

| 编号 | 描述 | 影响 | 建议 |
|---|---|---|---|
| #2 | ping-pong 无序号关联，设备回复接近超时时可能误判 | 低，属设计边界 | 后续版本可改为带序号的请求-响应关联 |
| — | 默认指令库 132 条未逐条人工核对 | 低，占位/不建议命令已 enabled=false 并注明 | 用户可逐条核查 |

---

## 7. 开发约束（硬性遵守）

1. **决策不回退**：第 3 节 D-01~D-08 是已确认决策，要改必须先记录原因并征得用户同意。
2. **统计口径唯一**：所有比率计算只用第 5 节定义。
3. **线程纪律**：V1 自动发送引擎在 UI 线程事件驱动，不得用阻塞循环/sleep；统计内存实时累加，界面按定时器刷新，禁止"收一条刷一次"。
4. **计数用 64 位**：所有次数计数器用 `quint64`/`qint64`，不得用 int。
5. **不写死协议语义**：核心收发/匹配代码不得内置任何具体命令语义。
6. **中文面向用户**：界面文案、日志、指令说明一律中文；代码标识符用英文。
7. **异常不崩溃**：串口打开失败、写失败、设备断开、非法输入、日志写失败，全部要有明确提示且程序存活。
8. **参考工程只读**：`reference/upper_upgrade` 不改动，只拷贝/借鉴。

---

## 8. 打包注意事项

```powershell
# Release 编译
cmake -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release

# 拷贝 exe 到独立目录，windeployqt 收依赖
windeployqt --compiler-runtime --no-translations <exe路径>

# 打 zip
Compress-Archive -Path dist\serial_test_tool -DestinationPath dist\serial_test_tool_v1.0_win64.zip
```

> ⚠️ **坑**：`windeployqt` 在 MinGW 下**不能传 `--release`**，否则把插件误判为 debug，运行时报 "Unable to find the platform plugin"。让它自动匹配即可。PATH 里只放 6.5.3 的 bin，避免与其它 Qt 版本冲突。

---

## 9. 后续可选方向（待与用户讨论确认）

> 以下仅为记录，不代表必须做。每次用户提出新需求时，在此节追加条目并标注状态。

| 方向 | 描述 | 状态 |
|---|---|---|
| 长稳压力测试 | 千万次级别运行，验证内存/UI/日志稳定性 | 待用户触发 |
| 配置记忆回归 | 重启后验证 QSettings 恢复是否正确 | 待用户触发 |
| 安装包 | 用 Inno Setup 制作传统安装程序 | 待用户决定 |
| 工作线程化 | 将 AutoSendEngine 移到独立工作线程 | 列为后续硬化项 |
| 多指令脚本 | 多条指令按顺序/条件组合执行 | 不在 V1 范围 |
| 多串口支持 | 同时测试多个串口设备 | 不在 V1 范围 |

---

## 10. 更新记录

> 格式：日期 | 类别 | 改动摘要（最新在上）

| 日期 | 类别 | 改动摘要 |
|---|---|---|
| 2026-07-03 | 文档 | 创建本文件 DEVELOPMENT_GUIDE.md，整合 CLAUDE.md 内容，面向所有 AI 助手的项目上下文文件 |
| 2026-06-05 | 打包 | 产出绿色版 `dist/serial_test_tool/` 与 zip（21.7MB），Release 编译。记录 windeployqt+MinGW 坑 |
| 2026-06-05 | V1.1 增强 | 8 项功能增强（D-08）：监视回溯重渲染、指令库导入导出、ETA/剩余、单发、趋势曲线、ASCII 转义、固定节拍模式。编译通过，待功能验证 |
| 2026-06-05 | Code Review | 修真 bug #1(isxdigit UB)/#3(中断写summary)/#4(首次save检查)/#5(日志句柄)/#6(recordSend顺序)；清理 #7~#10 |
| 2026-06-05 | S5 验收 | S5 核心验收通过：summary/exception/full 日志落盘、结果页导出 CSV。第一版功能全部完成 |
| 2026-06-04 | S1~S4 | S1~S4 全部真实设备验收通过；S4 加 RateBar 图形化百分比条 |
| 2026-06-04 | 工程搭建 | 搭建工程骨架，实现 S0~S2，默认指令库 132 条，gen_default_commands.py 生成 |

---

## 11. 文件索引

| 文件 | 用途 |
|---|---|
| `CLAUDE.md` | 原 Claude 专用开发指南（历史文档，内容与本文件有重叠） |
| `DEVELOPMENT_GUIDE.md`（本文件） | AI 通用开发指引，**以本文件为准** |
| `00_Doc/serial_protocol_summary.md` | 设备串口协议完整说明（新/旧/旧扩展） |
| `00_Doc/串口测试工具软件功能需求表_V1.1.md` | 软件功能需求表 |
| `00_Doc/代码导读与CppQt实战指南.html` | 面向 C/单片机工程师的代码导读 + 学习路线 |
| `01_Software/reference/upper_upgrade/` | 参考工程（只读，不可修改） |
| `01_Software/dist/` | 打包产出目录 |
