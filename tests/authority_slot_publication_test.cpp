#include "authority_slot_publication.h"

#include "companion_activation_authority_slots.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {

int failures = 0;

bool expect(bool condition, const char *message)
{
    if (!condition) {
        QTextStream(stderr) << "FAIL: " << message << '\n';
        ++failures;
    }
    return condition;
}

AuthoritySlotDomain domainA()
{
    AuthoritySlotDomain value;
    value.frameSchema = QByteArrayLiteral("aegisy-test-authority-slot-a/0.1");
    value.digestDomain =
        QByteArray("aegisy-test-authority-slot-a-digest/0.1\0", 39);
    value.errorPrefix = QStringLiteral("test-a-slot-");
    return value;
}

AuthoritySlotDomain domainB()
{
    AuthoritySlotDomain value;
    value.frameSchema = QByteArrayLiteral("aegisy-test-authority-slot-b/0.1");
    value.digestDomain =
        QByteArray("aegisy-test-authority-slot-b-digest/0.1\0", 39);
    value.errorPrefix = QStringLiteral("test-b-slot-");
    return value;
}

AuthoritySlotInput found(const QByteArray &frame)
{
    AuthoritySlotInput input;
    input.state = AuthoritySlotReadState::Found;
    input.frame = frame;
    return input;
}

AuthoritySlotInput missing()
{
    AuthoritySlotInput input;
    input.state = AuthoritySlotReadState::Missing;
    return input;
}

AuthoritySlotInput unavailable()
{
    AuthoritySlotInput input;
    input.state = AuthoritySlotReadState::Unavailable;
    return input;
}

void domainSeparationTests()
{
    const QByteArray payload = QByteArrayLiteral("{\"authority\":\"A\"}");
    const QByteArray frameA = AuthoritySlotPublication::frame(domainA(), 3, payload);
    const QByteArray frameB = AuthoritySlotPublication::frame(domainB(), 3, payload);
    expect(!frameA.isEmpty() && !frameB.isEmpty() && frameA != frameB,
           "two domains produced identical authority slot frames");

    // 一个子系统的槽位字节不能在另一个子系统里被当作有效授权：否则把复核授权
    // 搬进激活作用域就能冒充一份签发过的授权。
    qint64 generation = 0;
    QByteArray decoded;
    expect(AuthoritySlotPublication::parseFrame(domainA(), frameA, &generation,
                                                &decoded)
               && generation == 3 && decoded == payload,
           "a domain could not parse its own frame");
    expect(!AuthoritySlotPublication::parseFrame(domainB(), frameA, &generation,
                                                 &decoded),
           "a frame from another domain was accepted");
    expect(!AuthoritySlotPublication::parseFrame(domainA(), frameB, &generation,
                                                 &decoded),
           "a frame from another domain was accepted in reverse");

    // 只替换模式串仍然不通过：摘要域也参与摘要。
    QJsonObject object = QJsonDocument::fromJson(frameA).object();
    object.insert(QStringLiteral("schema_version"),
                  QString::fromLatin1(domainB().frameSchema));
    expect(!AuthoritySlotPublication::parseFrame(
               domainB(), QJsonDocument(object).toJson(QJsonDocument::Compact),
               &generation, &decoded),
           "relabelling a frame's schema made it valid in another domain");

    // 错误码带上域前缀，因此两个子系统的失败结论保持可区分。
    const AuthoritySlotSelection blockedA = AuthoritySlotPublication::select(
        domainA(), unavailable(), missing(), missing());
    const AuthoritySlotSelection blockedB = AuthoritySlotPublication::select(
        domainB(), unavailable(), missing(), missing());
    expect(blockedA.state == AuthoritySlotSelectionState::Unavailable
               && blockedA.errorCode.startsWith(domainA().errorPrefix)
               && blockedB.errorCode.startsWith(domainB().errorPrefix)
               && blockedA.errorCode != blockedB.errorCode,
           "authority slot failures are not attributable to their domain");

    // 未配置域必须直接拒绝，而不是回落到某个默认格式。
    expect(AuthoritySlotPublication::frame(AuthoritySlotDomain{}, 1, payload)
               .isEmpty(),
           "an unconfigured domain produced a frame");
    expect(!AuthoritySlotPublication::parseFrame(AuthoritySlotDomain{}, frameA,
                                                 &generation, &decoded),
           "an unconfigured domain parsed a frame");
    expect(AuthoritySlotPublication::select(AuthoritySlotDomain{}, missing(),
                                            missing(), missing())
               .state == AuthoritySlotSelectionState::Invalid,
           "an unconfigured domain selected a slot");
}

