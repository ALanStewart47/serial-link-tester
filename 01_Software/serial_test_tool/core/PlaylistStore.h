#pragma once

#include "core/CommandItem.h"

#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class CommandLibrary;
class QSettings;

// 自动测试轮询清单的内存数据源与持久化模型。
// 以内存 QList<CommandItem> 为唯一真相源，避免在 UI 控件中反复序列化/反序列化 JSON。
class PlaylistStore
{
public:
    struct RestoreOutcome
    {
        bool restoredFromSnapshot = false;
        QString notice;
    };

    const QList<CommandItem> &items() const { return m_items; }

    QStringList ids() const;

    int size() const { return m_items.size(); }

    bool isEmpty() const { return m_items.isEmpty(); }

    CommandItem at(int row) const;
    bool hasGeneratedSteps() const;

    bool isPreservingFailedSnapshot() const { return m_preserveFailedSnapshot; }

    // 返回包含 row 的单个步骤或成对（设置+读回）步骤的闭区间 [first, second]。
    QPair<int, int> pairBlock(int row) const;

    bool append(const QList<CommandItem> &incoming, const QJsonObject &planSettings,
                QString *error = nullptr);
    bool replaceAll(const QList<CommandItem> &incoming, const QJsonObject &planSettings,
                    QString *error = nullptr);
    bool moveBlock(int row, int delta, int *newRow = nullptr);
    bool removeBlock(int row);
    // 按拖拽后各元素对应的旧索引顺序重排；顺序未变或索引非法时返回 false。
    bool reorderByIndices(const QVector<int> &oldIndices);
    void clear();

    RestoreOutcome
    restoreFromSettings(const CommandLibrary &library, QSettings &settings,
                        const std::function<bool(const QJsonObject &, QString *)> &applyPlanSettings);
    bool saveToSettings(QSettings &settings, const QJsonObject &planSettings,
                        QString *notice = nullptr) const;

private:
    bool checkLimits(const QList<CommandItem> &candidate, const QJsonObject &planSettings,
                     QString *error) const;

    QList<CommandItem> m_items;
    bool m_preserveFailedSnapshot = false;
};
