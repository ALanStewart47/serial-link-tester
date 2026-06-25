#include "core/SerialTransport.h"

SerialTransport::SerialTransport(QObject *parent)
    : QObject(parent)
{
    connect(&m_serial, &QSerialPort::readyRead, this, &SerialTransport::onReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &SerialTransport::onErrorOccurred);
}

bool SerialTransport::open(const QString &portName,
                           qint32 baudRate,
                           QSerialPort::DataBits dataBits,
                           QSerialPort::Parity parity,
                           QSerialPort::StopBits stopBits,
                           QString *errorMessage)
{
    if (m_serial.isOpen()) {
        m_serial.close();
    }

    m_serial.setPortName(portName);
    m_serial.setBaudRate(baudRate);
    m_serial.setDataBits(dataBits);
    m_serial.setParity(parity);
    m_serial.setStopBits(stopBits);
    m_serial.setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial.open(QIODevice::ReadWrite)) {
        if (errorMessage != nullptr) {
            *errorMessage = m_serial.errorString();
        }
        emit portStateChanged(false);
        return false;
    }

    emit logMessage(QStringLiteral("串口已打开：%1，波特率 %2，无流控").arg(portName).arg(baudRate));
    emit portStateChanged(true);
    return true;
}

void SerialTransport::close()
{
    if (m_serial.isOpen()) {
        const QString name = m_serial.portName();
        m_serial.close();
        emit logMessage(QStringLiteral("串口已关闭：%1").arg(name));
    }
    emit portStateChanged(false);
}

bool SerialTransport::isOpen() const
{
    return m_serial.isOpen();
}

QString SerialTransport::portName() const
{
    return m_serial.portName();
}

bool SerialTransport::send(const QByteArray &data, QString *errorMessage)
{
    if (!m_serial.isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("串口未打开");
        }
        return false;
    }

    const qint64 written = m_serial.write(data);
    if (written != data.size()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("串口写入失败：%1").arg(m_serial.errorString());
        }
        return false;
    }
    return true;
}

void SerialTransport::onReadyRead()
{
    const QByteArray data = m_serial.readAll();
    if (!data.isEmpty()) {
        emit bytesReceived(data);
    }
}

void SerialTransport::onErrorOccurred(QSerialPort::SerialPortError error)
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
