#pragma once
#include "core/DiagnosticCommands.h"
#include "core/NetworkPathCheck.h"
#include <QElapsedTimer>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <functional>
#include <optional>

class DiagnosticEngine : public QObject {
    Q_OBJECT
public:
    struct Request {
        QHostAddress target, source;
        quint16 port=6600;
        QString interfaceName;
        QByteArray discoveredMac;
        Commands::Profile profile;
        QList<Commands::Command> selected, fallback;
        int connectMs=2000, connectAttempts=3, responseMs=500, quietMs=100, totalMs=60000;
    };
    using Inspector=std::function<NetDiag::Report(const NetDiag::Request &)>;
    explicit DiagnosticEngine(QObject *parent=nullptr);
    DiagnosticEngine(Inspector inspector,QObject *parent);
    bool start(const Request &request);
    void cancel();
    bool busy() const { return m_busy; }
    QJsonObject report() const;
    static QString phaseText(const QString &phase);
    static QString outcomeText(const QString &outcome);
signals:
    void progress(const QString &message);
    void finished(const QString &outcome,const QString &message);
private:
    enum class State { Idle, Connecting, Guard, Reply, Backoff };
    struct Step { Commands::Command command; QString phase; };
    void inspectPath(bool afterFailure);
    void advance();
    void connectAttempt();
    void connected();
    void beginGuard(bool reused);
    void receive();
    void drainOutsideWindow();
    void sendCommand();
    void socketError(QAbstractSocket::SocketError error);
    void connectionFailed(const QString &outcome,const QString &message);
    void resolve(const QString &forcedOutcome={},const QString &detail={});
    void nextPhase();
    void finish(const QString &outcome,const QString &message);
    void appendSkipped(const QList<Step> &steps,const QString &reason);
    void closeSocket();
    bool ensureBudget();
    void armStep(int ms);
    void armQuiet();
    void recordUnexecuted(const QList<Commands::Command> &commands,const QString &phase,const QString &reason);
    Request m_request;
    Inspector m_inspector;
    QTcpSocket m_socket;
    QTimer m_stepTimer,m_quietTimer,m_totalTimer,m_schedule;
    QElapsedTimer m_clock,m_responseClock;
    State m_state=State::Idle;
    bool m_busy=false,m_hasCurrent=false,m_binding=false,m_afterFailureChecked=false,m_hadFailure=false;
    Step m_current;
    QList<Step> m_pending;
    QList<Commands::Command> m_failedOriginal;
    std::optional<Commands::Command> m_baseline;
    QString m_phase,m_outcome,m_message,m_fallbackState;
    QJsonArray m_results,m_pathChecks,m_connections,m_unexecuted;
    QJsonObject m_currentEvidence,m_existingConnection;
    QByteArray m_reply,m_unsolicited,m_outsideWindow;
    quint64 m_received=0,m_unexpected=0,m_outsideWindowBytes=0,m_sentBytes=0,m_sequence=0;
    qint64 m_firstByteMs=-1;
    qint64 m_stepDeadline=0,m_quietDeadline=0;
    bool m_observedAfterDeadline=false;
    int m_attempt=0;
    QDateTime m_started,m_ended;
};
