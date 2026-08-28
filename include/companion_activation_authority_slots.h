#ifndef COMPANION_ACTIVATION_AUTHORITY_SLOTS_H
#define COMPANION_ACTIVATION_AUTHORITY_SLOTS_H

#include "authority_slot_publication.h"

// 激活日志的 A/B 双槽发布域。发布规则本身在 AuthoritySlotPublication 里，因为
// 扩展复核记录面对的是同一个危险：授权信封只有一份副本时，一次被打断的写入会
// 销毁唯一的 HMAC 密钥副本，使已存记录永久无法认证。这里只固定激活日志自己的
// 模式串与摘要域，它们参与已持久化的字节，因此不能更改。
class CompanionActivationAuthoritySlots
{
public:
    // 激活日志自己的域。适配器必须从这里取值，而不是自己再抄一份常量：两份副本会
    // 各自漂移，而这些字节已经被持久化。
    static AuthoritySlotDomain domain();

    // 代号与载荷共同参与摘要，因此旧摘要不能与新载荷混用。
    static QByteArray frame(qint64 generation, const QByteArray &payload);
    static bool parseFrame(const QByteArray &frame, qint64 *generation,
                           QByteArray *payload);
    static AuthoritySlotSelection select(const AuthoritySlotInput &slotA,
                                         const AuthoritySlotInput &slotB,
                                         const AuthoritySlotInput &legacy);
};

#endif // COMPANION_ACTIVATION_AUTHORITY_SLOTS_H
