#include "core/LocalTcpConnection.h"
#include <QFileInfo>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#endif

namespace LocalTcp {
QJsonObject findEstablished(const QHostAddress &target, quint16 port)
{
#ifdef Q_OS_WIN
    ULONG bytes=0;
    if(GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)!=ERROR_INSUFFICIENT_BUFFER || bytes==0) return {};
    QByteArray buffer(int(bytes),Qt::Uninitialized);
    auto *table=reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());
    if(GetExtendedTcpTable(table,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)!=NO_ERROR) return {};
    for(DWORD i=0;i<table->dwNumEntries;++i) {
        const auto &row=table->table[i];
        if(row.dwState!=MIB_TCP_STATE_ESTAB || QHostAddress(ntohl(row.dwRemoteAddr))!=target
            || ntohs(u_short(row.dwRemotePort))!=port) continue;
        QString processName;
        const HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,row.dwOwningPid);
        if(process) {
            wchar_t path[32768]; DWORD length=DWORD(sizeof(path)/sizeof(path[0]));
            if(QueryFullProcessImageNameW(process,0,path,&length)) processName=QFileInfo(QString::fromWCharArray(path,int(length))).fileName();
            CloseHandle(process);
        }
        return {{"pid",QString::number(quint64(row.dwOwningPid))},{"process_name",processName},
            {"local_ip",QHostAddress(ntohl(row.dwLocalAddr)).toString()},{"local_port",int(ntohs(u_short(row.dwLocalPort)))},
            {"remote_ip",target.toString()},{"remote_port",int(port)},{"state","established"}};
    }
#else
    Q_UNUSED(target); Q_UNUSED(port);
#endif
    return {};
}
}
