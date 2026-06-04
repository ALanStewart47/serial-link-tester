#pragma once

#include "SerialTransport.h"
#include "UpgradeController.h"

#include <QByteArray>
#include <QMainWindow>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QProgressBar;
class QSpinBox;
class QTextEdit;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void refreshPorts();
    void togglePort();
    void chooseFirmware();
    void startUpgrade();
    void abortUpgrade();
    void appendLog(const QString &message);
    void onPortStateChanged(bool open);
    void onControllerStateChanged(UpgradeController::State state, const QString &text);
    void onProgressChanged(int percent, quint32 receivedBytes, quint32 imageSize);
    void onFinished(bool ok);

private:
    void buildUi();
    void updateActionState();
    bool parseFirmwareVersion(quint32 *version) const;
    QString selectedPortName() const;
    qint32 selectedBaudRate() const;
    quint8 selectedTargetSlot() const;

    SerialTransport m_transport;
    UpgradeController m_controller;
    QByteArray m_firmware;
    QString m_firmwarePath;

    QComboBox *m_portCombo = nullptr;
    QComboBox *m_baudCombo = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QPushButton *m_fileButton = nullptr;
    QLabel *m_fileLabel = nullptr;
    QLabel *m_crcLabel = nullptr;
    QSpinBox *m_slaveSpin = nullptr;
    QComboBox *m_slotCombo = nullptr;
    QLineEdit *m_versionEdit = nullptr;
    QPushButton *m_startButton = nullptr;
    QPushButton *m_abortButton = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QLabel *m_statusLabel = nullptr;
    QTextEdit *m_logEdit = nullptr;
};