void sharedBehaviourTests()
{
    const AuthoritySlotDomain domain = domainA();
    const QByteArray payload = QByteArrayLiteral("{\"authority\":\"shared\"}");

    // 首次安装：两槽皆缺失，从代号 1 开始。
    const AuthoritySlotSelection first = AuthoritySlotPublication::select(
        domain, missing(), missing(), missing());
    expect(first.state == AuthoritySlotSelectionState::Missing
               && first.writeGeneration == 1
               && first.writeSlot == AuthoritySlotName::SlotA,
           "a fresh pair of slots was not treated as first install");

    // 选中较新的代号，并且始终写入对端槽位。
    const QByteArray frame1 =
        AuthoritySlotPublication::frame(domain, 1, payload);
    const QByteArray frame2 = AuthoritySlotPublication::frame(
        domain, 2, QByteArrayLiteral("{\"authority\":\"newer\"}"));
    const AuthoritySlotSelection newer = AuthoritySlotPublication::select(
        domain, found(frame1), found(frame2), missing());
    expect(newer.state == AuthoritySlotSelectionState::Found
               && newer.generation == 2
               && newer.writeSlot == AuthoritySlotName::SlotA
               && newer.writeGeneration == 3,
           "slot selection did not target the older peer");

    // 后端不可用时不能推断：可能有一个更新的代号读不出来。
    expect(AuthoritySlotPublication::select(domain, unavailable(), found(frame2),
                                            missing())
               .state == AuthoritySlotSelectionState::Unavailable,
           "a locked slot backend was read as a definite state");

    // 同代号但载荷不同必须拒绝，不能任选一个。
    const QByteArray conflicting = AuthoritySlotPublication::frame(
        domain, 2, QByteArrayLiteral("{\"authority\":\"other\"}"));
    const AuthoritySlotSelection conflict = AuthoritySlotPublication::select(
        domain, found(frame2), found(conflicting), missing());
    expect(conflict.state == AuthoritySlotSelectionState::Invalid
               && conflict.errorCode
                   == domain.errorPrefix + QStringLiteral("generation-conflict"),
           "conflicting same-generation slots were accepted");

    // 单槽损坏可由对端恢复；两槽同时损坏绝不能退化成"没有授权"。
    expect(AuthoritySlotPublication::select(domain, found("not json"),
                                            found(frame2), missing())
               .state == AuthoritySlotSelectionState::Found,
           "a corrupt slot was not recovered from its peer");
    const AuthoritySlotSelection bothCorrupt = AuthoritySlotPublication::select(
        domain, found("not json"), found("also not json"), missing());
    expect(bothCorrupt.state == AuthoritySlotSelectionState::Invalid
               && bothCorrupt.errorCode
                   == domain.errorPrefix + QStringLiteral("both-corrupt"),
           "two corrupt slots degraded to an empty authority");
    const AuthoritySlotSelection lonelyCorrupt = AuthoritySlotPublication::select(
        domain, found("not json"), missing(), missing());
    expect(lonelyCorrupt.state == AuthoritySlotSelectionState::Invalid
               && lonelyCorrupt.errorCode
                   == domain.errorPrefix + QStringLiteral("corrupt-without-peer"),
           "a corrupt slot without a peer degraded to first install");

    // 代号耗尽必须报告，而不是回绕。
    const QByteArray exhausted =
        AuthoritySlotPublication::frame(domain, 9007199254740991LL, payload);
    const AuthoritySlotSelection full = AuthoritySlotPublication::select(
        domain, found(exhausted), missing(), missing());
    expect(full.state == AuthoritySlotSelectionState::Invalid
               && full.errorCode
                   == domain.errorPrefix + QStringLiteral("generation-exhausted"),
           "an exhausted generation was allowed to advance");
}

// 独立复算激活日志的槽位摘要：域串、8 字节大端长度前缀、代号与载荷的顺序都在
// 这里写死，因此实现侧任何一项改动都会被发现。
QString expectedCompanionDigest(qint64 generation, const QByteArray &payload)
{
    const char domainBytes[] =
        "aegisy-companion-activation-journal-authority-slot-digest/0.1\0";
    QByteArray input(domainBytes, sizeof(domainBytes) - 1);
    const auto appendSized = [&input](const QByteArray &value) {
        const quint64 size = static_cast<quint64>(value.size());
        for (int shift = 56; shift >= 0; shift -= 8) {
            input.append(static_cast<char>((size >> shift) & 0xff));
        }
        input.append(value);
    };
    appendSized(QByteArray::number(generation));
    appendSized(payload);
    return QString::fromLatin1(
        QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

void companionCompatibilityTests()
{
    // 提取共享层不能改变已持久化的字节：激活日志的槽位格式必须与提取前逐字节
    // 一致，否则现有安装会读不出自己的授权。
    const QByteArray payload = QByteArrayLiteral("{\"authority\":\"companion\"}");
    const QByteArray frame =
        CompanionActivationAuthoritySlots::frame(4, payload);
    const QJsonObject object = QJsonDocument::fromJson(frame).object();
    expect(object.value(QStringLiteral("schema_version")).toString()
               == QStringLiteral(
                   "aegisy-companion-activation-journal-authority-slot/0.1"),
           "the companion slot schema drifted while extracting the shared layer");
    // 摘要在测试里独立重算，而不是引用实现的常量：这样格式漂移会被发现，而不是
    // 跟着实现一起漂移。
    expect(object.value(QStringLiteral("digest")).toString()
               == expectedCompanionDigest(4, payload),
           "the companion slot digest drifted while extracting the shared layer");

    qint64 generation = 0;
    QByteArray decoded;
    expect(CompanionActivationAuthoritySlots::parseFrame(frame, &generation,
                                                         &decoded)
               && generation == 4 && decoded == payload,
           "the companion facade could not parse its own frame");
    // 迁移路径仍然保留：旧的单槽授权按代号 1 采纳。
    const AuthoritySlotSelection legacy = CompanionActivationAuthoritySlots::select(
        missing(), missing(), found(QByteArrayLiteral("legacy-envelope")));
    expect(legacy.state == AuthoritySlotSelectionState::Found
               && legacy.generation == 1 && legacy.legacyPending
               && legacy.writeGeneration == 2,
           "the companion legacy migration path was lost");
    const AuthoritySlotSelection blocked =
        CompanionActivationAuthoritySlots::select(unavailable(), missing(),
                                                  missing());
    expect(blocked.errorCode
               == QStringLiteral("activation-authority-slot-unavailable"),
           "the companion error codes drifted while extracting the shared layer");
}

} // namespace

int main()
{
    domainSeparationTests();
    sharedBehaviourTests();
    companionCompatibilityTests();
    if (failures == 0) {
        QTextStream(stdout) << "authority slot publication tests passed\n";
    }
    return failures == 0 ? 0 : 1;
}
