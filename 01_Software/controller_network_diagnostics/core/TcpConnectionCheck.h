#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <functional>

// One socket and one attempt at a time. Never sends application payloads.
class TcpConnectionCheck : public QObject
{
    Q_OBJECT
public:
    struct Request {
        QHostAddress target;
        quint16 port = 6600;
        QHostAddress source; // Empty: system routing; explicit source is bound before connecting.
        QByteArray deviceMac;
        int timeoutMs = 2000, maxAttempts = 3, retryDelayMs = 250, observeMs = 300;
    };
    using ConnectOperation = std::function<void(QTcpSocket &, const QHostAddress &, quint16)>;
    explicit TcpConnectionCheck(QObject *parent = nullptr);
    // Deterministic no-network connector injection for timeout/cancellation tests.
    TcpConnectionCheck(ConnectOperation connector, QObject *parent);
    bool start(const Request &request);
    void cancel();
    bool busy() const { return m_busy; }
    QJsonObject report() const;
signals:
    void progress(const QString &message);
    void finished(const QString &outcome, const QString &message);
private:
    void beginAttempt();
    void resolveAttempt(const QString &outcome, const QString &message, bool retry = true);
    void finish(const QString &outcome, const QString &message);
    Request m_request;
    ConnectOperation m_connector;
    QTcpSocket m_socket;
    QTimer m_timeout, m_retry, m_observe;
    QElapsedTimer m_elapsed;
    QDateTime m_started, m_ended;
    QJsonArray m_attempts;
    QJsonObject m_current;
    QString m_outcome, m_message;
    bool m_busy = false, m_inAttempt = false, m_connected = false, m_binding = false;
    int m_attemptNumber = 0;
    quint64 m_receivedBytes = 0;
    QJsonObject m_existingConnection;
};
