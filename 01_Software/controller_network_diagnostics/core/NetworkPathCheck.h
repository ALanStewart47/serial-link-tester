#pragma once
#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QString>

namespace NetDiag {
enum class Status { Passed, Warning, Failed, Unknown, Skipped };
QString statusText(Status status);
QString statusKey(Status status);

struct Finding {
    QString code, title;
    Status status = Status::Unknown;
    QString evidence, suggestion;
};
struct Address {
    QHostAddress ip;
    int prefixLength = -1;
};
struct Interface {
    QString name, displayName;
    uint index = 0;
    bool up = false, running = false, loopback = false;
    QList<Address> addresses;
};
struct Route {
    enum class State { Available, NoRoute, Unavailable };
    State state = State::Unavailable;
    uint interfaceIndex = 0;
    QHostAddress source, nextHop, destinationPrefix;
    int prefixLength = -1;
    quint32 errorCode = 0;
    QString detail;
};
struct Snapshot {
    QDateTime capturedAt;
    QList<Interface> interfaces;
    Route route;
};
struct Request {
    QString targetText;
    QString expectedInterfaceName; // Empty: Windows chooses the interface. Never persisted by index.
};
struct Report {
    Request request;
    Snapshot snapshot;
    QList<Finding> findings;
    QString summary() const;
    QJsonObject toJson() const;
};

// Local information only: no ping, TCP connection, UDP discovery or configuration writes.
Snapshot collectSnapshot(const QHostAddress &target);
Report evaluate(const Request &request, const Snapshot &snapshot);
Report inspect(const Request &request);
} // namespace NetDiag
