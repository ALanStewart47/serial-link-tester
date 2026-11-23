# 串口测试工具交接（HANDOFF）

更新日期为 2026-11-23。串口工具当前交付为 **V0.2.6 Windows 64 位绿色包**。独立网络诊断工具持续开发。

历史验收记录中的版本构建目录和原始测试输出已移出工作区。正式绿色包和多通道试用 ZIP 仍在 `01_Software/dist`。响应审计的复现步骤见 [审计程序 README](01_Software/serial_test_tool/tools/response_audit/README.md)。

## V0.2.6 稳定性修复（2026-11-23）

- 绿色目录 `01_Software/dist/serial_test_tool_v0.2.6_win64`，同目录提供同名 ZIP。31 个文件逐项核对，解压后用隔离设置独立启动通过，EXE 文件及产品版本为 `0.2.6.0`。
- 指令库与报告原子保存，日志写盘有回执和 8 MiB 明细上限。每条链路一次确认一帧，回复分片匹配并保留 4 KiB 诊断样本。旧 ASCII 计划按原字节发送，显式转义格式统一单发与自动测试；监视和异常明细批量刷新。
- 三组 QtTest 174 项通过，4,096 步计划和在途停止通过。COM3 与 CH9121 用只读查询验证收发、暂停、继续及在途软件断开。本机长稳 36 分 35.992 秒，1,000,036 帧匹配且逐行核对全量 CSV。长稳快照与最终源码补测记录见 `AGENTS.md` 当前进度。

## V0.2.5 网口扫描兼容修复（2026-11-23）

- 绿色目录 `01_Software/dist/serial_test_tool_v0.2.5_win64` 和同名 ZIP。网口扫描在所选网卡发定向广播与 `255.255.255.255`，补齐只响应后者的控制器。实际八通道控制器界面扫描 3/3，发布构建和两组 QtTest 通过。

## V0.2.4 自动测试暂停与继续（2026-11-23）

- 绿色目录为 `01_Software/dist/serial_test_tool_v0.2.4_win64`，同目录提供同名 ZIP。打开绿色目录后运行 `serial_test_tool.exe`。
- 发布构建的 QtTest 两组通过，31 个 ZIP 文件逐项校验通过，EXE 版本为 0.2.4.0。暂停、继续和实机验证见 `AGENTS.md` 当前进度。

## V0.2.3 CH9121响应采集线程硬化（2026-10-10）

- 用户要求最大值保持所有成功轮的“发送到收到首字节”最大耗时，不能减间隔、限幅或过滤大值。针对CH9121 TCP路径，`NetworkTransportWorker`独占`QTcpSocket`及事件循环；实际write/flush完成和`readyRead`进入时以同一单调微秒时钟取样，`NetworkTransport`在UI线程仅转发状态和带时间戳数据。
- `AutoSendEngine`按I/O发送/接收时间计算首字节响应，并据接收时间戳判截止；UI监视、日志、结果页仍接收原始字节，但其排队和重绘不再改变已取样的响应或把已到达回复误判为超时。最大值仍由`TestStatistics::recordMatchUs`完整保留。串口仍为原UI线程路径，未经本轮线程硬化。
- 新回归`stampedReceiveIgnoresDelayedUiDispatch`：接收时间戳后阻塞UI12ms，响应仍<3ms；最终独立loopback审计的7/15ms UI阻塞控制轮记录88～171μs、未超时，证明旧UI延迟没有混入网口样本。自然I/O线程尾延迟仍会显示为真实大样本，未被隐藏。
- V0.2.3版本/绿色包、全量回归和实机复验状态见`AGENTS.md`本轮记录；审查与审计材料见`00_Doc/响应时间采集审查_2026-10-10.md`、`01_Software/serial_test_tool/tools/response_audit`。

## V0.2.2 网口响应采集审查补充（2026-10-10，未改产品功能）

