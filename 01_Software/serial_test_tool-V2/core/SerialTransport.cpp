#include "core/SerialTransport.h"

#include <QScopedValueRollback>
#include <limits>
#include <utility>

SerialTransport::SerialTransport(QObject *parent) : IByteTransport(parent)
{
    connect(&m_serial, &QSerialPort::readyRead, this, &SerialTransport::onReadyRead);
    connect(&m_serial, &QSerialPort::errorOccurred, this, &SerialTransport::onErrorOccurred);
    connect(&m_serial, &QSerialPort::bytesWritten, this, &SerialTransport::onBytesWritten);
    m_writeTimer.setSingleShot(true);
    m_writeTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_writeTimer, &QTimer::timeout, this,
            [this] { closeWithReason(QStringLiteral("串口发送超时")); });
}

bool SerialTransport::open(const QString &portName, qint32 baudRate, QSerialPort::DataBits dataBits,
                           QSerialPort::Parity parity, QSerialPort::StopBits stopBits, QString *errorMessage)
{
    if (m_closing) {
        if (errorMessage)
            *errorMessage = QStringLiteral("串口正在关闭");
        return false;
    }
    if (m_serial.isOpen()) {
        close();
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
    closeWithReason(QStringLiteral("串口已关闭"));
}

void SerialTransport::closeWithReason(const QString &reason)
{
    if (m_closing)
        return;
    const QScopedValueRollback<bool> closing(m_closing, true);
    if (isSendPending() && m_writtenBytes >= m_pendingSend.size())
        completePendingSend();
    const bool interrupted = isSendPending();
    if (interrupted && m_serial.isOpen())
        m_serial.clear(QSerialPort::Output);
    if (m_serial.isOpen()) {
        const QString name = m_serial.portName();
        m_serial.close();
        emit logMessage(QStringLiteral("串口已关闭：%1").arg(name));
    }
    if (interrupted)
        failPendingSend(reason);
    emit portStateChanged(false);
}

bool SerialTransport::isOpen() const
{
    return m_serial.isOpen() && !m_closing;
}

QString SerialTransport::portName() const
{
    return m_serial.portName();
}

qint32 SerialTransport::baudRate() const
{
    return m_serial.baudRate();
}

QSerialPort::DataBits SerialTransport::dataBits() const
{
    return m_serial.dataBits();
}

QSerialPort::Parity SerialTransport::parity() const
{
    return m_serial.parity();
}

QSerialPort::StopBits SerialTransport::stopBits() const
{
    return m_serial.stopBits();
}

QString SerialTransport::paramsText() const
{
    QString parity = QStringLiteral("N");
    switch (m_serial.parity()) {
    case QSerialPort::EvenParity:
        parity = QStringLiteral("E");
        break;
    case QSerialPort::OddParity:
        parity = QStringLiteral("O");
        break;
    default:
        break;
    }
    const int stop = (m_serial.stopBits() == QSerialPort::TwoStop) ? 2 : 1;
    return QStringLiteral("%1 %2%3%4")
        .arg(m_serial.baudRate())
        .arg(static_cast<int>(m_serial.dataBits()))
        .arg(parity)
        .arg(stop);
}

bool SerialTransport::send(const QByteArray &data, QString *errorMessage)
{
    return requestSend(0, data, 0, errorMessage);
}

bool SerialTransport::requestSend(quint64 id, const QByteArray &data, int replyTimeoutMs,
                                  QString *errorMessage)
{
    Q_UNUSED(replyTimeoutMs);
    if (!isOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("串口未打开");
        }
        return false;
    }
    if (isSendPending() || data.isEmpty()) {
        if (errorMessage)
            *errorMessage = isSendPending() ? QStringLiteral("串口上一帧尚未发送完成")
                                            : QStringLiteral("没有要发送的数据");
        return false;
    }

    m_pendingSend = data;
    m_sendId = id;
    ++m_writeGeneration;
    m_writtenBytes = 0;
    const qint64 startUs = ioMonotonicUs();
    const qint64 written = m_serial.write(data);
    if (!isSendPending())
        return true; // 同步错误已送达调用方。
    if (written != data.size()) {
        const QString error = QStringLiteral("串口写入失败：%1").arg(m_serial.errorString());
        if (written > 0) {
            m_serial.clear(QSerialPort::Output);
            m_serial.close();
        }
        failPendingSend(error);
        if (!m_serial.isOpen())
            emit portStateChanged(false);
        return true;
    }
    // Windows 串口可在整帧写完后才报告进度，按波特率计入合法的传输时间。
    const int stopHalfBits = stopBits() == QSerialPort::TwoStop          ? 4
                             : stopBits() == QSerialPort::OneAndHalfStop ? 3
                                                                         : 2;
    const int halfBits = 2 * (1 + dataBits() + (parity() != QSerialPort::NoParity)) + stopHalfBits;
    const qint64 rate = qMax(1, baudRate());
    const qint64 airtimeMs = (qint64(data.size()) * halfBits * 500 + rate - 1) / rate;
    m_writeTimer.start(int(qMin<qint64>(airtimeMs + 5000, std::numeric_limits<int>::max())));
    emit sendStarted(id, startUs);
    return true;
}

void SerialTransport::limitPendingSend(quint64 id, int waitMs)
{
    if (isSendPending() && m_sendId == id && m_writeTimer.remainingTime() > waitMs)
        m_writeTimer.start(waitMs);
}

void SerialTransport::onBytesWritten(qint64 size)
{
    if (!isSendPending())
        return;
    m_writtenBytes += size;
    if (m_writtenBytes < m_pendingSend.size())
        return;
    m_writeTimer.stop();
    const auto generation = m_writeGeneration;
    QMetaObject::invokeMethod(
        this,
        [this, generation] {
            if (generation != m_writeGeneration || !isSendPending())
                return;
            completePendingSend();
        },
        Qt::QueuedConnection);
}

void SerialTransport::completePendingSend()
{
    const auto data = std::exchange(m_pendingSend, {});
    const auto id = m_sendId;
    emit bytesSent(data);
    emit sendCompleted(id);
}

void SerialTransport::failPendingSend(const QString &reason)
{
    if (!isSendPending())
        return;
    m_writeTimer.stop();
    m_pendingSend.clear();
    emit sendFailed(m_sendId, m_writtenBytes, reason);
}

void SerialTransport::onReadyRead()
{
    const QByteArray data = m_serial.readAll();
    if (!data.isEmpty()) {
        m_lastRx = data;
        emit bytesReceived(data);
    }
}

void SerialTransport::onErrorOccurred(QSerialPort::SerialPortError error)
{
    if (m_closing || error == QSerialPort::NoError) {
        return;
    }

    const QString reason = QStringLiteral("串口异常：%1").arg(m_serial.errorString());
    emit logMessage(reason);

    if (error == QSerialPort::ResourceError || error == QSerialPort::DeviceNotFoundError
        || error == QSerialPort::PermissionError || error == QSerialPort::WriteError) {
        closeWithReason(reason);
        emit offline(reason);
    }
}
