#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>

namespace Commands {
constexpr int MaxCommands = 4096;
constexpr int MaxFrameBytes = 1024;
constexpr int MaxLibraryBytes = 4 * 1024 * 1024;

struct Command {
    QJsonObject source; // Independent import snapshot, not a reference to the original library.
    QString id, name, protocol;
    QByteArray request, expected;
    bool enabled = true;
    QString parseError;
    QJsonObject snapshot() const;
};
struct Profile {
    QString model;
    QString protocol; // Explicit selection: new, old, old_ext. Never automatically cross protocols.
    int channels = 0;
    QSet<QString> groups;
    bool compatibilityConfirmed = false;
};
enum class ReplyKind { None, BinaryCa, BinaryCc, BinaryTable, Fixed, AsciiDigits, AsciiChoice, Extended };
struct Assessment {
    bool allowed = false;
    QString reason, group;
    ReplyKind kind = ReplyKind::None;
    QByteArray prefix, choices;
    int digits = 0, priority = 100;
};
struct ReplyResult {
    bool received = false, protocolValid = false, deviceError = false;
    bool expectationChecked = false, expectationMatched = false;
    QString outcome, detail;
    QString association = QStringLiteral("未确认；协议无事务序号");
    QJsonObject toJson() const;
};

// Strict decoder. Rejects malformed HEX, lossy Unicode conversion and unknown escapes.
bool decode(const QString &text, const QString &format, QByteArray *bytes, QString *error);
quint8 bcc(const QByteArray &bytes);
Assessment assess(const Command &command, const Profile &profile);
// Call only on the complete response window, not on individual readyRead chunks.
ReplyResult verifyReply(const Command &command, const Profile &profile, const QByteArray &response);

class Library {
public:
    bool importJson(const QByteArray &json, QString *error);
    bool loadFile(const QString &path, QString *error);
    bool loadDefaults(QString *error);
    bool saveCopy(const QString &path, QString *error) const;
    QByteArray toJson() const;
    const QList<Command> &commands() const { return m_commands; }
    QList<Command> eligible(const Profile &profile) const;
    static QString defaultPath();
private:
    QList<Command> m_commands;
};
} // namespace Commands
