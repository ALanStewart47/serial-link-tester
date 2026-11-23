#pragma once

#include "core/AutoSendEngine.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>

class QFile;
class QTimer;

struct LogFileWrite
{
    QString path;
    QByteArray header;
    QByteArray data;
    bool replace = false;
};

// 文件句柄只在日志工作线程中使用。
class LogWriter : public QObject
{
    Q_OBJECT
public:
    ~LogWriter() override;
    void writeBatch(QList<LogFileWrite> files, bool finish);

signals:
    void batchFinished(const QHash<QString, QString> &errors);

private:
    QString append(const LogFileWrite &file);
    QString saveSnapshot(const LogFileWrite &file);
    void closeAll();

    QHash<QString, QFile *> m_files;
};

class LogManager : public QObject
{
    Q_OBJECT

public:
    explicit LogManager(QObject *parent = nullptr);
    ~LogManager() override;

    void setLogDir(const QString &dir);

    QString logDir() const { return m_logDir; }

    QString statusText() const { return m_status; }

    QString lastCompletionError() const { return m_lastCompletionError; }

    bool isBusy() const { return m_sessionActive || m_finishing; }

    qsizetype outstandingDetailBytes() const { return m_inFlightBytes + m_fullBuf.size() + m_excBuf.size(); }

    void beginSession(const QString &commandName, const QString &sendData, const QString &planJson = {});
    void beginSession(const QString &commandName, const QString &sendData, const QString &planJson,
                      bool fullLog);
    void endSession(const QString &summaryText);
    void logRound(quint64 index, AutoSendEngine::Outcome outcome, qint64 respUs, const QByteArray &actual,
                  const QString &commandName, quint64 receivedBytes);
    void logException(quint64 index, bool timeout, const QByteArray &actual, const QString &commandName,
                      quint64 receivedBytes);

signals:
    void message(const QString &text);
    void statusChanged(const QString &text);
    void idle();

private slots:
    void flush();

private:
    enum class Batch { None, Details, Final };
    void acceptDetail(const QString &path, QByteArray &buffer, const QString &line);
    void dispatch();
    void submit(QList<LogFileWrite> writes, Batch batch);
    void onBatchFinished(const QHash<QString, QString> &errors);
    void recordFailure(const QString &path, const QString &reason);
    QString failureText() const;
    void publishStatus(const QString &text);

    QThread m_thread;
    LogWriter *m_writer = nullptr;
    QTimer *m_flushTimer = nullptr;

    QString m_logDir;
    QString m_sessionDir;
    QString m_fullPath;
    QString m_excPath;
    QString m_summaryPath;
    QString m_planPath;
    QString m_status;
    QString m_lastCompletionError;
    QByteArray m_planBytes;
    QByteArray m_summaryPrefix;
    QString m_summaryText;
    QByteArray m_fullBuf;
    QByteArray m_excBuf;
    QHash<QString, QString> m_failures;

    Batch m_batch = Batch::None;
    qsizetype m_inFlightBytes = 0;
    bool m_fullEnabled = false;
    bool m_sessionActive = false;
    bool m_finishing = false;
    bool m_createFiles = false;
    bool m_flushRequested = false;
    bool m_showTrackedSession = false;
    bool m_destructing = false;
};
