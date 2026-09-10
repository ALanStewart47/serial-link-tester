#pragma once

#include <QString>

// 全局软件配置（QSettings 持久化，需求 FR-501~505）。
// 用固定 org/app 名，独立于 AppDataLocation（指令库路径），避免相互影响。
// QSettings 不可拷贝/移动，故由调用方自行构造：
//     QSettings s(AppConfig::Org(), AppConfig::App());
namespace AppConfig {

inline QString Org() { return QStringLiteral("SerialTestTool"); }
inline QString App() { return QStringLiteral("SerialTestTool"); }

// —— 键名 ——
namespace Key {
constexpr auto LogDir        = "log/dir";
constexpr auto FullLog       = "log/fullEnabled";

constexpr auto PortName      = "serial/portName";
constexpr auto Baud          = "serial/baud";
constexpr auto DataBits      = "serial/dataBits";
constexpr auto Parity        = "serial/parity";
constexpr auto StopBits      = "serial/stopBits";
constexpr auto DisplayFormat = "serial/displayFormat"; // 0 hex 1 ascii
constexpr auto Timestamp     = "serial/timestamp";
constexpr auto SendFormat    = "serial/sendFormat";    // 0 hex 1 ascii
constexpr auto SendBcc       = "serial/sendBcc";

constexpr auto AutoInterval  = "auto/intervalMs";
constexpr auto AutoCount     = "auto/count";
constexpr auto AutoDetection = "auto/detection";
constexpr auto AutoTimeoutMode = "auto/timeoutMode"; // 0 auto 1 manual
constexpr auto AutoTimeoutMs = "auto/timeoutMs";
constexpr auto AutoCommandId = "auto/commandId";
constexpr auto AutoRecentIds = "auto/recentIds";
constexpr auto AutoFavoriteIds = "auto/favoriteIds";
constexpr auto AutoStopMode  = "auto/stopMode";      // 0 count 1 duration
constexpr auto AutoDurationMs = "auto/durationMs";
constexpr auto PassMinSuccess = "auto/passMinSuccess"; // percent, default 99
constexpr auto PassMaxLoss   = "auto/passMaxLoss";     // percent, default 1
} // namespace Key

} // namespace AppConfig
