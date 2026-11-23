#include "core/PlaylistStore.h"

#include "core/AppConfig.h"
#include "core/CommandLibrary.h"
#include "core/TestPlan.h"

#include <QSettings>

QStringList PlaylistStore::ids() const
{
    QStringList result;
    result.reserve(m_items.size());
    for (const CommandItem &item : m_items) {
        result.append(item.commandId);
    }
    return result;
}

CommandItem PlaylistStore::at(int row) const
{
    if (row < 0 || row >= m_items.size()) {
        return {};
    }
    return m_items.at(row);
}

bool PlaylistStore::hasGeneratedSteps() const
{
    for (const CommandItem &item : m_items) {
        if (!item.testContext.isEmpty()) {
            return true;
        }
    }
    return false;
}

QPair<int, int> PlaylistStore::pairBlock(int row) const
{
    if (row < 0 || row >= m_items.size()) {
        return {row, row};
    }
    const QJsonObject &ctx = m_items.at(row).testContext;
    if (ctx.isEmpty() || !ctx.value(QStringLiteral("readback")).toBool()) {
        return {row, row};
    }
    const int first = ctx.value(QStringLiteral("phase")).toString() == QStringLiteral("read") ? row - 1 : row;
    return {qMax(0, first), qMin(int(m_items.size()) - 1, first + 1)};
}

bool PlaylistStore::checkLimits(const QList<CommandItem> &candidate, const QJsonObject &planSettings,
                                QString *error) const
{
    TestPlan::Document doc;
    doc.steps = candidate;
    doc.settings = planSettings;
    if (doc.steps.size() > TestPlan::MaxSteps || TestPlan::serialize(doc).size() > TestPlan::MaxFileBytes) {
        if (error) {
            *error = QStringLiteral("最多4096步且计划快照不超过4MB，请减少步骤");
        }
        return false;
    }
    return true;
}

bool PlaylistStore::append(const QList<CommandItem> &incoming, const QJsonObject &planSettings,
                           QString *error)
{
    QList<CommandItem> candidate = m_items;
    candidate.append(incoming);
    if (!checkLimits(candidate, planSettings, error)) {
        return false;
    }
    m_preserveFailedSnapshot = false;
    m_items = std::move(candidate);
    return true;
}

bool PlaylistStore::replaceAll(const QList<CommandItem> &incoming, const QJsonObject &planSettings,
                               QString *error)
{
    if (!checkLimits(incoming, planSettings, error)) {
        return false;
    }
    m_preserveFailedSnapshot = false;
    m_items = incoming;
    return true;
}

bool PlaylistStore::moveBlock(int row, int delta, int *newRow)
{
    if (row < 0 || row >= m_items.size() || delta == 0) {
        return false;
    }
    const QPair<int, int> block = pairBlock(row);
    const int neighbor = delta < 0 ? block.first - 1 : block.second + 1;
    if (neighbor < 0 || neighbor >= m_items.size()) {
        return false;
    }
    const QPair<int, int> other = pairBlock(neighbor);
    const int blockLen = block.second - block.first + 1;
    const int dest = delta < 0 ? other.first : other.second + 1 - blockLen;

    QList<CommandItem> moving;
    moving.reserve(blockLen);
    for (int i = 0; i < blockLen; ++i) {
        moving.append(m_items.takeAt(block.first));
    }
    for (int i = 0; i < moving.size(); ++i) {
        m_items.insert(dest + i, moving.at(i));
    }
    if (newRow) {
        *newRow = dest;
    }
    return true;
}

bool PlaylistStore::removeBlock(int row)
{
    if (row < 0 || row >= m_items.size()) {
        return false;
    }
    const QPair<int, int> block = pairBlock(row);
    for (int i = block.second; i >= block.first; --i) {
        m_items.removeAt(i);
    }
    return true;
}

bool PlaylistStore::reorderByIndices(const QVector<int> &oldIndices)
{
    if (oldIndices.size() != m_items.size()) {
        return false;
    }
    QVector<bool> seen(m_items.size(), false);
    bool changed = false;
    QList<CommandItem> reordered;
    reordered.reserve(m_items.size());
    for (int i = 0; i < oldIndices.size(); ++i) {
        const int idx = oldIndices.at(i);
        if (idx < 0 || idx >= m_items.size() || seen[idx]) {
            return false;
        }
        seen[idx] = true;
        if (idx != i) {
            changed = true;
        }
        reordered.append(m_items.at(idx));
    }
    if (!changed) {
        return false;
    }
    m_items = std::move(reordered);
    return true;
}

void PlaylistStore::clear()
{
    m_preserveFailedSnapshot = false;
    m_items.clear();
}

PlaylistStore::RestoreOutcome PlaylistStore::restoreFromSettings(
    const CommandLibrary &library, QSettings &settings,
    const std::function<bool(const QJsonObject &, QString *)> &applyPlanSettings)
{
    RestoreOutcome outcome;
    m_items.clear();
    m_preserveFailedSnapshot = false;

    const QStringList savedPlaylist = settings.value(AppConfig::Key::AutoPlaylistIds).toStringList();
    const bool hasSnapshotKey = settings.contains(AppConfig::Key::AutoPlaylistSnapshot);

    // 首次升级保留原始ID清单（包括暂缺/禁用项），之后以快照恢复。
    if (!hasSnapshotKey && !savedPlaylist.isEmpty()
        && !settings.contains(AppConfig::Key::AutoLegacyPlaylistBackup)) {
        settings.setValue(AppConfig::Key::AutoLegacyPlaylistBackup, savedPlaylist);
    }
    for (const QString &id : savedPlaylist) {
        const CommandItem *cmd = library.findById(id);
        if (cmd && cmd->enabled) {
            m_items.append(*cmd);
        }
    }
    if (!hasSnapshotKey && m_items.size() < savedPlaylist.size()) {
        outcome.notice = QStringLiteral("部分旧轮询指令已缺失或禁用；原ID清单已备份，请检查发送顺序");
    }

    const QByteArray snapshot = settings.value(AppConfig::Key::AutoPlaylistSnapshot).toByteArray();
    if (!snapshot.isEmpty()) {
        TestPlan::Document plan;
        QString error;
        if (TestPlan::deserialize(snapshot, &plan, &error) && applyPlanSettings(plan.settings, &error)) {
            m_items = plan.steps;
            m_preserveFailedSnapshot = false;
            outcome.restoredFromSnapshot = true;
            outcome.notice.clear();
        } else {
            m_preserveFailedSnapshot = true;
            settings.setValue(AppConfig::Key::AutoRecoverySnapshot, snapshot);
            outcome.notice = QStringLiteral("上次计划未恢复，已保留原快照：%1").arg(error);
        }
    }
    return outcome;
}

bool PlaylistStore::saveToSettings(QSettings &settings, const QJsonObject &planSettings,
                                   QString *notice) const
{
    settings.setValue(AppConfig::Key::AutoPlaylistIds, ids());
    if (m_preserveFailedSnapshot) {
        return true;
    }
    if (m_items.isEmpty()) {
        settings.remove(AppConfig::Key::AutoPlaylistSnapshot);
        return true;
    }
    TestPlan::Document plan;
    plan.steps = m_items;
    plan.settings = planSettings;
    const QByteArray snapshot = TestPlan::serialize(plan);
    if (snapshot.size() <= TestPlan::MaxFileBytes) {
        settings.setValue(AppConfig::Key::AutoPlaylistSnapshot, snapshot);
        return true;
    }
    if (notice) {
        *notice = QStringLiteral("计划超过4MB，未覆盖上次自动保存；请减少步骤");
    }
    return false;
}
