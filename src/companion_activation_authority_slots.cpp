#include "companion_activation_authority_slots.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>

#include <cmath>

namespace {

constexpr qint64 kMaximumGeneration = 9007199254740991LL;
constexpr int kMaximumFrameBytes = 32 * 1024;

const char kFrameSchema[] =
    "aegisy-companion-activation-journal-authority-slot/0.1";
const char kDigestDomain[] =
    "aegisy-companion-activation-journal-authority-slot-digest/0.1\0";

bool safeGeneration(const QJsonValue &value, qint64 *result)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || std::floor(number) != number
            || number < 1 || number > static_cast<double>(kMaximumGeneration)) {
        return false;
    }
    *result = static_cast<qint64>(number);
    return true;
}

void appendSized(QByteArray *target, const QByteArray &value)
{
    const quint64 size = static_cast<quint64>(value.size());
    for (int shift = 56; shift >= 0; shift -= 8) {
        target->append(static_cast<char>((size >> shift) & 0xff));
    }
    target->append(value);
}

QString digest(qint64 generation, const QByteArray &payload)
{
    QByteArray input(kDigestDomain, static_cast<int>(sizeof(kDigestDomain)) - 1);
    appendSized(&input, QByteArray::number(generation));
    appendSized(&input, payload);
    return QString::fromLatin1(
        QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

struct Parsed {
    bool valid = false;
    qint64 generation = 0;
    QByteArray payload;
    QByteArray frame;
};

Parsed parse(const AuthoritySlotInput &slot)
{
    Parsed parsed;
    if (slot.state != AuthoritySlotReadState::Found) return parsed;
    qint64 generation = 0;
    QByteArray payload;
    if (!CompanionActivationAuthoritySlots::parseFrame(
            slot.frame, &generation, &payload)) {
        return parsed;
    }
    parsed.valid = true;
    parsed.generation = generation;
    parsed.payload = payload;
    parsed.frame = slot.frame;
    return parsed;
}

} // namespace

QByteArray CompanionActivationAuthoritySlots::frame(
    qint64 generation, const QByteArray &payload)
{
    if (generation < 1 || generation > kMaximumGeneration || payload.isEmpty()) {
        return {};
    }
    const QJsonObject object{
        {QStringLiteral("schema_version"), QString::fromLatin1(kFrameSchema)},
        {QStringLiteral("generation"), generation},
        {QStringLiteral("payload_base64"), QString::fromLatin1(payload.toBase64())},
        {QStringLiteral("digest"), digest(generation, payload)},
    };
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return bytes.size() <= kMaximumFrameBytes ? bytes : QByteArray();
}

bool CompanionActivationAuthoritySlots::parseFrame(
    const QByteArray &frameBytes, qint64 *generation, QByteArray *payload)
{
    if (!generation || !payload || frameBytes.isEmpty()
            || frameBytes.size() > kMaximumFrameBytes) {
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(frameBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const QJsonObject object = document.object();
    const QSet<QString> expected{
        QStringLiteral("schema_version"), QStringLiteral("generation"),
        QStringLiteral("payload_base64"), QStringLiteral("digest")};
    const QStringList keys = object.keys();
    if (QSet<QString>(keys.cbegin(), keys.cend()) != expected
            || object.value(QStringLiteral("schema_version")).toString()
                != QString::fromLatin1(kFrameSchema)
            || !object.value(QStringLiteral("payload_base64")).isString()
            || !object.value(QStringLiteral("digest")).isString()) {
        return false;
    }
    qint64 parsedGeneration = 0;
    if (!safeGeneration(object.value(QStringLiteral("generation")),
                        &parsedGeneration)) {
        return false;
    }
    const QByteArray encoded =
        object.value(QStringLiteral("payload_base64")).toString().toLatin1();
    const QByteArray decoded = QByteArray::fromBase64(
        encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.isEmpty() || decoded.toBase64() != encoded) return false;
    if (object.value(QStringLiteral("digest")).toString()
            != digest(parsedGeneration, decoded)) {
        return false;
    }
    *generation = parsedGeneration;
    *payload = decoded;
    return true;
}

AuthoritySlotSelection CompanionActivationAuthoritySlots::select(
    const AuthoritySlotInput &slotA, const AuthoritySlotInput &slotB,
    const AuthoritySlotInput &legacy)
{
    AuthoritySlotSelection selection;
    // 任一槽位后端不可用时不能推断：可能有一个更新的代号读不出来。
    if (slotA.state == AuthoritySlotReadState::Unavailable
            || slotB.state == AuthoritySlotReadState::Unavailable
            || legacy.state == AuthoritySlotReadState::Unavailable) {
        selection.state = AuthoritySlotSelectionState::Unavailable;
        selection.errorCode =
            QStringLiteral("activation-authority-slot-unavailable");
        return selection;
    }
    const Parsed parsedA = parse(slotA);
    const Parsed parsedB = parse(slotB);
    const bool corruptA = slotA.state == AuthoritySlotReadState::Found
        && !parsedA.valid;
    const bool corruptB = slotB.state == AuthoritySlotReadState::Found
        && !parsedB.valid;
    if (slotA.state == AuthoritySlotReadState::Invalid
            || slotB.state == AuthoritySlotReadState::Invalid) {
        selection.state = AuthoritySlotSelectionState::Invalid;
        selection.errorCode =
            QStringLiteral("activation-authority-slot-backend-invalid");
        return selection;
    }
    if (corruptA && corruptB) {
        // 两个槽位同时损坏无法恢复，但它绝不能退化成"没有事务"。
        selection.state = AuthoritySlotSelectionState::Invalid;
        selection.errorCode =
            QStringLiteral("activation-authority-slot-both-corrupt");
        return selection;
    }
    if (parsedA.valid && parsedB.valid && parsedA.generation == parsedB.generation
            && parsedA.payload != parsedB.payload) {
        selection.state = AuthoritySlotSelectionState::Invalid;
        selection.errorCode =
            QStringLiteral("activation-authority-slot-generation-conflict");
        return selection;
    }

    const Parsed *newest = nullptr;
    if (parsedA.valid && parsedB.valid) {
        newest = parsedA.generation >= parsedB.generation ? &parsedA : &parsedB;
    } else if (parsedA.valid) {
        newest = &parsedA;
    } else if (parsedB.valid) {
        newest = &parsedB;
    }

    if (!newest) {
        if (legacy.state == AuthoritySlotReadState::Invalid) {
            selection.state = AuthoritySlotSelectionState::Invalid;
            selection.errorCode =
                QStringLiteral("activation-authority-slot-legacy-invalid");
            return selection;
        }
        if (legacy.state == AuthoritySlotReadState::Found) {
            // 迁移：把单槽载荷作为代号 1 采纳，直到双槽发布确认后才移除它。
            selection.state = AuthoritySlotSelectionState::Found;
            selection.payload = legacy.frame;
            selection.generation = 1;
            selection.writeSlot = AuthoritySlotName::SlotA;
            selection.writeGeneration = 2;
            selection.legacyPending = true;
            return selection;
        }
        // 两个槽位都缺失且没有旧数据：这是真正的首次安装。
        selection.state = corruptA || corruptB
            ? AuthoritySlotSelectionState::Invalid
            : AuthoritySlotSelectionState::Missing;
        if (corruptA || corruptB) {
            selection.errorCode =
                QStringLiteral("activation-authority-slot-corrupt-without-peer");
        }
        selection.writeSlot = corruptA ? AuthoritySlotName::SlotA
            : (corruptB ? AuthoritySlotName::SlotB : AuthoritySlotName::SlotA);
        selection.writeGeneration = 1;
        return selection;
    }

    const bool newestIsA = newest == &parsedA;
    selection.state = AuthoritySlotSelectionState::Found;
    selection.payload = newest->payload;
    selection.generation = newest->generation;
    // 永远写入对端槽位，选中的代号因此始终保持完好。
    selection.writeSlot = newestIsA ? AuthoritySlotName::SlotB
                                    : AuthoritySlotName::SlotA;
    selection.writeGeneration = newest->generation + 1;
    if (selection.writeGeneration > kMaximumGeneration) {
        selection.state = AuthoritySlotSelectionState::Invalid;
        selection.errorCode =
            QStringLiteral("activation-authority-slot-generation-exhausted");
    }
    return selection;
}