- 用户截图为CH9121/V0.2.2普通三指令轮询，间隔5ms、自动窗口10ms，响应172μs/396.2μs/9.466ms。当前正在运行的dist EXE与build-v0.2.2 EXE哈希一致。
- 当前最大值累加、清零和显示单位未发现错误；计时起点在发送调用前，采样终点在UI线程执行首批接收回调时。发送监视仍逐条同步排版，收发和引擎都在UI线程；故显示的首字节时间含主机处理延迟，不能等同设备自身响应。此前微秒/主机测试通过不证明设备计时准确。
- 独立loopback程序直接编译当前原始代码：12用例1560轮断言通过，相关19项QtTest通过。人工UI阻塞约7ms，回显46μs后已提交却记录7.210ms；人工阻塞约15ms，间隔5ms/窗口10ms判超时，间隔10ms/窗口20ms记录15.243ms，回显均约48μs后已提交。间隔未直接加进响应，但其自动窗口改变成功样本的筛选范围。
- 审查与复现见 `00_Doc/响应时间采集审查_2026-10-10.md` 和 `01_Software/serial_test_tool/tools/response_audit`。服务端 flush 时间采样于回显提交时。控制阻塞证明 UI 调度会影响旧版采样，截图中的极值和超时仍缺少逐轮证据。
- 本轮未修改产品收发/统计/UI或D-07线程模型，未替换绿色包，未发实机报文。后续优先减少监视同步排版，并在I/O边界保留时间及最大样本证据；若进行线程硬化须明确处理D-07设计变更。禁止减间隔、限制最大值或丢弃大样本来美化显示。

## 当前 V0.2.2 微秒响应显示（2026-10-03）

- 成功轮首字节响应由纳秒时钟转换为整数微秒累计，`recordMatchUs`、`roundCompleted`和`logRound`明确以微秒传递；最大值/清零及已有停止/截止口径保留。
- 最小、平均、最大分别自适应单位：小于1ms显示μs，达到1ms显示ms（毫秒三位小数）；平均微秒最多一位小数，显示舍入到1ms的边界统一切回ms。统计页、结果页、分指令表、汇总日志和报告共用格式。
- 结果页移除挤在汇总中的响应文本，新增浅蓝三栏卡片，数字强调、单位缩小；保持主窗口尺寸及滚动容器。趋势保存浮点毫秒，纵轴按范围显示μs/ms，微秒曲线可正常缩放。
- 全量CSV保留原“响应ms”列，使用三位小数，如0.850=850μs；失败/中断仍为“-”，兼容既有毫秒字段。拆卡后报告显式保留整体响应汇总。
- 应用/EXE版本0.2.2/0.2.2.0；独立构建目录`01_Software/serial_test_tool/build-v0.2.2`；绿色目录与ZIP为`01_Software/dist/serial_test_tool_v0.2.2_win64`及同名.zip。V0.2.1旧包保留。
- Qt6.5.3/MinGW Release及26+108项主机回归通过；修复前亚毫秒采样回归失败，修复后通过。中文字体offscreen渲染在720/1280宽度完整显示，源码只读复核未发现具体缺陷；31文件ZIP逐项内容、9档图标载荷与EXE版本校验通过。没有实机报文或原生窗口验收，待用户复验。V0.2.1记录保留为上一版证据。

## 上一版 V0.2.1 修复交付（2026-10-03）

- 应用版本0.2.1，底栏V0.2.1，Windows文件/产品版本0.2.1.0。当前可运行程序为 `01_Software/dist/serial_test_tool_v0.2.1_win64/serial_test_tool.exe`，ZIP为该目录同名加 `.zip`；构建目录 `01_Software/serial_test_tool/build-v0.2.1`。V0.2等历史包保留，原build目录程序仍是旧版。
- 成功轮响应统一取首字节观测时间；最小/平均/最大仍只统计成功轮，最大值比较算法保留。超时使用PreciseTimer和发送时启动的单调截止时间，拒绝截止后的字节；计时为主机回调观测值，包含传输与主机调度，不能单独归因于控制器。
- 保留sendCount实际已发送量；completedCount=match+mismatch+timeout，丢包率和总成功率只用completedCount作分母。无已结算轮时显示N/A，界面/报告保留在途或中断数量；停止测试统一未完成，不判全部合格。
- 手动停止进入Stopping，只取消后续发送，当前轮沿用原截止时间；strict_frame完整窗口比较继续。等待期间UI/手动发送/链路保持锁定，结果页刷新和日志会话继续；结算后再Stopped并关日志。断链/退出abort，当前轮记录Interrupted（全量日志启用时）、已收字节与未完成量，不提前计设备timeout。
- send()内部同步offline的回归先失败；新增发送过程暂存abort原因，等send返回后先记实际接受发送及Interrupted/SentOnly，再Stopped，拒绝发送不计数。防止日志提前关闭、后续发送或终态被Finished覆盖。
- Qt6.5.3/MinGW Release、CH9121 26项及多通道/统计94项回归全部通过（含初始化/清理）；只读复核后已处理发送重入问题。EXE版本、原9档图标载荷、31文件ZIP内容及白名单校验通过。验证辅助脚本与修复前失败记录保存在build-v0.2.1，未进入绿色包。
- 测试须设置Qt安装目录platforms：旧build仅部署qwindows，直接用offscreen会启动超时。ASCII协议分片用例分别输入两段但不依赖1ms Windows定时回调；截止时间另由确定性阻塞事件分发/提前调用/停止剩余时间用例检查，生产截止未放宽。
- 没有实机报文发送、硬件/长稳或原生视觉验收；请用新目录复验首字节统计、重新开始清零、停止等待和断链。以下V0.2及旧整合记录属于历史证据，不代表本轮实机验收。

