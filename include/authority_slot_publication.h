#ifndef AUTHORITY_SLOT_PUBLICATION_H
#define AUTHORITY_SLOT_PUBLICATION_H

#include <QByteArray>
#include <QString>

// 授权信封只有一份副本时，一次被打断的安全存储写入就可能销毁 HMAC 密钥本身，
// 而密钥不存在于任何其他位置，因此已存记录将永久无法认证。A/B 双槽发布把这种
// 不可恢复的损坏降级为"上一次发布没有生效"——这是各授权状态机已经能确定性
// 处理的情形。每次发布只写入持有较旧代号的槽位，另一个槽位始终保持完好。
//
// 这一层不属于任何单个子系统：激活日志与扩展复核记录面对的是同一个危险。每个
// 使用者必须给出自己的域，这样一个子系统的槽位字节不能被搬到另一个子系统的
// 作用域里冒充有效授权。
enum class AuthoritySlotReadState {
    Missing,
    Found,
    Unavailable,
    Invalid,
};

enum class AuthoritySlotName {
    SlotA,
    SlotB,
};

struct AuthoritySlotInput {
    AuthoritySlotReadState state = AuthoritySlotReadState::Missing;
    QByteArray frame;
};

enum class AuthoritySlotSelectionState {
    Missing,
    Found,
    Unavailable,
    Invalid,
};

struct AuthoritySlotSelection {
    AuthoritySlotSelectionState state = AuthoritySlotSelectionState::Invalid;
    QByteArray payload;
    qint64 generation = 0;
    // 下一次发布的目标：持有较旧代号（或已损坏）的槽位。
    AuthoritySlotName writeSlot = AuthoritySlotName::SlotA;
    qint64 writeGeneration = 1;
    // 选中的载荷来自迁移前的单槽，双槽发布确认后才可移除它。
    bool legacyPending = false;
    QString errorCode;
};

// 模式串与摘要域都参与持久化字节，因此它们是格式的一部分，不能事后更改。
struct AuthoritySlotDomain {
    QByteArray frameSchema;
    QByteArray digestDomain;
    // 错误码前缀，让每个子系统的失败结论保持可区分。
    QString errorPrefix;

    bool isValid() const
    {
        return !frameSchema.isEmpty() && !digestDomain.isEmpty()
            && !errorPrefix.isEmpty();
    }
};

class AuthoritySlotPublication
{
public:
    // 代号与载荷共同参与摘要，因此旧摘要不能与新载荷混用。
    static QByteArray frame(const AuthoritySlotDomain &domain, qint64 generation,
                            const QByteArray &payload);
    static bool parseFrame(const AuthoritySlotDomain &domain,
                           const QByteArray &frame, qint64 *generation,
                           QByteArray *payload);
    static AuthoritySlotSelection select(const AuthoritySlotDomain &domain,
                                         const AuthoritySlotInput &slotA,
                                         const AuthoritySlotInput &slotB,
                                         const AuthoritySlotInput &legacy);
};

#endif // AUTHORITY_SLOT_PUBLICATION_H
