# 响应采集审查复现程序

此程序用于验证 V0.2.3 网口响应采集边界，直接编译当前工程的
`NetworkTransport`、`AutoSendEngine`、`TestStatistics`、收发监视及结果页面。
不修改产品源码，不连接 CH9121；所有业务字节仅发送到 `127.0.0.1` 的临时端口。
QSettings 使用临时 INI 目录，不写正式软件设置。

回显端运行于独立线程；TCP socket与采样运行在产品的专用I/O线程，UI仅接收带时间戳的数据。
比较 1/5/10/50ms 间隔，有/无收发监视及结果页面；普通匹配与完整窗口匹配。
每条用例用三条六字节指令轮询，其期望回复等于发送内容，仅用于本机回显。

三个控制用例在第20轮同步 `bytesSent` 回调中忙等7ms或15ms。
忙等只存在于此诊断程序，用于验证UI延后不会进入响应样本，也不会把已在I/O
边界到达的回复误判为超时。控制用例不是正常软件会固定阻塞相同时长的证据。

输出：

- `response-audit-summary.txt`：每用例计数和响应极值。
- `response-audit.csv`：跨线程共享 Windows QPC 时钟的逐轮边界时刻，单位μs。
- `response-audit-stderr.txt`：Qt诊断输出。

CSV 的 `tx_signal_us` 在 UI 收到整帧发送确认的通知时采样。产品响应起点由 I/O 线程
在首次写入时记录。整帧确认通知可能晚于回显提交，因此回显相对 `tx_signal_us` 的
差值可能为负。`tx_slot_end_us` 是诊断发送通知回调的结束时刻。
`server_reply_flushed_us` 表示服务端已提交回显，
不是客户端网卡/内核接收时间。`round_completed_us` 包含匹配、统计及信号开销，
不等于精确的首字节采样时刻；`response_us` 才是产品记录的首批数据回调响应。
故这些差值用于定位可见的软件处理延后，不能当作设备处理时间或线缆RTT。

程序核对每用例最大成功样本与统计最大值一致，并核对控制轮的回显已在阻塞
结束前提交、响应保持I/O边界值且未被误判超时。退出0代表这些审查断言通过，
不代表设备时序或原生界面验收。

从仓库根目录运行（Qt路径按本机安装调整）：

```powershell
$env:PATH = 'D:\QT\Tools\mingw1120_64\bin;D:\QT\6.5.3\mingw_64\bin;D:\QT\Tools\Ninja;' + $env:PATH
cmake -S 01_Software/serial_test_tool/tools/response_audit -B 01_Software/serial_test_tool/build-response-audit -G Ninja -DCMAKE_PREFIX_PATH=D:/QT/6.5.3/mingw_64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_MAKE_PROGRAM=D:/QT/Tools/Ninja/ninja.exe -DCMAKE_CXX_COMPILER=D:/QT/Tools/mingw1120_64/bin/g++.exe
cmake --build 01_Software/serial_test_tool/build-response-audit
Set-Location 01_Software/serial_test_tool/build-response-audit
.\response_audit.exe -platform offscreen 2> response-audit-stderr.txt | Tee-Object -FilePath response-audit-summary.txt
```
