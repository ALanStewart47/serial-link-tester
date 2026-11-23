#include "core/LogManager.h"

#include "core/AppConfig.h"
#include "core/CsvUtil.h"
#include "core/PacketBuilder.h"
#include "core/TestPlan.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace {
constexpr qsizetype kDetailLimit = 8 * 1024 * 1024;
constexpr qsizetype kDispatchThreshold = 256 * 1024;
constexpr qsizetype kEvidenceLimit = 4096;
// 汇总可同时包含清单和判定中的指令名，单独保留两个计划的空间。
constexpr qsizetype kSummaryLimit = 2 * TestPlan::MaxFileBytes + 128 * 1024;

QByteArray encodeLine(const QString &line)
{
    return line.toUtf8() + "\r\n";
}

QString timeText()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"));
}

QString evidenceStatus(qsizetype stored, quint64 received)
{
    return received > quint64(stored) ? QStringLiteral("已截断，仅保留前%1字节").arg(stored)
                                      : QStringLiteral("完整");
}

QString fileError(const QString &action, const QFileDevice &file)
{
    return QStringLiteral("%1失败：%2").arg(action, file.errorString());
}
} // namespace

LogWriter::~LogWriter()
{
    closeAll();
}

QString LogWriter::append(const LogFileWrite &write)
{
    QFile *file = m_files.value(write.path, nullptr);
    if (!file) {
        QDir().mkpath(QFileInfo(write.path).absolutePath());
        file = new QFile(write.path);
        if (!file->open(QIODevice::Append)) {
            const QString error = fileError(QStringLiteral("打开"), *file);
            delete file;
            return error;
        }
        m_files.insert(write.path, file);
    }

    const bool needsHeader = file->size() == 0 && !write.header.isEmpty();
    if ((needsHeader && file->write(write.header) != write.header.size())
        || (!write.data.isEmpty() && file->write(write.data) != write.data.size()) || !file->flush()) {
        const QString error = fileError(QStringLiteral("写入"), *file);
        delete m_files.take(write.path);
        return error;
    }
    return {};
}

QString LogWriter::saveSnapshot(const LogFileWrite &write)
{
    QDir().mkpath(QFileInfo(write.path).absolutePath());
    QSaveFile file(write.path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(write.data) != write.data.size() || !file.flush()
        || !file.commit()) {
        return fileError(QStringLiteral("保存计划快照"), file);
    }
    return {};
}

void LogWriter::closeAll()
{
    qDeleteAll(m_files);
    m_files.clear();
}

void LogWriter::writeBatch(QList<LogFileWrite> files, bool finish)
{
    QHash<QString, QString> errors;
    for (const auto &file : files) {
        const QString error = file.replace ? saveSnapshot(file) : append(file);
        if (!error.isEmpty()) {
            errors.insert(file.path, error);
        }
    }
    if (finish) {
        closeAll();
    }
    files.clear();
    emit batchFinished(errors);
}

LogManager::LogManager(QObject *parent) : QObject(parent)
{
    m_writer = new LogWriter;
    m_writer->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_writer, &QObject::deleteLater);
    connect(m_writer, &LogWriter::batchFinished, this, &LogManager::onBatchFinished, Qt::QueuedConnection);
    m_thread.start();

    m_flushTimer = new QTimer(this);
    m_flushTimer->setInterval(300);
    connect(m_flushTimer, &QTimer::timeout, this, &LogManager::flush);
}

LogManager::~LogManager()
{
    m_destructing = true;
    if (m_sessionActive) {
        endSession(QStringLiteral("测试中断，未正常结束"));
    }
    // 主窗口会先异步等待空闲。直接销毁的调用者仍可完成余下写入。
    while (isBusy()) {
        QMetaObject::invokeMethod(m_writer, [] {}, Qt::BlockingQueuedConnection);
        QCoreApplication::sendPostedEvents(this, QEvent::MetaCall);
    }
    m_thread.quit();
    m_thread.wait();
}

void LogManager::setLogDir(const QString &dir)
{
    m_logDir = dir;
}

void LogManager::beginSession(const QString &commandName, const QString &sendData, const QString &planJson)
{
    const QSettings settings(AppConfig::Org(), AppConfig::App());
    beginSession(commandName, sendData, planJson, settings.value(AppConfig::Key::FullLog, false).toBool());
}

