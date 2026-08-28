#ifndef EXTENSION_EVIDENCE_LEDGER_H
#define EXTENSION_EVIDENCE_LEDGER_H

#include "extension_registry.h"

#include <QByteArray>
#include <QList>
#include <QString>

// 扩展证据的认证记录层。复核证据与启用授权是两类不同的证据，但它们的记录形态完全
// 一致：都把 (种类, ID, 来源身份, 内容身份) 联合绑定成一个集合，都需要一个单调代号，
// 都必须防住追加、删除、重排与单字段替换。把这套逻辑复制两份会产生两个可以各自漂移
// 的副本，因此它被抽取到这一层，由调用方提供自己的域。
//
// 域分隔是这一层的安全性质，不是格式细节。模式串、MAC 域与身份域全部进入被持久化的
// 字节，因此一类证据的记录在另一类里无法解析，仅仅改写 `schema` 字段也没有用。未配置
// 的域被直接拒绝，而不是退回某个默认格式。
//
// 这一层只做编解码与校验。它不持久化、不判定信任、不授予启用、不执行任何东西：解析出
// 的条目仍然要交给各自的策略层判定。
struct ExtensionEvidenceEntry {
    ExtensionKind kind = ExtensionKind::Skill;
    QString id;
    // 记录证据时确切的来源与内容摘要，格式与注册表一致。
    QString sourceIdentity;
    QString contentIdentity;
};

// 调用方的域。任何一项为空都会让编解码整体失败，因此不存在"缺省域"。
struct ExtensionEvidenceLedgerDomain {
    QString schema;
    // 已经包含各自域末尾分隔字节的确切前缀，必须与历史持久化字节一致。
    QByteArray macDomain;
    QByteArray identityDomain;
    // 被持久化的 JSON 里存放条目数组的键名。
    QString entriesKey;
    // 条目相关固定诊断代码里的名词，例如 `pin`。它进入诊断代码而不是持久化字节，
    // 因此既保持各调用方原有的代码不变，也不影响载荷兼容性。
    QString entryCodeNoun;
    QString identityPrefix;
    // 固定诊断代码的前缀，例如 `extension-review-ledger`。
    QString errorPrefix;

    bool configured() const
    {
        return !schema.isEmpty() && !macDomain.isEmpty()
            && !identityDomain.isEmpty() && !entriesKey.isEmpty()
            && !entryCodeNoun.isEmpty() && !identityPrefix.isEmpty()
            && !errorPrefix.isEmpty();
    }
};

// 反降级性质：被篡改的载荷永远得出 Invalid，绝不会退化成 Empty。退化会把"记录被改坏
// 了"表述成"从未记录过"，两者在故障诊断上完全不同，尽管两者都不授予任何权限。
enum class ExtensionEvidenceLedgerState {
    // 确实没有任何载荷。只有空输入会得出这个结论。
    Empty,
    Ready,
    // 载荷存在但无法认证或格式不合法。
    Invalid,
    // 无法读取载荷，因此当前内容未知。
    Unavailable,
};

struct ExtensionEvidenceLedgerResult {
    ExtensionEvidenceLedgerState state = ExtensionEvidenceLedgerState::Invalid;
    QList<ExtensionEvidenceEntry> entries;
    qint64 generation = 0;
    // 载荷内容的域分隔摘要，仅用于诊断与漂移比较。
    QString identity;
    QString errorCode;
};

class ExtensionEvidenceLedger
{
public:
    static constexpr int MaxEntries = ExtensionRegistry::MaxRecords;
    static constexpr qint64 MaxRecordBytes = 512 * 1024;
    static constexpr qint64 MaxGeneration = 9007199254740991LL;

    // 生成认证载荷。域未配置、代号非法、条目不合法、存在重复 (kind, id)、超出数量
    // 上限或密钥长度不为 32 字节时返回空字节序列。
    static QByteArray serialize(const ExtensionEvidenceLedgerDomain &domain,
                                qint64 generation,
                                const QList<ExtensionEvidenceEntry> &entries,
                                const QByteArray &key);

    static ExtensionEvidenceLedgerResult parse(
        const ExtensionEvidenceLedgerDomain &domain,
        const QByteArray &bytes,
        const QByteArray &key);
};

#endif // EXTENSION_EVIDENCE_LEDGER_H
