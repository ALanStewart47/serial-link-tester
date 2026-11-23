#pragma once
#include <QCoreApplication>
#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace AppIdentity {
inline const QString Organization = QStringLiteral("ControllerNetworkDiagnostics");
inline const QString Application = QStringLiteral("ControllerNetworkDiagnostics");
inline const QString DisplayName = QStringLiteral("控制器网络诊断工具");
inline void configure()
{
    QCoreApplication::setOrganizationName(Organization);
    QCoreApplication::setApplicationName(Application);
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
}
inline QString reportDirectory()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("reports"));
}
} // namespace AppIdentity
