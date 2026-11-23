#include "core/DiagnosticCommands.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <algorithm>

static void initializeCommandResources() { Q_INIT_RESOURCE(diagnostic_commands); }

namespace Commands {
namespace {
bool fail(QString *error, const QString &message) { if(error) *error=message; return false; }
bool oneOf(int value, std::initializer_list<int> set) { return std::find(set.begin(),set.end(),value)!=set.end(); }
int nibble(QChar c) {
    const ushort n=c.unicode();
    if(n>='0' && n<='9') return n-'0';
    if(n>='A' && n<='F') return n-'A'+10;
    if(n>='a' && n<='f') return n-'a'+10;
    return -1;
}
bool asciiChannel(char c, const Profile &p) { return c>='A' && c<='Z' && c-'A'+1<=p.channels; }
bool decimal(const QByteArray &data) {
    return !data.isEmpty() && std::all_of(data.begin(),data.end(),[](char c) { return c>='0' && c<='9'; });
}
Assessment denied(const QString &why) { Assessment a; a.reason=why; return a; }
Assessment recognize(const Command &c, const Profile &p)
{
    const auto &b=c.request; Assessment a;
    if(c.protocol=="new") {
        if(b.isEmpty() || bcc(b)!=0) return denied(QStringLiteral("新协议请求长度或BCC无效。"));
        const int head=quint8(b[0]);
        if(head==0xca && b.size()==6) {
            const int cmd=quint8(b[1]), channel=quint8(b[2]);
            if(!oneOf(cmd,{0x61,0x62,0x63,0x64,0x65,0x81,0x82,0x83,0x84,0x85,0x86,0x87,0x88,
                           0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa}))
                return denied(QStringLiteral("该CA命令不在已审核只读集合。"));
            if(b[3]!=0 || b[4]!=0) return denied(QStringLiteral("查询数据字段必须为0。"));
            const bool perChannel=oneOf(cmd,{0x61,0x62,0x64,0x81,0x82,0x83,0xa3,0xa4,0xa7,0xa8});
            if((perChannel && (channel<1 || channel>p.channels)) || (!perChannel && channel!=0))
                return denied(QStringLiteral("通道不符合设备档案；通道型查询禁止0通道。"));
            a.kind=ReplyKind::BinaryCa; a.prefix=b.left(3);
            a.group=cmd<=0x65?"digital":(cmd<=0x88?"strobe":"common");
            a.priority=cmd==0xa5?0:(cmd==0xa6?1:100);
            return a;
        }
        if(head==0xcc && (b.size()==7 || b.size()==9)) {
            const int cmd=quint8(b[1]);
            // Reference firmware defines one recipe and one trigger source. Reject zero as well.
            if(b[2]!=1 || b[3]!=1) return denied(QStringLiteral("当前审核档案仅支持配方1、触发源1。"));
            if(b.size()==7 && cmd>=0x32 && cmd<=0x3e && b[4]==0 && b[5]==0) {
                a.kind=ReplyKind::BinaryCc; a.prefix=b.left(4); a.group="program"; return a;
            }
            if(b.size()==9 && cmd==0x31) {
                const int row=quint8(b[4])*256+quint8(b[5]), channels=quint8(b[6])*256+quint8(b[7]);
                if(row>=1 && row<=64 && channels>=1 && channels<=p.channels) {
                    a.kind=ReplyKind::BinaryTable; a.prefix=b.left(8); a.group="program"; return a;
                }
            }
            return denied(QStringLiteral("CC查询布局或范围不符合已核对协议；原库表格查询占位帧不可执行。"));
        }
        return denied(QStringLiteral("仅允许已审核CA/CC读取帧；CB、写入及未知帧不自动发送。"));
    }
    if(c.protocol=="old_ext") {
        if(b.size()<5 || b.size()>30 || b[0]!='@' || b.back()!='#' || !decimal(b.mid(1,2)))
            return denied(QStringLiteral("扩展读取需要完整@功能号通道#帧。"));
        const int fn=b.mid(1,2).toInt();
        if(!oneOf(fn,{1,2,3,4,5,6,7,8,9,11,13})) return denied(QStringLiteral("扩展功能号未审核或参考固件未实现。"));
        if(oneOf(fn,{4,8,9}) && b.size()!=5) return denied(QStringLiteral("该全局读取只允许一个通道字母。"));
        QSet<char> used;
        for(int i=3;i<b.size()-1;++i) {
            if(!asciiChannel(b[i],p) || used.contains(b[i])) return denied(QStringLiteral("扩展读取通道越界、重复或含多余内容。"));
            used.insert(b[i]);
        }
        a.kind=ReplyKind::Extended; a.prefix=b.left(3);
        a.group=oneOf(fn,{1,5,8,13})?"digital":"strobe"; return a;
    }
    if(c.protocol=="old") {
        if(b=="CST" || b=="CST#") { a.kind=ReplyKind::Fixed; a.prefix="CST"; a.group="common"; a.priority=0; return a; }
        if(b=="SW#" || b=="DC#") { a.kind=ReplyKind::Fixed; a.prefix=b=="SW#"?"sw0000":"dc0000";
            a.group=b=="SW#"?"digital":"strobe"; return a; }
        if(b=="T#" || b=="TR#" || b=="CT#") {
            a.kind=ReplyKind::AsciiChoice; a.prefix=b=="T#"?"":(b=="TR#"?"tr":"ct");
            a.choices=b=="T#"?"HL":(b=="TR#"?"012":"01"); a.group=b=="T#"?"digital":"strobe"; return a;
        }
        if(b=="ST#") { a.kind=ReplyKind::AsciiDigits; a.prefix="t0"; a.digits=3; a.group="strobe"; return a; }
        const QList<QByteArray> prefixes{"SPU","SP","DL","DC","S"};
        for(const auto &prefix:prefixes) {
            if(b.size()!=prefix.size()+2 || !b.startsWith(prefix) || b.back()!='#') continue;
            const char channel=b[prefix.size()];
            if(!asciiChannel(channel,p)) return denied(QStringLiteral("ASCII通道超出设备档案。"));
            const char lower=char(channel-'A'+'a');
            a.group=prefix=="S"?"digital":"strobe";
            if(prefix=="SPU") { a.kind=ReplyKind::AsciiChoice; a.prefix=QByteArray("pu")+lower; a.choices="01"; }
            else {
                a.kind=ReplyKind::AsciiDigits; a.digits=3;
                a.prefix=(prefix=="S"?QByteArray():prefix.mid(prefix=="SP"?1:0).toLower())+lower+'0';
            }
            return a;
        }
    }
    return denied(QStringLiteral("发送内容不在已审核的只读查询集合；不依据指令名称或导入批准标记放行。"));
}
}

quint8 bcc(const QByteArray &bytes) { quint8 value=0; for(char b:bytes) value^=quint8(b); return value; }
bool decode(const QString &text,const QString &format,QByteArray *bytes,QString *error)
{
    bytes->clear(); if(error) error->clear();
    if(text.size()>MaxFrameBytes*8) return fail(error,QStringLiteral("报文文本过长。"));
    if(format=="hex") {
        int high=-1;
        for(QChar c:text) {
            if(c.isSpace() || c==',') continue;
            const int v=nibble(c); if(v<0) return fail(error,QStringLiteral("HEX包含非法字符。"));
            if(high<0) high=v; else { bytes->append(char(high*16+v)); high=-1; }
        }
        if(high>=0) return fail(error,QStringLiteral("HEX必须为完整字节。"));
    } else if(format=="ascii") {
        for(int i=0;i<text.size();++i) {
            QChar c=text[i];
            if(c.unicode()>127) return fail(error,QStringLiteral("ASCII包含非ASCII字符，拒绝有损转换。"));
            if(c!='\\') { bytes->append(char(c.unicode())); continue; }
            if(++i>=text.size()) return fail(error,QStringLiteral("ASCII转义不完整。"));
            c=text[i];
            if(c=='r') bytes->append('\r'); else if(c=='n') bytes->append('\n');
            else if(c=='t') bytes->append('\t'); else if(c=='0') bytes->append('\0');
            else if(c=='\\') bytes->append('\\');
            else if(c=='x' || c=='X') {
                if(i+2>=text.size() || nibble(text[i+1])<0 || nibble(text[i+2])<0) return fail(error,QStringLiteral("\\x需要两位HEX。"));
                bytes->append(char(nibble(text[i+1])*16+nibble(text[i+2]))); i+=2;
            } else return fail(error,QStringLiteral("未知ASCII转义。"));
        }
    } else return fail(error,QStringLiteral("报文格式必须为hex或ascii。"));
    if(bytes->size()>MaxFrameBytes) return fail(error,QStringLiteral("报文超过1024字节。"));
    return true;
}
QJsonObject Command::snapshot() const
{
    return {{"source",source},{"request_hex",QString::fromLatin1(request.toHex())},
        {"expected_hex",QString::fromLatin1(expected.toHex())},{"parse_error",parseError},
        {"sha256",QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(source).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex())}};
}
Assessment assess(const Command &c,const Profile &p)
{
    if(!c.parseError.isEmpty()) return denied(c.parseError);
    if(!c.enabled) return denied(QStringLiteral("条目未启用。"));
    if(!p.compatibilityConfirmed || p.model.trimmed().isEmpty()) return denied(QStringLiteral("请先确认目标型号与所选协议/功能兼容。"));
    if(p.channels<1 || p.channels>255) return denied(QStringLiteral("设备通道数需在1～255。"));
    if(c.protocol!=p.protocol) return denied(QStringLiteral("协议与设备档案不符，不自动跨协议探测。"));
    auto a=recognize(c,p);
    if(a.kind==ReplyKind::None) return a;
    if(!p.groups.contains(a.group)) return denied(QStringLiteral("实际命令功能组不在设备档案中。"));
    a.allowed=true; a.reason=QStringLiteral("已核对只读帧；适用性由本次设备档案限定。"); return a;
}
ReplyResult verifyReply(const Command &c,const Profile &p,const QByteArray &response)
{
    ReplyResult r; r.received=!response.isEmpty();
    const auto a=assess(c,p);
    if(!a.allowed) { r.outcome="not_validated"; r.detail=a.reason; return r; }
    if(response.isEmpty()) { r.outcome="no_reply"; r.detail=QStringLiteral("响应窗口内没有收到数据。"); return r; }
    bool valid=false;
    if(response.size()<=MaxFrameBytes) {
        const auto &b=c.request;
        switch(a.kind) {
        case ReplyKind::BinaryCa:
            valid=response.size()==6 && response.startsWith(a.prefix) && bcc(response)==0;
            r.deviceError=response.size()==4 && response.startsWith(a.prefix) && oneOf(quint8(response[3]),{1,2,3});
            break;
        case ReplyKind::BinaryCc:
        case ReplyKind::BinaryTable: {
            const int length=a.kind==ReplyKind::BinaryCc?7:13+2*(quint8(b[6])*256+quint8(b[7]));
            valid=response.size()==length && response.startsWith(a.prefix) && bcc(response)==0;
            // Reference CC failure frame: head, command, failed field index, error=3.
            r.deviceError=response.size()==4 && response.left(2)==b.left(2)
                && ((response[2]==0 && response[3]==1) || (response[3]==3
                && oneOf(quint8(response[2]),a.kind==ReplyKind::BinaryCc?std::initializer_list<int>{1,2}:std::initializer_list<int>{1,2,3,4})));
            break;
        }
        case ReplyKind::Fixed: valid=response==a.prefix; break;
        case ReplyKind::AsciiDigits:
            valid=response.size()==a.prefix.size()+a.digits && response.startsWith(a.prefix) && decimal(response.mid(a.prefix.size())); break;
        case ReplyKind::AsciiChoice:
            valid=response.size()==a.prefix.size()+1 && response.startsWith(a.prefix) && a.choices.contains(response.back()); break;
        case ReplyKind::Extended: {
            const int channels=b.size()-4;
            valid=response.size()==4+5*channels && response.startsWith(a.prefix) && response.back()=='#';
            for(int i=0;valid && i<channels;++i) valid=response[3+5*i]==b[3+i] && decimal(response.mid(4+5*i,4));
            break;
        }
        default: break;
        }
    }
    r.protocolValid=valid || r.deviceError;
    if(!r.protocolValid) { r.outcome="invalid_reply"; r.detail=QStringLiteral("已收到数据，但完整长度、命令/通道、字符或BCC不符合规则；不做包含匹配。"); return r; }
    r.association=QStringLiteral("字段符合；无事务序号，不能排除相同查询迟到回复或回显。");
    if(r.deviceError) { r.outcome="device_error"; r.detail=QStringLiteral("收到合法错误帧，协议有响应，但查询失败。"); return r; }
    r.expectationChecked=!c.expected.isEmpty(); r.expectationMatched=r.expectationChecked && response==c.expected;
    r.outcome=r.expectationChecked?(r.expectationMatched?"matched":"value_mismatch"):"valid_reply";
    r.detail=r.expectationChecked?(r.expectationMatched?QStringLiteral("完整协议回复符合期望值。"):
        QStringLiteral("协议回复有效，但数据不符合所填期望值。")):QStringLiteral("完整协议回复有效；未设置期望值，未判定数据值是否正确。");
    return r;
}
QJsonObject ReplyResult::toJson() const
{
    return {{"received",received},{"protocol_valid",protocolValid},{"device_error",deviceError},
        {"expectation_checked",expectationChecked},{"expectation_matched",expectationMatched},
        {"outcome",outcome},{"detail",detail},{"association",association}};
}
bool Library::importJson(const QByteArray &json,QString *error)
{
    if(error) error->clear();
    if(json.size()>MaxLibraryBytes) return fail(error,QStringLiteral("指令库超过4MiB。"));
    QJsonParseError parse; const auto doc=QJsonDocument::fromJson(json,&parse);
    if(parse.error!=QJsonParseError::NoError || !doc.isArray()) return fail(error,QStringLiteral("指令库必须为JSON数组：%1").arg(parse.errorString()));
    if(doc.array().size()>MaxCommands) return fail(error,QStringLiteral("指令库超过4096条。"));
    QList<Command> imported; QSet<QString> ids;
    for(const auto &value:doc.array()) {
        if(!value.isObject()) return fail(error,QStringLiteral("指令库中存在非对象条目。"));
        const auto o=value.toObject(); Command c; c.source=o;
        for(const QString &key:{QStringLiteral("command_id"),QStringLiteral("command_name"),QStringLiteral("protocol_type"),QStringLiteral("send_format"),QStringLiteral("send_data")})
            if(!o[key].isString()) return fail(error,QStringLiteral("条目缺少字符串字段：%1").arg(key));
        c.id=o["command_id"].toString(); c.name=o["command_name"].toString(); c.protocol=o["protocol_type"].toString();
        if(c.id.trimmed().isEmpty() || c.id.size()>128 || c.name.size()>256 || ids.contains(c.id)) return fail(error,QStringLiteral("指令ID为空、重复或名称/ID过长。"));
        ids.insert(c.id);
        for(const QString &key:{QStringLiteral("enabled"),QStringLiteral("enable_bcc")})
            if(o.contains(key) && !o[key].isBool()) return fail(error,QStringLiteral("%1必须为布尔值。").arg(key));
        for(const QString &key:{QStringLiteral("expected_reply"),QStringLiteral("expected_reply_format"),QStringLiteral("match_rule")})
            if(o.contains(key) && !o[key].isString()) return fail(error,QStringLiteral("%1必须为字符串。").arg(key));
        c.enabled=o["enabled"].toBool(true);
        const auto format=o["send_format"].toString();
        if(!decode(o["send_data"].toString(),format,&c.request,&c.parseError)) c.request.clear();
        if(c.parseError.isEmpty() && o["enable_bcc"].toBool()) {
            if(format!="hex") c.parseError=QStringLiteral("自动BCC仅适用于HEX。");
            else if(c.request.size()>=MaxFrameBytes) c.parseError=QStringLiteral("追加BCC后超过报文上限。");
            else c.request.append(char(bcc(c.request)));
        }
        if(c.parseError.isEmpty() && c.request.isEmpty()) c.parseError=QStringLiteral("发送内容为空。");
        if(c.parseError.isEmpty()) {
            const QString expectedFormat=o["expected_reply_format"].toString(format);
            if(!decode(o["expected_reply"].toString(),expectedFormat,&c.expected,&c.parseError)) c.expected.clear();
        }
        const auto rule=o["match_rule"].toString("exact");
        if(c.parseError.isEmpty() && rule!="exact" && rule!="strict_frame") c.parseError=QStringLiteral("诊断仅支持完整回复，不能导入contains等匹配规则。" );
        imported.append(c);
    }
    m_commands=imported; return true; // All structural checks passed; no partial replacement on error.
}
bool Library::loadFile(const QString &path,QString *error)
{
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)) return fail(error,file.errorString());
    return importJson(file.read(MaxLibraryBytes+1),error);
}
bool Library::loadDefaults(QString *error) { initializeCommandResources(); return loadFile(":/diagnostic/default_commands.json",error); }
QByteArray Library::toJson() const { QJsonArray a; for(const auto &c:m_commands) a.append(c.source); return QJsonDocument(a).toJson(); }
bool Library::saveCopy(const QString &path,QString *error) const
{
    if(error) error->clear();
    if(!QDir().mkpath(QFileInfo(path).absolutePath())) return fail(error,QStringLiteral("无法创建独立指令库目录。"));
    QSaveFile file(path); const auto bytes=toJson();
    if(bytes.size()>MaxLibraryBytes) return fail(error,QStringLiteral("规范化后的库超过4MiB，未覆盖文件。"));
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) return fail(error,file.errorString());
    return true;
}
QString Library::defaultPath() { return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("commands.json"); }
QList<Command> Library::eligible(const Profile &profile) const
{
    QList<Command> out; QSet<QByteArray> seen;
    for(const auto &c:m_commands) if(assess(c,profile).allowed && !seen.contains(c.request)) { out.append(c); seen.insert(c.request); }
    std::stable_sort(out.begin(),out.end(),[&](const Command &a,const Command &b) { return assess(a,profile).priority<assess(b,profile).priority; });
    return out;
}
} // namespace Commands
