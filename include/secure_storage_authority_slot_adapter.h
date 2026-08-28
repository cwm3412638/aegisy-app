#ifndef SECURE_STORAGE_AUTHORITY_SLOT_ADAPTER_H
#define SECURE_STORAGE_AUTHORITY_SLOT_ADAPTER_H

#include "authority_slot_publication.h"

// 把授权信封落到平台安全存储里的共享搬运层。激活日志、扩展复核记录与扩展启用授权
// 面对的是同一个危险：密钥只有一份副本时，一次被打断的写入就可能销毁它，而密钥不
// 存在于任何其他位置，因此所有已存记录将永久无法认证。A/B 双槽把这种不可恢复的
// 损坏降级为"上一次发布没有生效"，也就是各授权状态机已经能确定性处理的中断点。
//
// 每个使用者必须给出自己的作用域与域串。作用域决定字节落在哪里，模式串与摘要域
// 参与被持久化的字节本身，因此一个子系统的槽位字节不能被搬到另一个子系统的作用域
// 里冒充有效授权。这一层只搬字节：它不判定信任、不授予启用、不执行任何东西。
struct SecureStorageAuthoritySlotScopes {
    AuthoritySlotDomain domain;
    QString slotAScope;
    QString slotBScope;
    // 迁移前的单槽作用域。新增子系统留空，表示没有可采纳的旧授权。
    QString legacyScope;
    // 读写失败的错误码前缀，例如 "extension-enablement-secure"。
    QString errorPrefix;

    bool isValid() const
    {
        return domain.isValid() && !slotAScope.isEmpty() && !slotBScope.isEmpty()
            && slotAScope != slotBScope && !errorPrefix.isEmpty();
    }
};

enum class SecureStorageAuthoritySlotReadState {
    Missing,
    Found,
    Unavailable,
    Invalid,
};

enum class SecureStorageAuthoritySlotWriteOutcome {
    Committed,
    DefiniteFailure,
    OutcomeUnknown,
};

class SecureStorageAuthoritySlotAdapter
{
public:
    static constexpr int MaxAuthorityBytes = 32 * 1024;

    static SecureStorageAuthoritySlotReadState readFresh(
        const SecureStorageAuthoritySlotScopes &scopes,
        QByteArray *value, QString *errorCode);
    static SecureStorageAuthoritySlotWriteOutcome write(
        const SecureStorageAuthoritySlotScopes &scopes,
        const QByteArray &value, QString *errorCode);
};

#endif // SECURE_STORAGE_AUTHORITY_SLOT_ADAPTER_H