## 独立网络诊断工具（2026-09-21新增，持续开发）

用户要求网络诊断作为独立软件。新目录 `01_Software/controller_network_diagnostics`、名称“控制器网络诊断工具”、EXE `controller_network_diagnostics.exe`、设置身份 `ControllerNetworkDiagnostics/ControllerNetworkDiagnostics`，原串口工具V0.2保持。

N0/N1已完成，N2软件验证完成：本机网卡/IP/Windows路由只读检查、CH9121只读扫描/MAC地址变化、绑定源IP的TCP短连接/取消/有限重试，以及分层JSON报告。两处Release与51项测试（25+26）通过，只读复核和原生Windows界面核验通过；本轮未做控制器实机验收。新工程build下EXE已部署运行依赖。完整计划见 `00_Doc/控制器网络诊断工具实施计划.md`，原方案中的“新增原工具页面”已改为独立应用。

下一段N3指令诊断与回退；N4抓包及N5完整交付仍未完成，不能提前标记目标完成。开发工作区 `C:/Users/Administrator/.codex/worktrees/network-diagnostics/serial-link-tester`（`codex/network-diagnostics`），已验证的新源码同步回主目录；后续先核对两边文件，避免覆盖用户新改动。不把功能加入serial_test_tool，不共享可写设置或指令库。N2仅建立TCP、不发送业务字节；窗口导出schema 2使用local_path_before_connection/discovery/tcp_check分层，命令行本机报告仍为schema 1。

补充2026-09-21：独立诊断N3已有指令基础与选择窗口，**N3整体未完成**。`DiagnosticCommands`实现独立132库导入/副本保存、实际字节只读审核和完整回复分级；`CommandSelectionDialog`按明确设备档案选择，保存prepared_not_executed快照至主窗口报告。修改档案或目标清除旧选择；本轮没有业务发送。规则见独立工程PROTOCOL_AUDIT.md。两处Release、三组101项（25+26+50）通过，CC不支持错误帧回归先失败后修正；只读复核及原生选择窗口核验完成，23个源码文件已同步主目录。

补充2026-09-21：N3执行器软件实现完成，真实控制器待验收。整次诊断复用持久TCP会话，断线后有限重连；异常后本机复查、其他适用只读查询、成功基准和原失败复测均已接入。每次新连接前检查本机既有目标会话并显示进程/PID，步骤间迟到数据独立留证且不会匹配下一条。截图中的旧协议报错实际发生于TCP建连超时，业务指令尚未发送；同时修正未连接时“源:0”误导显示。工作区与主目录四组120项测试（25+28+50+17）通过，只读复核无Critical/Important；主目录依赖已部署，原生启动正常，原工具75项源码资源哈希未变。N4抓包、N5完整工作流/独立包仍待实施；不得把N3软件模拟当作控制器实机通过。原串口工具V0.2以下交付记录不变。

## 历史V0.2交付

- 用户要求去掉试验版文案，底栏左V0.2/中间链路状态/右AlanStewart，保留新图标，输出不含源码的绿色包；已完成。
- 标题“串口测试工具”，应用版本0.2，Windows文件/产品版本0.2.0.0；等宽三列底栏确保链路居中。
- 最新可运行目录：`D:/OneDrive/serial-link-tester/01_Software/dist/serial_test_tool_v0.2_win64`；ZIP为同名`.zip`，约22.76MiB。
- 构建源产物：`01_Software/serial_test_tool/build-v0.2/serial_test_tool.exe`。旧v0.1/试用包保留，仅新包体现新版标题/版本/作者排布，请勿再次打开旧试用包核验新版。
- 26+74项回归全部通过；EXE九档图标载荷与选定ICO逐字节一致；原生控件树核对新标题/底栏文案，当前桌面状态影响截图，本轮不声称完成截图视觉验收。
- ZIP完整性与文件白名单已核验：31个文件=1主程序+28 DLL+2份使用说明；无源码、测试程序或构建文件。pack_release.ps1默认名称已改为v0.2，并强制白名单校验。
- 以下为此前多通道整合的实现与验收记录，v0.1产物路径为历史记录。

