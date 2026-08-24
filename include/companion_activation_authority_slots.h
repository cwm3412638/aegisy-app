#ifndef COMPANION_ACTIVATION_AUTHORITY_SLOTS_H
#define COMPANION_ACTIVATION_AUTHORITY_SLOTS_H

#include <QByteArray>
#include <QString>

// 授权信封只有一份副本时，一次被打断的安全存储写入就可能销毁 HMAC 密钥本身，
// 而密钥不存在于任何其他位置，因此事务将永久无法认证。A/B 双槽发布把这种
// 不可恢复的损坏降级为"上一次发布没有生效"——这是日志状态机已经能确定性
// 处理的情形。每次发布只写入持有较旧代号的槽位，另一个槽位始终保持完好。
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

class CompanionActivationAuthoritySlots
{
public:
    // 代号与载荷共同参与摘要，因此旧摘要不能与新载荷混用。
    static QByteArray frame(qint64 generation, const QByteArray &payload);
    static bool parseFrame(const QByteArray &frame, qint64 *generation,
                           QByteArray *payload);
    static AuthoritySlotSelection select(const AuthoritySlotInput &slotA,
                                         const AuthoritySlotInput &slotB,
                                         const AuthoritySlotInput &legacy);
};

#endif // COMPANION_ACTIVATION_AUTHORITY_SLOTS_H
