#pragma once

#include "core/AutoSendEngine.h"

#include <QFile>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

// 后台写文件的工作者，运行在独立线程（NFR-004/006/407：日志异步落盘、不驻内存、不卡 UI）。
class LogWriter : public QObject
{
    Q_OBJECT
public slots:
    void writeLines(const QString &path, const QStringList &lines); // 追加写并 flush
    void closeFile(const QString &path);                            // 关闭单个文件并释放句柄
    void closeAll();

private:
    QHash<QString, QFile *> m_files; // 每个 writer 实例独立持有打开的文件句柄
};

// 日志管理（在 UI 线程被调用）。
// 统计日志 + 异常日志默认保存；全量日志可选（每轮）。
// 行先缓存在 UI 线程，按定时器成批投递到工作线程落盘，避免高频信号与内存堆积。
class LogManager : public QObject
{
    Q_OBJECT

public:
    explicit LogManager(QObject *parent = nullptr);
    ~LogManager() override;

    void setLogDir(const QString &dir);
    QString logDir() const { return m_logDir; }
    // 全量日志开关的唯一来源是 QSettings；beginSession 时读取，避免与引擎处的读取漂移（见 #8）。

    // 一次测试开始/结束
    void beginSession(const QString &commandName, const QString &sendData);
    void endSession(const QString &summaryText);

    // 运行期记录
    void logRound(quint64 index, AutoSendEngine::Outcome outcome, qint64 respMs, const QByteArray &actual);
    void logException(quint64 index, bool timeout, const QByteArray &actual);

signals:
    void writeLinesRequested(const QString &path, const QStringList &lines);
    void closeFileRequested(const QString &path);
    void closeRequested();
    void message(const QString &text); // 状态/错误提示给 UI

private slots:
    void flush();

private:
    static QString csvCell(const QString &s);

    QThread m_thread;
    LogWriter *m_writer = nullptr;

    QString m_logDir;
    bool m_fullEnabled = false;

    bool m_sessionActive = false;
    QString m_fullPath;
    QString m_excPath;
    QString m_summaryPath;
    QString m_sessionCmd;
    QString m_sessionSend;
    QString m_sessionStart;

    QStringList m_fullBuf;
    QStringList m_excBuf;
    class QTimer *m_flushTimer = nullptr;
};
