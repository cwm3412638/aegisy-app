#ifndef EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_H
#define EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_H

#include "extension_staging_restore_presentation.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

// 暂存恢复审批的认证审计记录层。恢复审批是授权：只要它被记录，一份可编辑的普通文件就能让
// 任何人通过改字来伪造"用户同意过这次恢复"——这正是复核记录与启用授权各自获得认证账本的
// 同一个论证。因此恢复审批的决定（批准与拒绝）记录在自己的认证审计链里。
//
// 条目形状绑定的是凭据绑定的内容，而不是复核/启用条目的 (kind, id, sourceIdentity,
// contentIdentity)：主体、备份 id、目标根、计划身份、树身份、披露的警告集合、决定与决定
// 时间。形状根本不同（含一个有界列表与一个时间戳，且同一份备份被反复批准/拒绝是合法历史，
// 不存在 (kind, id) 去重），因此本层不参数化共享的 ExtensionEvidenceLedger，而是沿用它的
// 全部成文约定独立实现：域分隔的 HMAC-SHA256、8 字节大端长度前缀分帧、有界单调代号、
// CRYPTO_memcmp 比较、OPENSSL_cleanse 清理。共享编解码器的字节因此一字节未动，既有安装的
// 复核/启用载荷兼容性由它们自己未修改的测试继续证明。
//
// 域分隔是这一层的安全性质，不是格式细节：模式串、MAC 域与身份域全部独立并进入被持久化
// 的字节，因此一条恢复审计记录无法被解析成复核记录或启用授权，反过来也一样——"用户同意
// 把这份备份写回目标"绝不能被搬成"我看过这份内容"或"我要求运行这份内容"。
//
// 拒绝同样被记录：只记录批准的日志无法区分"用户拒绝了"与"从未问过用户"，记录拒绝证明
// 这个问题被问过并且被回答了。拒绝条目不携带任何授权——本层不授予任何东西。
//
// 反降级性质与既有账本一致：被篡改的载荷永远得出 Invalid，绝不会退化成 Empty。退化会把
// "审计记录被改坏了"表述成"从未记录过任何决定"。一份已认证的空日志（Ready 且零条目）是
// 合法的"已审计、尚无记录"状态，与"从未建立账本"（Empty）严格区分。
//
// 这一层只做编解码与校验。它不持久化、不判定批准是否有效（判定属于独立的恢复审批
// 策略层，两个域互不借用）、不执行任何东西：本层记录决定，从不对恢复请求做判定，
// 读出的条目今天没有任何消费方。
enum class ExtensionStagingRestoreAuditDecision {
    Approved,
    Declined,
};

// 一条恢复审批审计记录。所有身份在写入前按形状校验；警告集合按呈现层固定的枚举顺序
// 排列（严格递增、无重复），因此同一个逻辑集合只有一个规范字节形。
struct ExtensionStagingRestoreAuditEntry {
    QString subject;
    QString backupId;
    QString destinationRoot;
    // 凭据绑定的确切计划身份与树身份（完整形式，与提示回显逐字节一致）。
    QString planIdentity;
    QString treeIdentity;
    // 决定做出时屏幕上确切披露的警告集合。
    QList<ExtensionStagingRestoreWarning> warnings;
    ExtensionStagingRestoreAuditDecision decision =
        ExtensionStagingRestoreAuditDecision::Declined;
    // 决定时间由调用方注入（本层不自带时钟），必须是有效的 UTC 时间。
    QDateTime decidedAt;
};

enum class ExtensionStagingRestoreAuditLedgerState {
    // 确实没有任何载荷。只有空输入会得出这个结论。
    Empty,
    Ready,
    // 载荷存在但无法认证或格式不合法。
    Invalid,
    // 无法读取载荷，因此当前内容未知。
    Unavailable,
};

struct ExtensionStagingRestoreAuditLedgerResult {
    ExtensionStagingRestoreAuditLedgerState state =
        ExtensionStagingRestoreAuditLedgerState::Invalid;
    QList<ExtensionStagingRestoreAuditEntry> entries;
    qint64 generation = 0;
    // 载荷内容的域分隔摘要，仅用于诊断与漂移比较。
    QString identity;
    QString errorCode;
};

class ExtensionStagingRestoreAuditLedger
{
public:
    // 审计链的上界。恢复是罕见的一次性决定，1024 条已审计决定足够宽松；写满后持久化层
    // 以独立代号拒绝新条目，绝不静默驱逐历史。
    static constexpr int MaxEntries = 1024;
    static constexpr qint64 MaxRecordBytes = 512 * 1024;
    static constexpr qint64 MaxGeneration = 9007199254740991LL;
    // 披露警告集合的上界等于警告枚举的基数。
    static constexpr int MaxWarnings = 6;

    // 生成认证载荷。代号非法、条目不合法、超出数量上限或密钥长度不为 32 字节时返回空
    // 字节序列。审计条目允许重复（同一份备份可以被多次批准或拒绝），不做去重。
    static QByteArray serialize(
        qint64 generation,
        const QList<ExtensionStagingRestoreAuditEntry> &entries,
        const QByteArray &key);

    static ExtensionStagingRestoreAuditLedgerResult parse(
        const QByteArray &bytes, const QByteArray &key);
};

#endif // EXTENSION_STAGING_RESTORE_AUDIT_LEDGER_H
