#pragma once
#include <QHostAddress>
#include <QJsonObject>

namespace LocalTcp {
// Returns one established local TCP session to the exact peer, if Windows exposes one.
QJsonObject findEstablished(const QHostAddress &target, quint16 port);
}