## 本轮结果

用户要求“读一下handoff文档，把之前测试用的多通道功能整合到现在的正式版里面”。已按D-12逐文件整合到主工作区，未执行git提交/推送或整目录覆盖。

- 正式源码：`D:/OneDrive/serial-link-tester/01_Software/serial_test_tool`，原分支 `Claude`。
- 原试用副本：`C:/Users/Administrator/.codex/worktrees/multichannel-trial/serial-link-tester`，原有修改保留。
- 独立整合区：`C:/Users/Administrator/.codex/worktrees/multichannel-integration/serial-link-tester`，已将验证后的25个功能/测试/构建/说明文件同步回正式目录。
- 审计/回退材料：整合区上一级 `integration-audit`，含正式修改前文件、应用文件清单、哈希、原设置导出及指令库备份。恢复时按具体文件审阅，勿整库覆盖用户后续变化。
- 两边HEAD原为 `51db4e1bc06eaa47f2ca55c319d9885e35a31595`；功能仍为未提交修改和新文件。不要误以为git merge试用分支可以替代已完成的文件整合。

## 已整合范围

新CA、旧ASCII、旧扩展$/@参数生成；通道范围升序去重；设置后读回；多批追加；成对移动/删除；JSON快照保存/打开/重启恢复；遍历轮数；逐步骤/逐通道统计；完整覆盖判定；会话plan日志与报告。

生成步骤使用strict_frame，收到整个响应窗口后精确比较，每步约超时＋间隔，响应耗时记首字节。原始指令原有匹配与统计公式保留。未完成/未覆盖不能判全部通过，关闭检测为纯发送。

普通旧协议T/W亮度与ST/SW冲突保护仍有效，不静默跳过通道或换协议。ASCII不追加BCC；旧扩展提供通道开关。新协议编码1～255，旧/旧扩展A～Z为1～26，真实设备能力按型号确认。上限4096步、4MiB计划、总发送1000万。

未加入型号档案、参数扫描、CB/旧扩展多通道块打包，也未实施网络自动诊断建议稿。

## 正式身份与数据兼容

- `main.cpp`应用名仍为“串口测试工具”；QSettings仍为 `SerialTestTool/SerialTestTool`。图标PNG/ICO/RC/QRC和CH9121 GET解析/实机样本回归完整保留。
- 测试目标定义 `SERIAL_TEST_TOOL_TESTING`，QSettings按进程隔离；多通道测试数据使用Qt test路径。
- 首次从原 `auto/playlistIds` 建立完整快照（保持有效指令顺序、重复项与原匹配方式），原ID清单备份到 `auto/legacyPlaylistIdsBackup`；暂缺/禁用项提示并保留备份。
- 正式计划 `auto/playlistSnapshot`；损坏/超大快照原样保留，并写 `auto/recoverySnapshot`。
- 正式计划使用加入时的报文快照，后续改库不会自动改已有计划，应重新加入。
- 不读取试用应用的trial注册表键；在试用程序保存JSON，正式程序“打开计划”导入即可。schema_version=1、ca_v1/old_ascii_v1/old_ext_v1兼容。

## 已确认图标：后续修改必须保留

用户已于2026-09-20选定并要求使用“双向链路”图标：蓝色圆角底、白色右向箭头、青色左向箭头与尾部触点。这是正式应用标识，不是待选概念稿。后续功能修改、试用分支整合、重构、重新构建及打包均须保留，未经用户要求不要换回默认Qt图标或重新设计。

以下路径均相对正式工程 `01_Software/serial_test_tool/`：

