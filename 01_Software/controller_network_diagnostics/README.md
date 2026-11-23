# 控制器网络诊断工具

独立 Windows 应用，Qt 6.5.3 / C++17 / MinGW 64 位。程序名 `controller_network_diagnostics.exe`。

## 当前可用：阶段 N0 / N1 / N2 / N3（软件验证）

填写控制器 IPv4 或先选定网卡点击“扫描控制器”，从发现列表选择设备。可以分别执行“检查本机网络”和“检查 TCP”，查看各层证据，并导出 JSON 报告。TCP默认端口6600；超时和最多尝试次数可调，运行中可停止。

“检查本机网络”只读取本机状态；扫描发送只读CH9121发现请求（UDP本地60000→设备50000），不读写配置；TCP检查建立短连接、观察300ms后结束，不发送业务指令。扫描端口被其他工具占用时会提示。扫描不到可以手填IP继续检查。

选定网卡/源IP后，TCP绑定该源地址；“自动”使用系统路由。报告区分本机预计出口与连接实际源地址。多网卡出口不一致提示核对，不直接断言路由错误。IP所在子网不同并不一定错误，直连无需默认网关；路由API读取失败按“未确认”处理。

发现MAC/IP仅为发现记录，TCP不认证MAC；同MAC地址变化会留证据。TCP成功只证明连接建立并完成短时观察，不能证明MCU回复正常；拒绝、超时或立即断开也不能单独断言被其他软件占用。每次尝试保留，恢复不覆盖先前失败。本机若已有程序连接同一目标IP/端口，检查会显示进程/PID并停止创建第二条连接；这仍不证明设备只允许单连接。

界面导出报告schema_version=2，包含target、local_path_before_connection（连接前快照）、discovery、tcp_check及protocol_validated=false；没有执行的层不生成结果。命令行本机检查仍返回原本机报告格式。扫描、TCP不并发运行，检查期间锁定目标/参数。

## N3只读指令诊断

点击“指令库与诊断选择”打开独立132条出厂库副本，也可导入原工具导出的JSON数组。导入写入本应用自己的commands.json；原文件不被回写。结构错误、重复ID、超限文件导入失败时保留当前库；报文内容错误的条目可查看，但禁止选择。

填写目标型号/固件、协议、通道数和支持功能，并确认这些内容适用于目标后，可以勾选已审核的只读查询。实际发送字节决定资格，名称、ID、function_group或导入的批准标志不能放行写命令。修改档案会取消兼容确认并清空旧选择。新协议CA/CC、旧ASCII、旧扩展@分别筛选，不自动跨协议。

“保存本次诊断选择”生成prepared_not_executed快照；回到主窗口点击“开始指令诊断”才会执行。执行顺序是所选查询，失败后重新检查本机路径，再尝试同档案下其他适用的只读查询；若得到有效基准回复，最后复测基准与原失败指令。写入、复位、恢复出厂和保存Flash等命令不会进入自动集合。总时限、取消、未执行原因和原始失败均保留。

整次诊断尽量复用一条持久TCP会话，断线后才有限重连。每条指令使用发送前安静窗口和完整响应窗口；窗口外迟到字节单独保存，不能匹配下一条指令。每次新连接前重新检查本机是否已有同目标会话，避免与原串口工具争用。TCP建连失败发生在任何协议字节发送之前，报告会明确写明与新/旧协议类型无关；未建立连接时不再显示伪造的“源:0”。

回复校验核心已实现：按完整响应窗口校验命令/通道、长度、ASCII格式或BCC，区分没有回复、无效回复、有效动态回复、合法错误帧、期望值不符与匹配成功。没有期望值仅代表可以验证协议结构，不代表数据值正确；无事务序号仍不能排除同命令迟到回复或回显。详见 `PROTOCOL_AUDIT.md`。

## 数据隔离

- 独立源码目录和 CMake，不依赖 `../serial_test_tool`。
- 独立应用/组织身份 `ControllerNetworkDiagnostics`，不读写原工具 `SerialTestTool/SerialTestTool` 设置。
- 默认报告目录为本应用的 Windows 本地应用数据目录下 `reports`，导出时可另选位置。
- 后续指令库使用导入副本；不会与原软件共用可写数据。
- 当前版本是0.1.0开发版，原“串口测试工具 V0.2”和发行包不受影响。

## 编译与测试

在此工程目录用 PowerShell 执行：

```powershell
$env:Path = "D:/QT/6.5.3/mingw_64/bin;D:/QT/Tools/mingw1120_64/bin;D:/QT/Tools/CMake_64/bin;D:/QT/Tools/Ninja;" + $env:Path
$env:QT_PLUGIN_PATH = "D:/QT/6.5.3/mingw_64/plugins"
$env:QT_QPA_PLATFORM_PLUGIN_PATH = "D:/QT/6.5.3/mingw_64/plugins/platforms"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=D:/QT/6.5.3/mingw_64 -DCMAKE_CXX_COMPILER=D:/QT/Tools/mingw1120_64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure
windeployqt --no-translations --no-opengl-sw --no-system-d3d-compiler build/controller_network_diagnostics.exe
```

windeployqt 不加 `--release`。部署后双击 `build/controller_network_diagnostics.exe`。

命令行只读检查（不发送目标报文）：

```powershell
./build/controller_network_diagnostics.exe --inspect-local 192.0.2.1 | Out-File local-check.json -Encoding utf8
```

该命令退出码表示程序执行，不是网络连通性结论；请阅读 JSON 的 findings/status。192.0.2.1 为示例地址，替换为实际目标。路径检查通过不等于这个地址真实在线。

## 验证范围

自动测试覆盖无效目标、回环/本机地址、直连无网关、跨网段路由、网卡禁用/移除/无IPv4、多网卡出口差异、路由API失败、源地址快照变化、子网地址提示、JSON证据及配置隔离。包含本机回环路由的真实只读API检查；不是控制器实机验收。

N2测试使用本机UDP/TCP模拟器，覆盖搜索报文/无效回复/同MAC改IP、扫描端口占用、实际源绑定、超时/取消/断开/有限重试及恢复保留失败。拒绝分类由注入的socket错误确定性验证；真实本机关端口检查按观察到的拒绝或超时记录，避免假定主机一定返回RST。尚无控制器实机验收。

新增50项指令核心/UI测试覆盖132条库复制、恶意名称/标记无法放行写指令、帧长度/BCC/字段关联、动态/错误/期望值分级、档案变化清空选择、导入失败保留及原库不变。新增17项执行器测试覆盖单在途、持久连接、分片/迟到/超长/断线/总时限/取消、异常回退、基准与原失败复测，以及重连前会话占用复查；连接组增至28项。四组共120项（含初始化/清理）。完整阶段计划见仓库 `00_Doc/控制器网络诊断工具实施计划.md`；N3仍需真实控制器验收，接下来接入N4抓包与N5完整交付。
