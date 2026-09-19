# cst-isp-flasher

工业控制器 Bootloader 升级上位机（Qt 6）。

- 联调踩坑、对号入座：`docs/KEY_ISSUES.md`
- 各机 MCU / FPGA 升级协议（字节级）：`docs/PROTOCOLS.md`
- 仓库结构、构建、开发环境、近期进度：`HANDOFF.md`
- 验收与发布打包：`README.md`

## 硬约束

- 只动本仓库。不要把 `TPS_FxxK_Remake` 的固件、硬件、客户 FPGA 工程拷进来。
- 构建用 **Qt 6.5.3 mingw_64**。Qt 6.10 官方包缺 SerialPort，编不过。
- 本工具是工厂软件：不要加主题切换、账号系统、自动更新。
- MCU 页默认波特率 **19200**；「与连接相同」默认勾选。不要把自定义波特率写入 QSettings。
- 网口（CH9121→USART1）App/Boot **都不能改波特率**，固定 19200。`setBaudRate` 对 TCP 是空操作。
- FPGA 直连与 MCU 不同：安路触发 19200、烧录常 115200。不要把 MCU 的「藏波特率」套到 FPGA 页。
- **v1.1**：串口/网口连接在页签上方共用，不要拆回三页各一套。六个 Controller 共用传输，切页必须 `setRxEnabled`；升级中由 `sessionBusy()` 互斥。
- 窗口版本字符串在 `IspMainWindow.cpp` 硬编码（当前 `v1.2`），升版本时改这里。
- 协议字节序、块索引、应答语义以 `HANDOFF.md` 为准，不要凭「常见 Modbus」改 CRC 或地址。
- 不写密钥、不提交 `build*` / `release/` / `*.user`。

## 改完怎么验

```powershell
$env:PATH = "D:\Qt\Tools\mingw1120_64\bin;D:\Qt\Tools\Ninja;D:\Qt\6.5.3\mingw_64\bin;$env:PATH"
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="D:/Qt/6.5.3/mingw_64" -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## 会话收尾（必须更新 HANDOFF）

用户说「测试没问题」「项目暂时结束」「项目先这样」「项目完成了」「搞好了」或同义收尾时：立刻更新 `HANDOFF.md` 的日期、完成情况、最近修改、新踩的坑、未完成项。不要另写一份新文档，也不要在用户没要求时 commit。
