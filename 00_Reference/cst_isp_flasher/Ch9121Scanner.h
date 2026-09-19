#pragma once

// CH9121 设备发现：UDP 广播扫描局域网内的 CH9121/W5500 仿真设备，并读取/写入配置。
//
// 配置协议与 tools/upper_upgrade 的 W5500ConfigProtocol 一致（CH9121 兼容协议）：
//   - UDP 设备端口 50000，本机端口 60000（绑 AnyIPv4 以接收 255.255.255.255 广播应答）
//   - 出站网卡用 QNetworkDatagram::setInterfaceIndex 指定，避免多网卡发错口
//   - 285 字节固定包，无 CRC
//   - 0x04 Search → 0x84 SearchAck；0x02 Get → 0x82 GetAck；0x01 Set → 0x81 SetAck
// 扫描到设备后可取出其 IP，用 IspNetworkTransport 建立 TCP 透传连接（默认端口 6600）。

#include "W5500ConfigProtocol.h"

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QTimer>
#include <QUdpSocket>
#include <QVector>

class Ch9121Scanner : public QObject
{
    Q_OBJECT

public:
    struct InterfaceInfo {
        QString name;
        QString displayName;
        QHostAddress address;
        QHostAddress broadcast;
        QByteArray mac;
        int index = 0; // QNetworkInterface::index()，用于指定出站网卡
    };

    explicit Ch9121Scanner(QObject *parent = nullptr);

    // 枚举本机可用网卡（IPv4、有广播地址），并自动选中第一块。
    void refreshInterfaces();

    // 本机可用网卡（IPv4、有广播地址）。
    QVector<InterfaceInfo> availableInterfaces() const;

    // 选择用于扫描的网卡；index 越界则忽略。
    void setInterface(int index);

    // 启动一次广播扫描；约 1.5 秒后 emit scanFinished()。
    void startScan();

    // 请求选中设备的完整配置（回调 configReceived）。
    void requestConfig(const QByteArray &deviceMac);

signals:
    void interfacesRefreshed();
    void deviceDiscovered(const W5500Config::DeviceSummary &device);
    void scanFinished();
    void configReceived(const W5500Config::DeviceConfig &config);
    void logMessage(const QString &message);

private slots:
    void onUdpReadyRead();
    void onScanFinishTimer();

private:
    bool bindSocket();
    bool sendSearchPacket();

    QUdpSocket m_udpSocket;
    QTimer m_scanFinishTimer;
    QVector<InterfaceInfo> m_interfaces;
    int m_selectedInterface = -1;
    QByteArray m_pcMac;
    QHash<QString, W5500Config::DeviceSummary> m_devices; // 按 MAC 去重
};
