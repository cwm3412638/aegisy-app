#include "companion_activation_authority_slots.h"

#include "authority_slot_publication.h"

namespace {

// 这些常量参与已持久化的字节，因此不能更改：它们是槽位格式的一部分。
const char kFrameSchema[] =
    "aegisy-companion-activation-journal-authority-slot/0.1";
const char kDigestDomain[] =
    "aegisy-companion-activation-journal-authority-slot-digest/0.1\0";

// A/B 发布逻辑对每个子系统都相同，激活日志只提供自己的域。域参与摘要与模式串，
// 因此扩展复核记录的槽位字节无法被搬到激活作用域里冒充有效授权。
AuthoritySlotDomain domain()
{
    AuthoritySlotDomain value;
    value.frameSchema = QByteArray(kFrameSchema, sizeof(kFrameSchema) - 1);
    value.digestDomain = QByteArray(kDigestDomain, sizeof(kDigestDomain) - 1);
    value.errorPrefix = QStringLiteral("activation-authority-slot-");
    return value;
}

} // namespace

QByteArray CompanionActivationAuthoritySlots::frame(
    qint64 generation, const QByteArray &payload)
{
    return AuthoritySlotPublication::frame(domain(), generation, payload);
}

bool CompanionActivationAuthoritySlots::parseFrame(
    const QByteArray &frameBytes, qint64 *generation, QByteArray *payload)
{
    return AuthoritySlotPublication::parseFrame(
        domain(), frameBytes, generation, payload);
}

AuthoritySlotSelection CompanionActivationAuthoritySlots::select(
    const AuthoritySlotInput &slotA, const AuthoritySlotInput &slotB,
    const AuthoritySlotInput &legacy)
{
    return AuthoritySlotPublication::select(domain(), slotA, slotB, legacy);
}
