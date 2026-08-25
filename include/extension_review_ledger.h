#ifndef EXTENSION_REVIEW_LEDGER_H
#define EXTENSION_REVIEW_LEDGER_H

#include "extension_trust_policy.h"

#include <QByteArray>
#include <QList>
#include <QString>

// 复核记录的认证记录层。信任完全由复核记录决定，因此能够改写普通配置的人不能因此
// 伪造一条复核：载荷经过域分隔的 HMAC-SHA256 认证，MAC 联合覆盖代号与整个复核
// 集合，任何字段都无法被单独替换。
//
// 反降级性质与激活日志一致：被篡改的载荷永远得出 Invalid，绝不会退化成 Empty。
// 退化成 Empty 会把"复核记录被改坏了"表述成"从未复核过"，两者在故障诊断上完全
// 不同——尽管两者都不授予信任，因此二者都是安全的失败。
//
// 这一层只做编解码与校验。它不持久化、不安装、不启用、不执行任何东西：解析出的
// 复核记录仍然要经过 ExtensionTrustPolicy 判定，启用仍然需要注册表的
// Verified + Compatible 门禁加上一个独立的启用动作。
enum class ExtensionReviewLedgerState {
    // 确实没有任何载荷。只有空输入会得出这个结论。
    Empty,
    Ready,
    // 载荷存在但无法认证或格式不合法。
    Invalid,
    // 无法读取载荷，因此当前内容未知。
    Unavailable,
};

struct ExtensionReviewLedgerResult {
    ExtensionReviewLedgerState state = ExtensionReviewLedgerState::Invalid;
    QList<ExtensionReviewPin> pins;
    qint64 generation = 0;
    // 载荷内容的域分隔摘要，仅用于诊断与漂移比较。
    QString identity;
    QString errorCode;
};

class ExtensionReviewLedger
{
public:
    static constexpr int MaxPins = ExtensionTrustPolicy::MaxReviewPins;
    static constexpr qint64 MaxRecordBytes = 512 * 1024;
    static constexpr qint64 MaxGeneration = 9007199254740991LL;

    // 生成认证载荷。代号非法、复核记录不合法、存在重复 (kind, id)、超出数量上限
    // 或密钥长度不为 32 字节时返回空字节序列。
    static QByteArray serialize(qint64 generation,
                                const QList<ExtensionReviewPin> &pins,
                                const QByteArray &key);

    static ExtensionReviewLedgerResult parse(const QByteArray &bytes,
                                             const QByteArray &key);
};

#endif // EXTENSION_REVIEW_LEDGER_H
