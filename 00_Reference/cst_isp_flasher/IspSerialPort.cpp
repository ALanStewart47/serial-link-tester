#include "IspSerialPort.h"

IspSerialPort::IspSerialPort(QObject *parent)
    : IIspTransport(parent)
{
    connect(&m_serial, &QSerialPort::readyRead, this, &IspSerialPort::onReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &IspSerialPort::onErrorOccurred);
}

bool IspSerialPort::open(const QString &portName, qint32 baudRate, QString *errorMessage)
{
    if (m_serial.isOpen()) {
        m_serial.close();
    }

    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(QSerialPort::Data8);
    m_serial.setParity(QSerialPort::NoParity);
    m_serial.setStopBits(QSerialPort::OneStop);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial.open(QIODevice::ReadWrite)) {
        if (errorMessage != nullptr) {
            *errorMessage = m_serial.errorString();
        }
        emit portStateChanged(false);
        return false;
    }

    emit logMessage(QStringLiteral("串口已打开：%1，波特率 %2，8N1，无流控").arg(portName).arg(baudRate));
    emit portStateChanged(true);
    return true;
}

void IspSerialPort::close()
{
    if (m_serial.isOpen()) {
        const QString name = m_serial.portName();
        m_serial.close();
        emit logMessage(QStringLiteral("串口已关闭：%1").arg(name));
    }
    emit portStateChanged(false);
}

bool IspSerialPort::isOpen() const
{
    return m_serial.isOpen();
}

QString IspSerialPort::portName() const
{
    return m_serial.portName();
}

bool IspSerialPort::sendRaw(const QByteArray &bytes, QString *errorMessage)
{
    if (!m_serial.isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("串口未打开");
        }
        return false;
    }

    const qint64 written = m_serial.write(bytes);
    if (written != bytes.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("串口写入失败：%1").arg(m_serial.errorString());
        }
        return false;
    }

    // 原始发送日志：短帧打完整 hex，长数据帧只摘要，避免刷屏。
    if (bytes.size() <= 16) {
        emit logMessage(QStringLiteral("TX: %1").arg(QString::fromLatin1(bytes.toHex(' ').toUpper())));
    } else {
        emit logMessage(QStringLiteral("TX: %1 字节（数据帧，前 8 字节 %2 …）")
                            .arg(bytes.size())
                            .arg(QString::fromLatin1(bytes.left(8).toHex(' ').toUpper())));
    }
    return true;
}

bool IspSerialPort::setBaudRate(qint32 baudRate)
{
    if (!m_serial.setBaudRate(baudRate)) {
        emit logMessage(QStringLiteral("切换波特率失败：%1").arg(m_serial.errorString()));
        return false;
    }
    emit logMessage(QStringLiteral("波特率切换为 %1").arg(baudRate));
    return true;
}

void IspSerialPort::onReadyRead()
{
    const QByteArray chunk = m_serial.readAll();
    if (!chunk.isEmpty()) {
        emit logMessage(QStringLiteral("RX: %1").arg(QString::fromLatin1(chunk.toHex(' ').toUpper())));
        emit bytesReceived(chunk);
    }
}

void IspSerialPort::onErrorOccurred(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError) {
        return;
    }

    const QString reason = QStringLiteral("串口异常：%1").arg(m_serial.errorString());
    emit logMessage(reason);

    if (error == QSerialPort::ResourceError ||
        error == QSerialPort::DeviceNotFoundError ||
        error == QSerialPort::PermissionError) {
        if (m_serial.isOpen()) {
            m_serial.close();
        }
        emit portStateChanged(false);
        emit offline(reason);
    }
}