| 文件/位置 | 必须保留的内容 |
|---|---|
| `resources/app-icon.png` | 1254×1254透明RGBA源图，供后续重新导出使用。 |
| `resources/app-icon.ico` | 16/20/24/32/40/48/64/128/256像素共9档，窗口与EXE共用。 |
| `resources/app.rc` | `IDI_APP_ICON ICON "app-icon.ico"`，提供Windows文件及快捷方式图标。 |
| `resources/resources.qrc` | 包含`app-icon.ico`，供Qt运行时读取`:/app-icon.ico`。 |
| `main.cpp` | 包含`QIcon`，在创建主窗口前调用`a.setWindowIcon(QIcon(QStringLiteral(":/app-icon.ico")))`。 |
| `CMakeLists.txt` | Windows下启用RC、将`resources/app.rc`加入程序目标、设置resources包含路径，并以`OBJECT_DEPENDS`追踪ICO变化。 |
| `resources/ICON.md` | 图标设计来源、最终提示词与重新封装ICO的方法。 |

不要仅保留PNG/ICO文件而漏掉RC、QRC、入口设置或CMake接线。若从旧分支搬运`main.cpp`、`CMakeLists.txt`或`resources.qrc`，必须逐项核对上述内容；图标已内嵌到EXE，用户运行时无需另外复制图标文件。

构建或打包后的验收：确认EXE含原生图标资源且ICO保有9档尺寸；启动**本次新生成的EXE**检查标题栏/任务栏图标，并在资源管理器检查该EXE的文件图标。不能只看旧进程或旧快捷方式，Windows可能缓存旧图标。2026-09-20已验证主工程EXE的9档图标载荷与ICO一致，原生标题栏显示正确；最新多通道正式整合也已保留此图标，历史旧包不据此视为已更新。

## 本轮修复与验证

- 独立只读审阅后修正异常证据静默截断：strict_frame诊断缓存至少4KiB（且大于期望长度），另计真实接收字节数；日志CSV新增“接收字节数/记录状态”，结果及导出明确标注截断。短错误回复完整保留。
- 修正成功恢复生成快照仍显示旧库指令缺失。
- 新增实际异步落盘回归发现快速关闭只写了表头；日志析构改为同步排空先前队列并关闭文件后，再退出工作线程。
- Qt6.5.3/MinGW Release编译通过；CH9121 26项、多通道74项通过，均含初始化/清理。新增迁移/原数据不变/重复顺序/缺失ID备份/试用JSON/恢复、完整错误回复/超长诊断计数/日志尾部落盘回归。诊断截断与日志尾部均有先失败后通过证据。
- 原生Windows：新包启动、正式标题及图标、真实旧两条轮询与收藏/历史恢复、CA多通道16步预览检查；三协议切换/生成、成对操作、恢复等Qt自动测试通过，运行锁定代码已复核。未通过UI自动化改动正式测试清单或发送设备指令。
- 正式用户commands.json启动前后SHA256一致。原有网络/串口设置保留。原始工程中其他未提交文档、`.embeddedskills`、参考工程和历史包未清除。

## 2026-09-20 历史产物与启动

- 最新正式程序：`D:/OneDrive/serial-link-tester/01_Software/serial_test_tool/build/serial_test_tool.exe`。
- 正式绿色目录：`D:/OneDrive/serial-link-tester/01_Software/dist/serial_test_tool_v0.1_multichannel_20260920_win64`。
- 正式ZIP：同目录名加 `.zip`，包含Qt/MinGW运行库、使用说明与多通道功能说明。
- 用户旧进程未中断；原build EXE备份为 `serial_test_tool_pre_multichannel_20260920.exe`，旧窗口仍是旧功能，关闭后从上述新版路径重新打开。
- 旧试用包及 `serial_test_tool_v0.1_win64.zip` 都保留。

打包脚本 `pack_release.ps1` 现在按唯一包名创建目录，已存在则拒绝覆盖；不再删除整个dist或build。先构建/ctest，再windeployqt（不要加 --release）、检查依赖并压缩。示例：

```powershell
./01_Software/serial_test_tool/pack_release.ps1 -BuildDirectory build -PackageName serial_test_tool_v0.1_日期_win64
```

包名限英文字母、数字、点、下划线、短横线。测试使用Qt安装目录platforms，避免已部署build缺少qoffscreen.dll。

## 下一步与实机边界

本次没有新增实机发送验收。既有用户证据：CH9121透传、旧ASCII A～H亮度255，16步×100轮=1600条均成功，错误/超时0；响应15/30.0/47ms。只能据此认定这条流程通过。

后续在新版重跑上述产品流程，再按需要验收I～Z、T/W固件特殊行为、旧扩展、脉宽/延时、异常、长稳和物理输出。CH9121实机GET修复已保留；SET与断电保持仍待验收。测试工程师若需试用版保存的计划，先从试用版导出JSON再由正式版打开。
