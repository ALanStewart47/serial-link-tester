#include "core/LogManager.h"

#include "core/AppConfig.h"
#include "core/CsvUtil.h"
#include "core/PacketBuilder.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QTextStream>
#include <QTimer>

// ===================== LogWriter（工作线程） =====================
void LogWriter::writeLines(const QString &path, const QStringList &lines)
{
    if (path.isEmpty() || lines.isEmpty()) {
        return;
    }
    QFile *file = m_files.value(path, nullptr);
    if (!file) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        file = new QFile(path);
        if (!file->open(QIODevice::Append | QIODevice::Text)) {
            delete file;
            return;
        }
        m_files.insert(path, file);
    }
    QTextStream out(file);
    out.setEncoding(QStringConverter::Utf8);
    for (const QString &line : lines) {
        out << line << '\n';
    }
    out.flush();
}

void LogWriter::closeFile(const QString &path)
{
    if (QFile *file = m_files.take(path)) {
        file->close();
        delete file;
    }
}

void LogWriter::closeAll()
{
    for (QFile *file : m_files) {
        file->close();
        delete file;
    }
    m_files.clear();
}

// ===================== LogManager（UI 线程） =====================
LogManager::LogManager(QObject *parent)
    : QObject(parent)
{
    m_writer = new LogWriter;
    m_writer->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_writer, &QObject::deleteLater);
    connect(this, &LogManager::writeLinesRequested, m_writer, &LogWriter::writeLines);
    connect(this, &LogManager::closeFileRequested, m_writer, &LogWriter::closeFile);
    connect(this, &LogManager::closeRequested, m_writer, &LogWriter::closeAll);
    m_thread.start();

    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(300);
    connect(m_flushTimer, &QTimer::timeout, this, &LogManager::flush);
}

LogManager::~LogManager()
{
    // 程序在测试中途关闭：补一条会话汇总，避免 summary.csv 丢失该次运行（#3）。
    if (m_sessionActive) {
        endSession(QStringLiteral("(测试中断，未正常结束)"));
    }
    flush();
    emit closeRequested();
    m_thread.quit();
    m_thread.wait();
}

void LogManager::setLogDir(const QString &dir)
{
    m_logDir = dir;
}

QString LogManager::csvCell(const QString &s)
{
    return CsvUtil::escape(s);
}

void LogManager::beginSession(const QString &commandName, const QString &sendData)
{
    if (m_logDir.isEmpty()) {
        emit message(QStringLiteral("未设置日志目录，本次测试不落盘日志"));
        m_sessionActive = false;
        return;
    }
    // 全量日志开关以 QSettings 为唯一来源，与引擎在测试开始时的读取保持一致（#8）。
    {
        QSettings s(AppConfig::Org(), AppConfig::App());
        m_fullEnabled = s.value(AppConfig::Key::FullLog, false).toBool();
    }

    const QString ts = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    m_fullPath = QStringLiteral("%1/full_%2.csv").arg(m_logDir, ts);
    m_excPath = QStringLiteral("%1/exception_%2.csv").arg(m_logDir, ts);
    m_summaryPath = QStringLiteral("%1/summary.csv").arg(m_logDir);
    m_fullBuf.clear();
    m_excBuf.clear();
    m_sessionActive = true;
    m_sessionCmd = commandName;
    m_sessionSend = sendData;
    m_sessionStart = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));

    // 异常日志表头（始终写）
    emit writeLinesRequested(m_excPath, {QStringLiteral("序号,时间,类型,实际HEX,实际ASCII")});
    // 全量日志表头（仅开启时）
    if (m_fullEnabled) {
        emit writeLinesRequested(m_fullPath, {QStringLiteral("序号,时间,结果,响应ms,实际HEX,实际ASCII")});
    }
    // summary 表头（仅文件不存在时）
    if (!QFileInfo::exists(m_summaryPath)) {
        emit writeLinesRequested(m_summaryPath,
            {QStringLiteral("开始时间,指令,发送内容,汇总")});
    }
    emit message(QStringLiteral("日志会话开始：%1").arg(m_logDir));
    m_flushTimer->start();
}

void LogManager::logRound(quint64 index, AutoSendEngine::Outcome outcome, qint64 respMs, const QByteArray &actual)
{
    if (!m_sessionActive || !m_fullEnabled) {
        return;
    }
    QString outText;
    switch (outcome) {
    case AutoSendEngine::Outcome::Match: outText = QStringLiteral("匹配成功"); break;
    case AutoSendEngine::Outcome::Mismatch: outText = QStringLiteral("回复错误"); break;
    case AutoSendEngine::Outcome::Timeout: outText = QStringLiteral("超时"); break;
    case AutoSendEngine::Outcome::SentOnly: outText = QStringLiteral("仅发送"); break;
    }
    const QString line = QStringLiteral("%1,%2,%3,%4,%5,%6")
        .arg(QString::number(index),
             QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             outText,
             respMs < 0 ? QStringLiteral("-") : QString::number(respMs),
             csvCell(PacketBuilder::toHexText(actual)),
             csvCell(PacketBuilder::toAsciiText(actual)));
    m_fullBuf.append(line);
}

void LogManager::logException(quint64 index, bool timeout, const QByteArray &actual)
{
    if (!m_sessionActive) {
        return;
    }
    const QString line = QStringLiteral("%1,%2,%3,%4,%5")
        .arg(QString::number(index),
             QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
             timeout ? QStringLiteral("超时(丢包)") : QStringLiteral("回复错误"),
             csvCell(PacketBuilder::toHexText(actual)),
             csvCell(PacketBuilder::toAsciiText(actual)));
    m_excBuf.append(line);
}

void LogManager::flush()
{
    if (!m_fullBuf.isEmpty()) {
        const QStringList batch = m_fullBuf;
        m_fullBuf.clear();
        emit writeLinesRequested(m_fullPath, batch);
    }
    if (!m_excBuf.isEmpty()) {
        const QStringList batch = m_excBuf;
        m_excBuf.clear();
        emit writeLinesRequested(m_excPath, batch);
    }
}

void LogManager::endSession(const QString &summaryText)
{
    if (!m_sessionActive) {
        return;
    }
    flush();
    m_flushTimer->stop();

    const QString line = QStringLiteral("%1,%2,%3,%4")
        .arg(csvCell(m_sessionStart),
             csvCell(m_sessionCmd),
             csvCell(m_sessionSend),
             csvCell(summaryText));
    emit writeLinesRequested(m_summaryPath, {line});
    // 关闭本次会话的全量/异常文件，及时释放句柄（#5：避免每次测试累积打开句柄）。
    emit closeFileRequested(m_fullPath);
    emit closeFileRequested(m_excPath);
    emit message(QStringLiteral("日志会话结束，已写入 %1").arg(m_logDir));
    m_sessionActive = false;
}
