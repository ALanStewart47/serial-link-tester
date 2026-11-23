#include "core/TestPlan.h"
#include "core/PacketBuilder.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>

namespace TestPlan {
namespace {
bool fail(QString *error, const QString &text)
{
    if (error)
        *error = text;
    return false;
}

int command(const QString &id)
{
    if (id == QStringLiteral("brightness"))
        return 0x01;
    if (id == QStringLiteral("switch"))
        return 0x02;
    if (id == QStringLiteral("pulse"))
        return 0x21;
    if (id == QStringLiteral("light_delay"))
        return 0x22;
    if (id == QStringLiteral("camera_delay"))
        return 0x23;
    return -1;
}

QByteArray frame(int cmd, int channel, int value)
{
    QByteArray b;
    for (int v : {0xca, cmd, channel, value >> 8, value & 255})
        b.append(char(v));
    return b;
}
} // namespace

QString protocolName(const QString &protocol)
{
    if (protocol == "new")
        return QStringLiteral("新协议 CA");
    if (protocol == "old")
        return QStringLiteral("旧协议 ASCII");
    if (protocol == "old_ext")
        return QStringLiteral("旧扩展协议 $/@");
    return {};
}

int channelLimit(const QString &protocol)
{
    return protocol == "new" ? 255 : (protocol == "old" || protocol == "old_ext") ? 26 : 0;
}

QStringList functions(const QString &protocol)
{
    if (protocol == "old")
        return {QStringLiteral("brightness"), QStringLiteral("pulse"), QStringLiteral("light_delay"),
                QStringLiteral("camera_delay")};
    if (protocol != "new" && protocol != "old_ext")
        return {};
    return {QStringLiteral("brightness"), QStringLiteral("pulse"), QStringLiteral("switch"),
            QStringLiteral("light_delay"), QStringLiteral("camera_delay")};
}

QString functionName(const QString &id)
{
    if (id == QStringLiteral("brightness"))
        return QStringLiteral("亮度");
    if (id == QStringLiteral("pulse"))
        return QStringLiteral("脉宽");
    if (id == QStringLiteral("switch"))
        return QStringLiteral("通道开关");
    if (id == QStringLiteral("light_delay"))
        return QStringLiteral("光源延时");
    if (id == QStringLiteral("camera_delay"))
        return QStringLiteral("相机延时");
    return {};
}

int maximum(const Parameters &p)
{
    return p.function == QStringLiteral("brightness") ? p.brightnessMax
           : p.function == QStringLiteral("switch")   ? 1
                                                      : 999;
}

bool parseChannels(const QString &text, int maximum, QList<int> *channels, QString *error)
{
    if (maximum < 1 || maximum > 255 || text.size() > 4096)
        return fail(error, QStringLiteral("设备通道数须为1～255，范围文本不得过长"));
    QSet<int> unique;
    const auto parts = text.split(QLatin1Char(','), Qt::KeepEmptyParts);
    const QRegularExpression re(QStringLiteral("^\\s*([0-9]+)\\s*(?:-\\s*([0-9]+)\\s*)?$"));
    for (const QString &part : parts) {
        const auto m = re.match(part);
        bool ok1 = false, ok2 = false;
        const int first = m.captured(1).toInt(&ok1);
        const int last = m.captured(2).isEmpty() ? first : m.captured(2).toInt(&ok2);
        if (!m.hasMatch() || !ok1 || (!m.captured(2).isEmpty() && !ok2) || first < 1 || last < first
            || last > maximum)
            return fail(error, QStringLiteral("通道范围无效：%1。请填写1～%2内的单个通道、1-8或1,3,5-8")
                                   .arg(part)
                                   .arg(maximum));
        for (int ch = first; ch <= last; ++ch)
            unique.insert(ch);
    }
    *channels = unique.values();
    std::sort(channels->begin(), channels->end());
    return !channels->isEmpty();
}

bool generate(const Parameters &p, QList<CommandItem> *steps, QString *error)
{
    if (!functions(p.protocol).contains(p.function))
        return fail(error, QStringLiteral("所选协议不支持此逐通道功能"));
    if (p.channelCount < 1 || p.channelCount > channelLimit(p.protocol))
        return fail(error, QStringLiteral("%1的设备通道数须为1～%2")
                               .arg(protocolName(p.protocol))
                               .arg(channelLimit(p.protocol)));
    const int cmd = command(p.function);
    if (cmd < 0 || (p.brightnessMax != 255 && p.brightnessMax != 999) || p.model.trimmed().isEmpty()
        || p.model.size() > 100 || p.pulseUnit.size() > 100)
        return fail(error, QStringLiteral("设备名称、功能或参数档位无效"));
    if (p.value < 0 || p.value > maximum(p))
        return fail(error, QStringLiteral("%1取值须为0～%2").arg(functionName(p.function)).arg(maximum(p)));
    QList<int> channels;
    if (!parseChannels(p.channels, p.channelCount, &channels, error))
        return false;
    if (p.protocol == "old" && p.function == "brightness" && (channels.contains(20) || channels.contains(23)))
        return fail(
            error,
            QStringLiteral(
                "旧协议T/W通道亮度与ST内触发周期/"
                "SW分区开关命令重名；请改用旧扩展协议，或排除20、23通道。目标固件专用规则确认后再开放。"));
    QList<CommandItem> generated;
    const QString groupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (int ch : channels) {
        const QString caseId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        for (int phase = 0; phase < (p.readback ? 2 : 1); ++phase) {
            CommandItem item;
            item.commandId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            item.protocolType = p.protocol;
            item.functionGroup = cmd < 0x20 ? QStringLiteral("digital") : QStringLiteral("strobe");
            const QString unit =
                p.function == QStringLiteral("brightness") || p.function == QStringLiteral("switch")
                    ? QString()
                    : QStringLiteral("（%1）").arg(p.pulseUnit);
            item.commandName =
                QStringLiteral("通道%1 · %2=%3%4 · %5")
                    .arg(ch)
                    .arg(functionName(p.function))
                    .arg(p.value)
                    .arg(unit, phase ? QStringLiteral("读回核对") : QStringLiteral("设置确认"));
            if (p.protocol == "new") {
                item.sendData =
                    PacketBuilder::toHexText(frame(cmd + (phase ? 0x60 : 0), ch, phase ? 0 : p.value));
                item.enableBcc = true;
                QByteArray expected;
                if (phase)
                    expected = PacketBuilder::appendBcc(frame(cmd + 0x60, ch, p.value));
                else
                    for (int b : {0xca, cmd, ch, 0})
                        expected.append(char(b));
                item.expectedReply = PacketBuilder::toHexText(expected);
            } else {
                const QString letter(QChar('A' + ch - 1));
                const QString digits = QStringLiteral("%1").arg(p.value, 4, 10, QLatin1Char('0'));
                item.commandName.prepend(QStringLiteral("%1 · %2 · ").arg(protocolName(p.protocol), letter));
                item.sendFormat = item.expectedReplyFormat = QStringLiteral("ascii");
                item.enableBcc = false;
                if (p.protocol == "old") {
                    const QString prefix = p.function == "brightness"    ? "S"
                                           : p.function == "pulse"       ? "SP"
                                           : p.function == "light_delay" ? "DL"
                                                                         : "DC";
                    const QString ack = (p.function == "brightness"    ? QString()
                                         : p.function == "pulse"       ? "p"
                                         : p.function == "light_delay" ? "dl"
                                                                       : "dc")
                                        + letter.toLower();
                    item.sendData = prefix + letter + (phase ? QString() : digits) + '#';
                    item.expectedReply = ack + (phase ? digits : QString());
                } else {
                    const QString code = p.function == "brightness"    ? "01"
                                         : p.function == "pulse"       ? "02"
                                         : p.function == "switch"      ? "05"
                                         : p.function == "light_delay" ? "06"
                                                                       : "07";
                    item.sendData = (phase ? "@" : "$") + code + letter + (phase ? QString() : digits) + '#';
                    item.expectedReply = phase ? "@" + code + letter + digits + '#' : "$";
                }
            }
            item.matchRule = QStringLiteral("strict_frame");
            item.description =
                QStringLiteral("%4参数模板v1；设备：%1；通道数%2（人工配置）；逐通道%3")
                    .arg(p.model)
                    .arg(p.channelCount)
                    .arg(p.readback ? QStringLiteral("设置后读回") : QStringLiteral("设置确认"))
                    .arg(protocolName(p.protocol));
            item.testContext = {{"generator", p.protocol == "new"   ? "ca_v1"
                                              : p.protocol == "old" ? "old_ascii_v1"
                                                                    : "old_ext_v1"},
                                {"template_version", 1},
                                {"group_id", groupId},
                                {"case_id", caseId},
                                {"function", p.function},
                                {"channel", ch},
                                {"value", p.value},
                                {"phase", phase ? "read" : "write"},
                                {"readback", p.readback},
                                {"model", p.model},
                                {"channel_count", p.channelCount},
                                {"brightness_max", p.brightnessMax},
                                {"unit", p.pulseUnit},
                                {"capability_source", "manual"}};
            generated.append(item);
        }
    }
    *steps = generated;
    return true;
}

bool totalSends(int stepCount, quint64 repetitions, quint64 *total, QString *error)
{
    if (stepCount < 1 || stepCount > MaxSteps || repetitions < 1
        || repetitions > MaxSends / quint64(stepCount))
        return fail(error, QStringLiteral("计划不能为空或超过4096步；总发送条数不得超过10,000,000"));
    *total = quint64(stepCount) * repetitions;
    return true;
}

bool validate(const QList<CommandItem> &steps, QString *error)
{
    if (steps.isEmpty() || steps.size() > MaxSteps)
        return fail(error, QStringLiteral("测试计划须包含1～4096步"));
    QSet<QString> generatedIds, cases;
    for (int i = 0; i < steps.size(); ++i) {
        const auto &s = steps[i];
        if (s.commandName.isEmpty() || s.commandName.size() > 512 || !s.enabled
            || s.sendData.size() > CommandItem::MaxFrameTextLength
            || s.expectedReply.size() > CommandItem::MaxFrameTextLength)
            return fail(error, QStringLiteral("第%1步的名称、内容长度或启用状态无效").arg(i + 1));
        bool ok = true;
        const QByteArray payload = PacketBuilder::decodeCommandText(s.sendFormat, s.sendData, &ok);
        if (!ok || payload.isEmpty())
            return fail(error, QStringLiteral("第%1步发送内容无效").arg(i + 1));
        const QByteArray expected =
            PacketBuilder::decodeCommandText(s.expectedReplyFormat, s.expectedReply, &ok);
        if (!ok || (s.matchRule != "exact" && s.matchRule != "strict_frame")
            || (s.matchRule == "strict_frame" && expected.isEmpty()))
            return fail(error, QStringLiteral("第%1步期望或匹配策略无效，完整帧检测不可留空").arg(i + 1));
        if (s.testContext.isEmpty())
            continue;
        const auto &c = s.testContext;
        const QString generator = c["generator"].toString();
        if ((generator != "ca_v1" && generator != "old_ascii_v1" && generator != "old_ext_v1")
            || c["template_version"].toInt() != 1 || s.commandId.isEmpty()
            || generatedIds.contains(s.commandId) || c["case_id"].toString().isEmpty()
            || c["group_id"].toString().isEmpty()
            || (c["phase"].toString() != "write" && c["phase"].toString() != "read"))
            return fail(error, QStringLiteral("生成步骤标识或模板版本无效"));
        generatedIds.insert(s.commandId);
        Parameters p;
        p.protocol = generator == "ca_v1" ? "new" : generator == "old_ascii_v1" ? "old" : "old_ext";
        p.model = c["model"].toString();
        p.channelCount = c["channel_count"].toInt();
        p.brightnessMax = c["brightness_max"].toInt();
        p.pulseUnit = c["unit"].toString();
        p.function = c["function"].toString();
        p.value = c["value"].toInt(-1);
        p.channels = QString::number(c["channel"].toInt());
        p.readback = c["readback"].toBool();
        QList<CommandItem> check;
        if (!generate(p, &check, error))
            return false;
        const bool read = c["phase"].toString() == "read";
        if (read && !p.readback)
            return fail(error, QStringLiteral("读回步骤缺少配对声明"));
        const auto &canonical = check.at(read ? 1 : 0);
        if (s.protocolType != canonical.protocolType || s.sendFormat != canonical.sendFormat
            || s.expectedReplyFormat != canonical.expectedReplyFormat || s.enableBcc != canonical.enableBcc
            || s.matchRule != "strict_frame" || s.sendData != canonical.sendData
            || s.expectedReply != canonical.expectedReply)
            return fail(error, QStringLiteral("第%1步报文与参数快照不一致，请重新生成").arg(i + 1));
        const QString cid = c["case_id"].toString();
        if (!read) {
            if (cases.contains(cid))
                return fail(error, QStringLiteral("计划中存在重复用例标识"));
            cases.insert(cid);
        }
        if (read || p.readback) {
            const int otherIndex = i + (read ? -1 : 1);
            if (otherIndex < 0 || otherIndex >= steps.size())
                return fail(error, QStringLiteral("设置与读回必须成对相邻，请重新生成此通道"));
            auto other = steps[otherIndex].testContext;
            auto thisContext = c;
            if (other["phase"].toString() != (read ? "write" : "read"))
                return fail(error, QStringLiteral("设置与读回顺序无效"));
            other.remove("phase");
            thisContext.remove("phase");
            if (other != thisContext)
                return fail(error, QStringLiteral("设置与读回参数不一致"));
        }
    }
    return true;
}

QByteArray serialize(const Document &plan)
{
    QJsonArray steps;
    for (const auto &s : plan.steps)
        steps.append(s.toJson());
    return QJsonDocument(QJsonObject{{"format", "serial-test-plan"},
                                     {"schema_version", 1},
                                     {"name", plan.name},
                                     {"settings", plan.settings},
                                     {"steps", steps}})
        .toJson(QJsonDocument::Compact);
}

bool deserialize(const QByteArray &json, Document *plan, QString *error)
{
    if (json.size() > MaxFileBytes)
        return fail(error, QStringLiteral("计划文件超过4MB"));
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(json, &parseError);
    const auto obj = doc.object();
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()
        || obj["format"].toString() != "serial-test-plan" || obj["schema_version"].toInt() != 1
        || !obj["steps"].isArray() || !obj["settings"].isObject())
        return fail(error, QStringLiteral("不是受支持的测试计划JSON（需要版本1）"));
    Document candidate;
    candidate.name = obj["name"].toString();
    candidate.settings = obj["settings"].toObject();
    const auto arr = obj["steps"].toArray();
    if (arr.size() > MaxSteps)
        return fail(error, QStringLiteral("计划最多4096步"));
    for (const auto &v : arr) {
        if (!v.isObject())
            return fail(error, QStringLiteral("计划步骤须为对象"));
        candidate.steps.append(CommandItem::fromJson(v.toObject()));
    }
    if (!validate(candidate.steps, error))
        return false;
    *plan = candidate;
    return true;
}

bool save(const QString &path, const Document &plan, QString *error)
{
    if (!validate(plan.steps, error))
        return false;
    const QByteArray data = serialize(plan);
    if (data.size() > MaxFileBytes)
        return fail(error, QStringLiteral("计划文件超过4MB"));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        return fail(error, f.errorString());
    return true;
}

bool load(const QString &path, Document *plan, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(error, f.errorString());
    if (f.size() > MaxFileBytes)
        return fail(error, QStringLiteral("计划文件超过4MB"));
    return deserialize(f.readAll(), plan, error);
}
} // namespace TestPlan
