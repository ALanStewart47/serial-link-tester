#pragma once

#include <QString>

// 扫描结果写入设备表后的选中策略：一台自动选中，多台必须由操作员点选。
enum class ScanSelectionAction {
    SelectNewRow,
    ClearSelection,
    KeepSelection,
};

inline ScanSelectionAction scanSelectionAfterUpsert(int previousRowCount, bool isNewRow)
{
    if (!isNewRow) {
        return ScanSelectionAction::KeepSelection;
    }
    if (previousRowCount <= 0) {
        return ScanSelectionAction::SelectNewRow;
    }
    return ScanSelectionAction::ClearSelection;
}

enum class TcpLinkState {
    Idle,
    Connecting,
    Connected,
    WaitingPowerOn,
};

// 等待上电时也跟当前 socket：已连通 / 连接中 / 尚未连上。不要用等待任务把真实 TCP 状态盖掉。
inline TcpLinkState tcpLinkStateForUi(bool powerOnWaiting, bool connected, bool connecting)
{
    if (connected) {
        return TcpLinkState::Connected;
    }
    if (connecting) {
        return TcpLinkState::Connecting;
    }
    if (powerOnWaiting) {
        return TcpLinkState::WaitingPowerOn;
    }
    return TcpLinkState::Idle;
}

// 顶栏只显示连接状态和设备 ID，不带 IP:端口，避免长 ID 把窗口撑宽。
inline QString formatTcpStatus(TcpLinkState state, int discoveredCount, const QString &id)
{
    switch (state) {
    case TcpLinkState::WaitingPowerOn:
        return id.isEmpty()
                   ? QStringLiteral("TCP：等待上电，自动连接中…")
                   : QStringLiteral("TCP：等待上电，自动连接 %1").arg(id);
    case TcpLinkState::Connected:
        return id.isEmpty()
                   ? QStringLiteral("TCP：已连接")
                   : QStringLiteral("TCP：已连接 · %1").arg(id);
    case TcpLinkState::Connecting:
        return id.isEmpty()
                   ? QStringLiteral("TCP：连接中…")
                   : QStringLiteral("TCP：连接中… · %1").arg(id);
    case TcpLinkState::Idle:
        if (!id.isEmpty()) {
            return QStringLiteral("TCP：未连接 · 已选：%1").arg(id);
        }
        if (discoveredCount > 0) {
            return QStringLiteral("TCP：未连接 · 请在列表中选择设备");
        }
        return QStringLiteral("TCP：未连接");
    }
    return QStringLiteral("TCP：未连接");
}