void LogManager::beginSession(const QString &commandName, const QString &sendData, const QString &planJson,
                              bool fullLog)
{
    if (m_sessionActive) {
        endSession(QStringLiteral("测试中断，未正常结束"));
    }
    if (isBusy()) {
        m_showTrackedSession = false;
        publishStatus(QStringLiteral("日志不完整：上一轮仍在保存，本次测试继续运行但不保存日志。"));
        return;
    }
    m_showTrackedSession = true;
    if (m_logDir.isEmpty()) {
        publishStatus(QStringLiteral("日志不完整：未设置日志目录，本次测试继续运行但不保存日志。"));
        return;
    }

    const QDateTime now = QDateTime::currentDateTime();
    const QString suffix = now.toString(QStringLiteral("yyyyMMdd_HHmmss_zzz")) + QLatin1Char('_')
                           + QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_sessionDir = m_logDir;
    m_fullPath = QStringLiteral("%1/full_%2.csv").arg(m_sessionDir, suffix);
    m_excPath = QStringLiteral("%1/exception_%2.csv").arg(m_sessionDir, suffix);
    m_summaryPath = QStringLiteral("%1/summary.csv").arg(m_sessionDir);
    m_planPath = QStringLiteral("%1/plan_%2.json").arg(m_sessionDir, suffix);
    m_fullEnabled = fullLog;
    m_sessionActive = true;
    m_createFiles = true;
    m_flushRequested = false;
    m_failures.clear();
    m_lastCompletionError.clear();
    m_summaryText.clear();

    const auto fieldPrefix = [&](const QString &command, const QString &data) {
        return QStringLiteral("%1,%2,%3,")
            .arg(CsvUtil::escape(now.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))),
                 CsvUtil::escape(command), CsvUtil::escape(data))
            .toUtf8();
    };
    m_summaryPrefix = fieldPrefix(commandName, sendData);
    m_planBytes = planJson.toUtf8();
    publishStatus(QStringLiteral("日志记录中：%1").arg(m_sessionDir));
    if (m_summaryPrefix.size() > TestPlan::MaxFileBytes) {
        m_summaryPrefix = fieldPrefix(QStringLiteral("轮询清单详见计划快照"), {});
        recordFailure(m_summaryPath, QStringLiteral("清单摘要超过保存上限，已省略"));
    }
    if (m_planBytes.size() > TestPlan::MaxFileBytes) {
        m_planBytes.clear();
        recordFailure(m_planPath, QStringLiteral("计划快照超过 4 MiB"));
    }
    m_flushTimer->start();
    dispatch();
}

void LogManager::logRound(quint64 index, AutoSendEngine::Outcome outcome, qint64 respUs,
                          const QByteArray &actual, const QString &commandName, quint64 receivedBytes)
{
    if (!m_sessionActive || !m_fullEnabled || m_failures.contains(m_fullPath)) {
        return;
    }
    QString outText;
    switch (outcome) {
    case AutoSendEngine::Outcome::Match:
        outText = QStringLiteral("匹配成功");
        break;
    case AutoSendEngine::Outcome::Mismatch:
        outText = QStringLiteral("回复错误");
        break;
    case AutoSendEngine::Outcome::Timeout:
        outText = QStringLiteral("超时");
        break;
    case AutoSendEngine::Outcome::SentOnly:
        outText = QStringLiteral("仅发送");
        break;
    case AutoSendEngine::Outcome::Interrupted:
        outText = QStringLiteral("中断/未完成");
        break;
    }
    const QByteArray preview = actual.left(kEvidenceLimit);
    receivedBytes = qMax(receivedBytes, quint64(actual.size()));
    acceptDetail(m_fullPath, m_fullBuf,
                 QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9")
                     .arg(QString::number(index), timeText(), outText,
                          respUs < 0 ? QStringLiteral("-") : QString::number(respUs / 1000.0, 'f', 3),
                          CsvUtil::escape(PacketBuilder::toHexText(preview)),
                          CsvUtil::escape(PacketBuilder::toAsciiText(preview)), CsvUtil::escape(commandName),
                          QString::number(receivedBytes), evidenceStatus(preview.size(), receivedBytes)));
}

void LogManager::logException(quint64 index, bool timeout, const QByteArray &actual,
                              const QString &commandName, quint64 receivedBytes)
{
    if (!m_sessionActive || m_failures.contains(m_excPath)) {
        return;
    }
    const QByteArray preview = actual.left(kEvidenceLimit);
    receivedBytes = qMax(receivedBytes, quint64(actual.size()));
    acceptDetail(m_excPath, m_excBuf,
                 QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8")
                     .arg(QString::number(index), timeText(),
                          timeout ? QStringLiteral("超时(丢包)") : QStringLiteral("回复错误"),
                          CsvUtil::escape(PacketBuilder::toHexText(preview)),
                          CsvUtil::escape(PacketBuilder::toAsciiText(preview)), CsvUtil::escape(commandName),
                          QString::number(receivedBytes), evidenceStatus(preview.size(), receivedBytes)));
}

void LogManager::acceptDetail(const QString &path, QByteArray &buffer, const QString &line)
{
    const QByteArray row = encodeLine(line);
    const qsizetype pending = m_fullBuf.size() + m_excBuf.size();
    if (row.size() > kDetailLimit - pending - m_inFlightBytes) {
        recordFailure(path, QStringLiteral("待写和在途明细达到 8 MiB 上限，后续明细未保存"));
        return;
    }
    buffer.append(row);
    if (pending + row.size() >= kDispatchThreshold) {
        flush();
    }
}

