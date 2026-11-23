#pragma once
#include "core/AutoSendEngine.h"
#include "core/TestVerdict.h"

namespace PlanAssessment {
struct Row
{
    QString title;
    QString writeStatus;
    QString readStatus;
    QString status;
};

bool hasGenerated(const AutoSendEngine &engine);
QVector<Row> rows(const AutoSendEngine &engine);
QString coverage(const AutoSendEngine &engine);
TestVerdict::Result verdict(const AutoSendEngine &engine);
} // namespace PlanAssessment
