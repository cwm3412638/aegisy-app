#ifndef EXTENSION_ENABLEMENT_LEDGER_H
#define EXTENSION_ENABLEMENT_LEDGER_H

#include "extension_enablement_policy.h"

#include <QByteArray>
#include <QList>
#include <QString>

// 启用授权的认证记录层。启用授权和复核证据一样必须被认证：能够改写普通配置的人不能
// 因此授权启用一个扩展。两者共用 `ExtensionEvidenceLedger` 的编解码与反降级逻辑，
// 但各自持有独立的域。
//
// 域分隔在这里是必需的安全性质，而不是整洁性：复核证据与启用授权是两类不同的授权。
// 如果它们共用格式，一份复核记录的字节就能被移动到启用授权的位置，从而把"我看过这份
// 内容"直接变成"我要求运行这份内容"。模式串、MAC 域与身份域全部不同，因此一类记录在
// 另一类里无法解析，仅仅改写 `schema` 字段也没有用。
//
// 这一层只做编解码与校验。它不持久化、不判定信任、不安装、不执行任何东西：解析出的
// 授权仍然要交给 ExtensionEnablementPolicy 判定，而判定本身还要求已复核、兼容且已
// 安装。
enum class ExtensionEnablementLedgerState {
    // 确实没有任何载荷。只有空输入会得出这个结论。
    Empty,
    Ready,
    // 载荷存在但无法认证或格式不合法。
    Invalid,
    // 无法读取载荷，因此当前内容未知。
    Unavailable,
};

struct ExtensionEnablementLedgerResult {
    ExtensionEnablementLedgerState state = ExtensionEnablementLedgerState::Invalid;
    QList<ExtensionEnablementGrant> grants;
    qint64 generation = 0;
    // 载荷内容的域分隔摘要，仅用于诊断与漂移比较。
    QString identity;
    QString errorCode;
};

class ExtensionEnablementLedger
{
public:
    static constexpr int MaxGrants = ExtensionEnablementPolicy::MaxGrants;
    static constexpr qint64 MaxRecordBytes = 512 * 1024;
    static constexpr qint64 MaxGeneration = 9007199254740991LL;

    // 生成认证载荷。代号非法、授权不合法、存在重复 (kind, id)、超出数量上限或密钥
    // 长度不为 32 字节时返回空字节序列。
    static QByteArray serialize(qint64 generation,
                                const QList<ExtensionEnablementGrant> &grants,
                                const QByteArray &key);

    static ExtensionEnablementLedgerResult parse(const QByteArray &bytes,
                                                 const QByteArray &key);
};

#endif // EXTENSION_ENABLEMENT_LEDGER_H
