#include "companion_activation_authority_slots.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

namespace {

using Slots = CompanionActivationAuthoritySlots;

bool expect(bool condition, const char *message)
{
    if (condition) return true;
    QTextStream(stderr) << message << Qt::endl;
    return false;
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

AuthoritySlotInput invalid()
{
    AuthoritySlotInput input;
    input.state = AuthoritySlotReadState::Invalid;
    return input;
}

QByteArray withField(const QByteArray &frame, const QString &key,
                     const QJsonValue &value)
{
    QJsonObject object = QJsonDocument::fromJson(frame).object();
    if (value.isUndefined()) object.remove(key);
    else object.insert(key, value);
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

} // namespace

int main()
{
    const QByteArray payloadOne = QByteArrayLiteral("{\"authority\":1}");
    const QByteArray payloadTwo = QByteArrayLiteral("{\"authority\":2}");
    const QByteArray frameOne = Slots::frame(1, payloadOne);
    const QByteArray frameTwo = Slots::frame(2, payloadTwo);
    if (!expect(!frameOne.isEmpty() && !frameTwo.isEmpty(),
                "framing a valid payload failed")) return 1;
    if (!expect(Slots::frame(0, payloadOne).isEmpty()
                    && Slots::frame(1, QByteArray()).isEmpty(),
                "framing accepted an invalid generation or empty payload")) return 1;

    qint64 generation = 0;
    QByteArray payload;
    if (!expect(Slots::parseFrame(frameOne, &generation, &payload)
                    && generation == 1 && payload == payloadOne,
                "frame round trip failed")) return 1;
    // 摘要覆盖代号与载荷，因此任何字段替换都不能通过校验。
    if (!expect(!Slots::parseFrame(
                    withField(frameOne, QStringLiteral("generation"), 7),
                    &generation, &payload),
                "a substituted generation passed the digest")) return 1;
    if (!expect(!Slots::parseFrame(
                    withField(frameOne, QStringLiteral("payload_base64"),
                              QString::fromLatin1(payloadTwo.toBase64())),
                    &generation, &payload),
                "a substituted payload passed the digest")) return 1;
    if (!expect(!Slots::parseFrame(
                    withField(frameOne, QStringLiteral("digest"), QJsonValue()),
                    &generation, &payload),
                "a frame without a digest was accepted")) return 1;
    if (!expect(!Slots::parseFrame(
                    withField(frameOne, QStringLiteral("extra"), 1),
                    &generation, &payload),
                "an unknown frame field was accepted")) return 1;
    if (!expect(!Slots::parseFrame(
                    withField(frameOne, QStringLiteral("generation"), 1.5),
                    &generation, &payload),
                "a fractional generation was accepted")) return 1;

    // 首次安装：两个槽位与旧作用域都缺失，且第一次发布落在 A。
    const AuthoritySlotSelection fresh =
        Slots::select(missing(), missing(), missing());
    if (!expect(fresh.state == AuthoritySlotSelectionState::Missing
                    && fresh.writeSlot == AuthoritySlotName::SlotA
                    && fresh.writeGeneration == 1 && !fresh.legacyPending,
                "a clean install was not classified as missing")) return 1;

    // 只有 A 存在：发布必须写入 B，选中的代号因此保持完好。
    const AuthoritySlotSelection onlyA =
        Slots::select(found(frameOne), missing(), missing());
    if (!expect(onlyA.state == AuthoritySlotSelectionState::Found
                    && onlyA.payload == payloadOne && onlyA.generation == 1
                    && onlyA.writeSlot == AuthoritySlotName::SlotB
                    && onlyA.writeGeneration == 2,
                "a single published slot did not target its peer")) return 1;

    // 较新的代号胜出，且下一次发布回写较旧的槽位。
    const AuthoritySlotSelection both =
        Slots::select(found(frameOne), found(frameTwo), missing());
    if (!expect(both.state == AuthoritySlotSelectionState::Found
                    && both.payload == payloadTwo && both.generation == 2
                    && both.writeSlot == AuthoritySlotName::SlotA
                    && both.writeGeneration == 3,
                "the newer generation did not win")) return 1;
    const AuthoritySlotSelection swapped =
        Slots::select(found(frameTwo), found(frameOne), missing());
    if (!expect(swapped.state == AuthoritySlotSelectionState::Found
                    && swapped.payload == payloadTwo
                    && swapped.writeSlot == AuthoritySlotName::SlotB,
                "slot order changed the selection")) return 1;

    // 一次被打断的发布只损坏目标槽位：完好的对端仍然可用。
    const AuthoritySlotSelection torn =
        Slots::select(found(frameTwo), found(QByteArrayLiteral("{}")), missing());
    if (!expect(torn.state == AuthoritySlotSelectionState::Found
                    && torn.payload == payloadTwo
                    && torn.writeSlot == AuthoritySlotName::SlotB
                    && torn.writeGeneration == 3,
                "a torn publication destroyed a recoverable authority")) return 1;

    // 两个槽位同时损坏无法恢复，但绝不能退化成"没有事务"。
    const AuthoritySlotSelection bothCorrupt = Slots::select(
        found(QByteArrayLiteral("{}")), found(QByteArrayLiteral("{}")), missing());
    if (!expect(bothCorrupt.state == AuthoritySlotSelectionState::Invalid
                    && bothCorrupt.errorCode
                        == QStringLiteral("activation-authority-slot-both-corrupt"),
                "two corrupt slots degraded to an empty authority")) return 1;
    const AuthoritySlotSelection loneCorrupt =
        Slots::select(found(QByteArrayLiteral("{}")), missing(), missing());
    if (!expect(loneCorrupt.state == AuthoritySlotSelectionState::Invalid
                    && loneCorrupt.errorCode == QStringLiteral(
                        "activation-authority-slot-corrupt-without-peer"),
                "a corrupt slot without a peer degraded to an empty authority")) {
        return 1;
    }

    // 同代号但载荷不同意味着有一次未预期的并发发布：不可推断。
    const AuthoritySlotSelection conflict = Slots::select(
        found(Slots::frame(3, payloadOne)), found(Slots::frame(3, payloadTwo)),
        missing());
    if (!expect(conflict.state == AuthoritySlotSelectionState::Invalid
                    && conflict.errorCode == QStringLiteral(
                        "activation-authority-slot-generation-conflict"),
                "conflicting same-generation payloads were accepted")) return 1;
    const AuthoritySlotSelection sameFrame =
        Slots::select(found(frameTwo), found(frameTwo), missing());
    if (!expect(sameFrame.state == AuthoritySlotSelectionState::Found
                    && sameFrame.payload == payloadTwo,
                "identical slots were treated as a conflict")) return 1;

    // 任一槽位不可用时不能推断：可能有一个读不出来的更新代号。
    if (!expect(Slots::select(unavailable(), found(frameTwo), missing()).state
                    == AuthoritySlotSelectionState::Unavailable
                    && Slots::select(found(frameTwo), unavailable(), missing()).state
                        == AuthoritySlotSelectionState::Unavailable
                    && Slots::select(missing(), missing(), unavailable()).state
                        == AuthoritySlotSelectionState::Unavailable,
                "a locked backend was mistaken for a readable authority")) return 1;
    if (!expect(Slots::select(invalid(), missing(), missing()).state
                    == AuthoritySlotSelectionState::Invalid,
                "an invalid backend degraded to an empty authority")) return 1;

    // 迁移：旧单槽载荷作为代号 1 被采纳，首次双槽发布写入 A 并标记待清理。
    const AuthoritySlotSelection legacy =
        Slots::select(missing(), missing(), found(payloadOne));
    if (!expect(legacy.state == AuthoritySlotSelectionState::Found
                    && legacy.payload == payloadOne && legacy.generation == 1
                    && legacy.writeSlot == AuthoritySlotName::SlotA
                    && legacy.writeGeneration == 2 && legacy.legacyPending,
                "the single-slot authority did not migrate")) return 1;
    // 已存在双槽发布时，旧作用域的残留不得覆盖已发布的代号。
    const AuthoritySlotSelection legacyIgnored =
        Slots::select(missing(), found(frameTwo), found(payloadOne));
    if (!expect(legacyIgnored.state == AuthoritySlotSelectionState::Found
                    && legacyIgnored.payload == payloadTwo
                    && !legacyIgnored.legacyPending,
                "a legacy remnant overrode a published slot")) return 1;
    if (!expect(Slots::select(missing(), missing(), invalid()).state
                    == AuthoritySlotSelectionState::Invalid,
                "an invalid legacy scope degraded to an empty authority")) return 1;

    // 序号耗尽必须报告，而不是回绕。
    const AuthoritySlotSelection exhausted = Slots::select(
        found(Slots::frame(9007199254740991LL, payloadOne)), missing(), missing());
    if (!expect(exhausted.state == AuthoritySlotSelectionState::Invalid
                    && exhausted.errorCode == QStringLiteral(
                        "activation-authority-slot-generation-exhausted"),
                "an exhausted generation was allowed to wrap")) return 1;

    QTextStream(stdout) << "companion activation authority slots ok" << Qt::endl;
    return 0;
}