void LogManager::flush()
{
    m_flushRequested = true;
    dispatch();
}

void LogManager::submit(QList<LogFileWrite> writes, Batch batch)
{
    m_batch = batch;
    LogWriter *writer = m_writer;
    QMetaObject::invokeMethod(
        writer,
        [writer, writes = std::move(writes), batch]() mutable {
            writer->writeBatch(std::move(writes), batch == Batch::Final);
        },
        Qt::QueuedConnection);
}

void LogManager::dispatch()
{
    if (m_batch != Batch::None || !isBusy()) {
        return;
    }
    QList<LogFileWrite> writes;
    if (m_createFiles || m_flushRequested || m_finishing) {
        m_inFlightBytes = m_fullBuf.size() + m_excBuf.size();
        if (!m_planBytes.isEmpty()) {
            writes.append({m_planPath, {}, std::exchange(m_planBytes, {}), true});
        }
        if (m_createFiles || !m_excBuf.isEmpty()) {
            writes.append(
                {m_excPath,
                 encodeLine(QStringLiteral("序号,时间,类型,实际HEX,实际ASCII,指令,接收字节数,记录状态")),
                 std::exchange(m_excBuf, {})});
        }
        if ((m_createFiles && m_fullEnabled) || !m_fullBuf.isEmpty()) {
            writes.append({m_fullPath,
                           encodeLine(QStringLiteral(
                               "序号,时间,结果,响应ms,实际HEX,实际ASCII,指令,接收字节数,记录状态")),
                           std::exchange(m_fullBuf, {})});
        }
        m_createFiles = false;
        m_flushRequested = false;
        if (!writes.isEmpty()) {
            submit(std::move(writes), Batch::Details);
            return;
        }
    }
    if (m_finishing) {
        auto summaryRow = [this] {
            QString detail = m_summaryText;
            if (!m_failures.isEmpty()) {
                detail += QStringLiteral("；") + failureText();
            }
            return m_summaryPrefix + encodeLine(CsvUtil::escape(detail));
        };
        QByteArray line = summaryRow();
        if (line.size() > kSummaryLimit) {
            m_summaryText = m_summaryText.left(32768) + QStringLiteral("…");
            recordFailure(m_summaryPath, QStringLiteral("汇总超出保存上限，已截断"));
            line = summaryRow();
        }
        m_summaryPrefix.clear();
        m_summaryText.clear();
        writes.append(
            {m_summaryPath, encodeLine(QStringLiteral("开始时间,指令,发送内容,汇总")), std::move(line)});
        submit(std::move(writes), Batch::Final);
    }
}

void LogManager::endSession(const QString &summaryText)
{
    if (!m_sessionActive) {
        return;
    }
    m_sessionActive = false;
    m_finishing = true;
    m_summaryText = summaryText;
    m_flushTimer->stop();
    if (m_showTrackedSession) {
        publishStatus(m_failures.isEmpty() ? QStringLiteral("测试已结束，正在保存日志")
                                           : failureText() + QStringLiteral("；正在保存剩余日志"));
    }
    dispatch();
}

void LogManager::onBatchFinished(const QHash<QString, QString> &errors)
{
    const Batch finished = std::exchange(m_batch, Batch::None);
    m_inFlightBytes = 0;
    for (auto it = errors.cbegin(); it != errors.cend(); ++it) {
        if (it.key() == m_fullPath) {
            m_fullBuf.clear();
        } else if (it.key() == m_excPath) {
            m_excBuf.clear();
        }
        recordFailure(it.key(), it.value());
    }
    if (finished == Batch::Final) {
        m_finishing = false;
        m_lastCompletionError = m_failures.isEmpty() ? QString() : failureText();
        if (m_showTrackedSession) {
            publishStatus(m_lastCompletionError.isEmpty()
                              ? QStringLiteral("日志会话结束，已写入 %1").arg(m_sessionDir)
                              : m_lastCompletionError);
        }
        if (!m_destructing && !isBusy()) {
            emit idle();
        }
    } else {
        dispatch();
    }
}

void LogManager::recordFailure(const QString &path, const QString &reason)
{
    if (m_failures.value(path) == reason) {
        return;
    }
    m_failures.insert(path, reason);
    if (m_showTrackedSession) {
        publishStatus(failureText());
    }
}

QString LogManager::failureText() const
{
    QStringList issues;
    QStringList paths = m_failures.keys();
    paths.sort();
    for (const QString &path : paths) {
        issues.append(QStringLiteral("%1（%2）").arg(QFileInfo(path).fileName(), m_failures.value(path)));
    }
    return QStringLiteral("日志不完整：%1").arg(issues.join(QStringLiteral("；")));
}

void LogManager::publishStatus(const QString &text)
{
    m_status = text;
    if (!m_destructing) {
        emit statusChanged(text);
        emit message(text);
    }
}
