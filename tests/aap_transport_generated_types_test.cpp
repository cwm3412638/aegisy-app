#include "aap_transport_runtime.h"
#include "aap_transport_types_generated.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using aegisy::aap::transport_generated::TransportJsonNumber;
using aegisy::aap::transport_generated::TransportJsonValue;
using aegisy::aap::transport_generated::TransportDecodeError;
using aegisy::aap::transport_generated::TransportDispatchError;
using aegisy::aap::transport_generated::TransportDispatchErrorKind;
using aegisy::aap::transport_generated::TransportMethodKind;
using aegisy::aap::transport_generated::TransportParseErrorKind;
using aegisy::aap::transport_generated::TransportPendingRequest;
using aegisy::aap::transport_generated::TransportRequestOrNotification;
using aegisy::aap::transport_generated::TransportRequestOrNotificationKind;
using aegisy::aap::transport_generated::TransportResponse;
using aegisy::aap::transport_generated::TransportResponseKind;
using aegisy::aap::transport_generated::TransportSchemaError;
using aegisy::aap::transport_runtime::TransportSchemaRuntime;
using JsonArray = TransportJsonValue::Array;
using JsonObject = TransportJsonValue::Object;

constexpr auto kSchemaId = "https://aegisy.cc/schemas/aap/stable/v0.1/aap.schema.json";
constexpr auto kFixtureSchema = "aap-transport-fixture-catalog/0.1";
constexpr auto kCorpusSchema = "aap-transport-validation-corpus/0.1";
constexpr auto kParserProfile =
    "exact-json-number-schema-bounded-integer-unicode-scalar-no-duplicate-keys/0.1";

int fail(const QString &message, int code)
{
    std::cerr << message.toStdString() << '\n';
    return code;
}

bool hasExactKeys(const JsonObject &object, std::initializer_list<QString> keys)
{
    if (object.size() != qsizetype(keys.size())) {
        return false;
    }
    for (const QString &key : keys) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

bool stringField(const JsonObject &object, const QString &key, QString *output)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd()) {
        return false;
    }
    const auto *value = std::get_if<QString>(&iterator.value().value);
    if (!value) {
        return false;
    }
    *output = *value;
    return true;
}

bool boolField(const JsonObject &object, const QString &key, bool *output)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd()) {
        return false;
    }
    const auto *value = std::get_if<bool>(&iterator.value().value);
    if (!value) {
        return false;
    }
    *output = *value;
    return true;
}

const JsonArray *arrayField(const JsonObject &object, const QString &key)
{
    const auto iterator = object.constFind(key);
    return iterator == object.constEnd()
        ? nullptr
        : std::get_if<JsonArray>(&iterator.value().value);
}

bool integerField(const JsonObject &object, const QString &key, qint64 *output)
{
    const auto iterator = object.constFind(key);
    if (iterator == object.constEnd()) {
        return false;
    }
    const auto *number = std::get_if<TransportJsonNumber>(&iterator.value().value);
    if (!number || !number->integer) {
        return false;
    }
    bool valid = false;
    const qint64 parsed = number->canonical.toLongLong(&valid);
    if (!valid) {
        return false;
    }
    *output = parsed;
    return true;
}

bool readMetadata(const QString &path, JsonObject *output, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QStringLiteral("metadata is unreadable");
        }
        return false;
    }
    TransportJsonValue value;
    if (!aegisy::aap::transport_runtime::parseTransportJsonRaw(file.readAll(), &value, error)) {
        return false;
    }
    const auto *object = std::get_if<JsonObject>(&value.value);
    if (!object) {
        if (error) {
            *error = QStringLiteral("metadata must be an object");
        }
        return false;
    }
    *output = *object;
    return true;
}

std::unique_ptr<TransportSchemaRuntime> probeRuntime(const QByteArray &schema)
{
    QString error;
    auto runtime = TransportSchemaRuntime::fromRawSchema(schema, &error);
    if (!runtime) {
        std::cerr << "cannot compile corpus probe: " << error.toStdString() << '\n';
    }
    return runtime;
}

int emitFixtureIdentity(const QString &path)
{
    JsonObject catalog;
    QString error;
    if (!readMetadata(path, &catalog, &error)) {
        return fail(QStringLiteral("fixture metadata is invalid: ") + error, 3);
    }
    if (!hasExactKeys(catalog, {QStringLiteral("schema_version"),
                                QStringLiteral("schema_id"),
                                QStringLiteral("canonical_bytes"),
                                QStringLiteral("canonical_sha256"),
                                QStringLiteral("entries")})) {
        return fail(QStringLiteral("fixture fields are invalid"), 4);
    }
    QString schemaVersion;
    QString schemaId;
    QString expectedHash;
    qint64 expectedBytes = 0;
    const JsonArray *entries = arrayField(catalog, QStringLiteral("entries"));
    if (!stringField(catalog, QStringLiteral("schema_version"), &schemaVersion)
        || schemaVersion != QString::fromLatin1(kFixtureSchema)
        || !stringField(catalog, QStringLiteral("schema_id"), &schemaId)
        || schemaId != QString::fromLatin1(kSchemaId)
        || !stringField(catalog, QStringLiteral("canonical_sha256"), &expectedHash)
        || !integerField(catalog, QStringLiteral("canonical_bytes"), &expectedBytes)
        || expectedBytes <= 0 || !entries || entries->size() != 101) {
        return fail(QStringLiteral("fixture identity or coverage is invalid"), 5);
    }

    QByteArray canonical = schemaVersion.toUtf8() + '\n' + schemaId.toUtf8() + '\n';
    QString previous;
    for (qsizetype index = 0; index < entries->size(); ++index) {
        const auto *entry = std::get_if<JsonObject>(&entries->at(index).value);
        QString definition;
        QString raw;
        if (!entry || !hasExactKeys(*entry, {QStringLiteral("definition"),
                                             QStringLiteral("value_json")})
            || !stringField(*entry, QStringLiteral("definition"), &definition)
            || definition <= previous
            || !stringField(*entry, QStringLiteral("value_json"), &raw)) {
            return fail(QStringLiteral("fixture entry metadata is invalid"), 6);
        }
        previous = definition;
        TransportJsonValue value;
        if (!aegisy::aap::transport_generated::validateTransportDefinitionRaw(
                definition, raw.toUtf8(), &value, &error)) {
            return fail(QStringLiteral("generated C++ fixture rejection: ") + definition
                            + QStringLiteral(": ") + error,
                        7);
        }
        const QByteArray valueJson =
            aegisy::aap::transport_generated::canonicalTransportJson(value);
        canonical += definition.toUtf8() + '\t' + QByteArray::number(valueJson.size()) + '\n';
        canonical += valueJson + '\n';
    }
    const QByteArray digest =
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex();
    if (canonical.size() != expectedBytes || QString::fromLatin1(digest) != expectedHash) {
        return fail(QStringLiteral("generated C++ fixture identity differs from golden"), 8);
    }
    std::cout << canonical.size() << ' ' << digest.constData() << '\n';
    return 0;
}

int emitCorpusIdentity(const QString &path)
{
    JsonObject corpus;
    QString error;
    if (!readMetadata(path, &corpus, &error)) {
        return fail(QStringLiteral("corpus metadata is invalid: ") + error, 10);
    }
    if (!hasExactKeys(corpus, {QStringLiteral("schema_version"),
                               QStringLiteral("schema_id"),
                               QStringLiteral("parser_profile"),
                               QStringLiteral("expected_decisions_sha256"),
                               QStringLiteral("cases")})) {
        return fail(QStringLiteral("corpus fields are invalid"), 11);
    }
    QString schemaVersion;
    QString schemaId;
    QString parserProfile;
    QString expectedHash;
    const JsonArray *cases = arrayField(corpus, QStringLiteral("cases"));
    static const QRegularExpression hashPattern(QStringLiteral("^[0-9a-f]{64}$"));
    static const QRegularExpression namePattern(
        QStringLiteral("^[a-z0-9]+(?:-[a-z0-9]+)*$"));
    if (!stringField(corpus, QStringLiteral("schema_version"), &schemaVersion)
        || schemaVersion != QString::fromLatin1(kCorpusSchema)
        || !stringField(corpus, QStringLiteral("schema_id"), &schemaId)
        || schemaId != QString::fromLatin1(kSchemaId)
        || !stringField(corpus, QStringLiteral("parser_profile"), &parserProfile)
        || parserProfile != QString::fromLatin1(kParserProfile)
        || !stringField(corpus, QStringLiteral("expected_decisions_sha256"), &expectedHash)
        || !hashPattern.match(expectedHash).hasMatch() || !cases || cases->isEmpty()
        || cases->size() > 128) {
        return fail(QStringLiteral("corpus identity or bounds are invalid"), 12);
    }

    const auto anyOf = probeRuntime(QByteArrayLiteral(
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"$defs\":{},\"anyOf\":[{\"type\":\"string\",\"minLength\":1},"
        "{\"type\":\"integer\",\"minimum\":1}]}"));
    const auto constNumber = probeRuntime(QByteArrayLiteral(
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"$defs\":{},\"const\":1}"));
    const auto uniqueItems = probeRuntime(QByteArrayLiteral(
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"$defs\":{},\"type\":\"array\",\"uniqueItems\":true,\"items\":true}"));
    if (!anyOf || !constNumber || !uniqueItems) {
        return 13;
    }

    QByteArray decisions = schemaVersion.toUtf8() + '\t' + parserProfile.toUtf8() + '\n';
    QSet<QString> names;
    for (qsizetype index = 0; index < cases->size(); ++index) {
        const auto *entry = std::get_if<JsonObject>(&cases->at(index).value);
        QString name;
        QString target;
        QString raw;
        bool expected = false;
        if (!entry || !hasExactKeys(*entry, {QStringLiteral("name"),
                                             QStringLiteral("target"),
                                             QStringLiteral("valid"),
                                             QStringLiteral("value_json")})
            || !stringField(*entry, QStringLiteral("name"), &name) || name.size() > 96
            || !namePattern.match(name).hasMatch() || names.contains(name)
            || !stringField(*entry, QStringLiteral("target"), &target)
            || !boolField(*entry, QStringLiteral("valid"), &expected)
            || !stringField(*entry, QStringLiteral("value_json"), &raw)) {
            return fail(QStringLiteral("corpus case metadata is invalid"), 14);
        }
        names.insert(name);
        bool accepted = false;
        if (target == QStringLiteral("$root")) {
            accepted = aegisy::aap::transport_generated::validateTransportMessageRaw(
                raw.toUtf8(), nullptr, &error);
        } else if (target == QStringLiteral("$probe:anyOf")) {
            accepted = anyOf->validateRootRaw(raw.toUtf8(), nullptr, &error);
        } else if (target == QStringLiteral("$probe:constNumber")) {
            accepted = constNumber->validateRootRaw(raw.toUtf8(), nullptr, &error);
        } else if (target == QStringLiteral("$probe:uniqueItems")) {
            accepted = uniqueItems->validateRootRaw(raw.toUtf8(), nullptr, &error);
        } else {
            accepted = aegisy::aap::transport_generated::validateTransportDefinitionRaw(
                target, raw.toUtf8(), nullptr, &error);
        }
        if (accepted != expected) {
            return fail(QStringLiteral("generated C++ corpus decision differs: ") + name, 15);
        }
        decisions += name.toUtf8() + '\t' + target.toUtf8() + '\t'
            + (accepted ? QByteArrayLiteral("accept\n") : QByteArrayLiteral("reject\n"));
    }
    const QByteArray digest =
        QCryptographicHash::hash(decisions, QCryptographicHash::Sha256).toHex();
    if (QString::fromLatin1(digest) != expectedHash) {
        return fail(QStringLiteral("generated C++ corpus identity differs from golden"), 16);
    }
    std::cout << cases->size() << ' ' << digest.constData() << '\n';
    return 0;
}

int verifyGeneratedDispatch()
{
    using namespace aegisy::aap::transport_generated;
    const auto &methods = transportMethods();
    const auto &typedErrors = transportTypedErrors();
    if (methods.size() != 14 || typedErrors.size() != 6
        || !std::is_sorted(methods.cbegin(), methods.cend(),
            [](const auto &left, const auto &right) { return left.method < right.method; })
        || !std::is_sorted(typedErrors.cbegin(), typedErrors.cend(),
            [](const auto &left, const auto &right) {
                return std::pair<QString, QString>{left.method, left.schema_version}
                    < std::pair<QString, QString>{right.method, right.schema_version};
            })) {
        return fail(QStringLiteral("generated transport metadata is incomplete or unsorted"), 30);
    }
    const auto *initialize = transportMethodMetadata(QStringLiteral("initialize"));
    const auto *event = transportMethodMetadata(QStringLiteral("event"));
    const auto *retention = transportTypedErrorMetadata(
        QStringLiteral("timeline/sync"), QStringLiteral("timeline-retention-gap/0.1"));
    if (!initialize || initialize->kind != TransportMethodKind::Request
        || initialize->success_response_definition
            != QStringLiteral("initializeSuccessResponse")
        || !event || event->kind != TransportMethodKind::Notification
        || !retention
        || retention->response_definition
            != QStringLiteral("timelineSyncRetentionGapErrorResponse")
        || transportMethodMetadata(QStringLiteral("future/request"))
        || transportTypedErrorMetadata(QStringLiteral("initialize"),
                                       QStringLiteral("timeline-retention-gap/0.1"))) {
        return fail(QStringLiteral("generated transport metadata lookup is invalid"), 31);
    }

    TransportRequestOrNotification requestOrNotification;
    TransportDispatchError dispatchError;
    if (decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("01"), &requestOrNotification, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::Parse
        || dispatchError.parse.kind != TransportParseErrorKind::Syntax
        || dispatchError.parse.offset != 1) {
        return fail(QStringLiteral("dispatch did not preserve parse kind and offset"), 32);
    }
    if (decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\"}"),
            &requestOrNotification, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidEnvelope) {
        return fail(QStringLiteral("invalid generic envelope was misclassified"), 33);
    }
    if (!decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"future-1\",\"method\":\"future/request\",\"params\":{}}"),
            &requestOrNotification, &dispatchError)
        || requestOrNotification.kind != TransportRequestOrNotificationKind::UnknownRequest
        || requestOrNotification.metadata) {
        return fail(QStringLiteral("unknown request lost generic compatibility"), 34);
    }
    if (!decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"method\":\"future/notification\",\"params\":{}}"),
            &requestOrNotification, &dispatchError)
        || requestOrNotification.kind
            != TransportRequestOrNotificationKind::UnknownNotification) {
        return fail(QStringLiteral("unknown notification lost generic compatibility"), 35);
    }
    if (decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"known\",\"method\":\"runtime/heartbeat\",\"params\":{}}"),
            &requestOrNotification, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidKnownMessage) {
        return fail(QStringLiteral("known malformed request used generic fallback"), 36);
    }

    TransportDecodeError decodeError;
    TransportJsonValue value;
    if (decodeTransportDefinitionRaw(QStringLiteral("unknownDefinition"),
                                     QByteArrayLiteral("null"), &value, &decodeError)
        || decodeError.kind != TransportDecodeError::Kind::Schema
        || decodeError.schema != TransportSchemaError::UnknownDefinition) {
        return fail(QStringLiteral("unknown definition error was not preserved"), 37);
    }
    if (decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"method\":\"initialized\",\"params\":{},\"id\":\"wrong-kind\"}"),
            &requestOrNotification, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidEnvelope) {
        return fail(QStringLiteral("known method envelope kind was not enforced"), 38);
    }
    if (decodeTransportRequestOrNotificationRaw(
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"known\",\"method\":\"runtime/heartbeat\",\"params\":{}}"),
            nullptr, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::ValidatorUnavailable) {
        return fail(QStringLiteral("local dispatch output fault was not classified locally"), 39);
    }

    const auto pending = [](const QString &id, const QString &method,
                            std::optional<QString> identity = std::nullopt) {
        return std::optional<TransportPendingRequest>(
            TransportPendingRequest{id, method, std::move(identity)});
    };
    TransportResponse response;
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("heartbeat-1"), QStringLiteral("runtime/heartbeat")),
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"heartbeat-1\",\"result\":{\"schema_version\":\"runtime-heartbeat/0.1\",\"nonce\":\"nonce-1\",\"state\":\"alive\"}}"),
            &response, &dispatchError)
        || response.kind != TransportResponseKind::KnownSuccess
        || !response.method_metadata
        || response.method_metadata->method != QStringLiteral("runtime/heartbeat")) {
        return fail(QStringLiteral("known success response dispatch failed"), 40);
    }
    if (decodeTransportResponseRaw(
            pending(QStringLiteral("wrong-success"), QStringLiteral("initialize")),
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"wrong-success\",\"result\":{\"schema_version\":\"runtime-heartbeat/0.1\",\"nonce\":\"nonce-1\",\"state\":\"alive\"}}"),
            &response, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidKnownMessage) {
        return fail(QStringLiteral("known success wrapper used generic fallback"), 41);
    }
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("future-result"), QStringLiteral("future/request")),
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"future-result\",\"result\":1e10000}"),
            &response, &dispatchError)
        || response.kind != TransportResponseKind::UnknownMethod
        || canonicalTransportJson(response.message.value)
            != QByteArrayLiteral("{\"id\":\"future-result\",\"jsonrpc\":\"2.0\",\"result\":1e10000}")) {
        return fail(QStringLiteral("generic arbitrary-precision result was narrowed"), 42);
    }
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("future-error"), QStringLiteral("initialize")),
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"future-error\",\"error\":{\"code\":123456789012345678901234567890,\"message\":\"future\",\"data\":{\"schema_version\":\"future-error/0.1\"}}}"),
            &response, &dispatchError)
        || response.kind != TransportResponseKind::GenericError) {
        return fail(QStringLiteral("generic arbitrary-precision error was rejected"), 43);
    }
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("future-error"), QStringLiteral("initialize")),
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,\"message\":\"future\"}}"),
            &response, &dispatchError)
        || response.kind != TransportResponseKind::Unmatched) {
        return fail(QStringLiteral("generic null response ID did not remain unmatched"), 51);
    }

    const QByteArray retentionGap = QByteArrayLiteral(
        "{\"jsonrpc\":\"2.0\",\"id\":\"timeline-gap-1\",\"error\":{\"code\":-32148,\"message\":\"requested Timeline history is no longer retained\",\"data\":{\"schema_version\":\"timeline-retention-gap/0.1\",\"reason\":\"requested-anchor-not-retained\",\"session_id\":\"session-1\",\"requested_after\":{\"sequence\":0,\"event_id\":null},\"requested_watermark\":null,\"retained_floor\":{\"sequence\":2,\"event_id\":\"event:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"},\"head\":{\"sequence\":3,\"event_id\":\"event:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},\"snapshot_required\":true,\"snapshot_available\":true,\"snapshot_capability\":\"timeline.snapshot.current\",\"snapshot_method\":\"timeline/snapshot\",\"event_history_complete\":false,\"replay_from_floor_allowed\":false}}}");
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("timeline-gap-1"), QStringLiteral("timeline/sync")),
            retentionGap, &response, &dispatchError)
        || response.kind != TransportResponseKind::KnownTypedError
        || !response.typed_error_metadata
        || response.typed_error_metadata->method != QStringLiteral("timeline/sync")) {
        return fail(QStringLiteral("known retention-gap dispatch failed"), 44);
    }
    QByteArray wrongGapId = retentionGap;
    wrongGapId.replace("timeline-gap-1", "timeline-gap-2");
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("timeline-gap-1"), QStringLiteral("timeline/sync")),
            wrongGapId, &response, &dispatchError)
        || response.kind != TransportResponseKind::Unmatched) {
        return fail(QStringLiteral("wrong response ID did not remain unmatched"), 45);
    }
    QByteArray nullGapId = retentionGap;
    nullGapId.replace("\"timeline-gap-1\"", "null");
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("timeline-gap-1"), QStringLiteral("timeline/sync")),
            nullGapId, &response, &dispatchError)
        || response.kind != TransportResponseKind::Unmatched) {
        return fail(QStringLiteral("known typed error with null ID was not unmatched"), 46);
    }
    TransportMessage parsedGap;
    TransportDecodeError parsedGapError;
    if (!parseTransportMessageRaw(retentionGap, &parsedGap, &parsedGapError)
        || !decodeTransportResponse(
            pending(QStringLiteral("timeline-gap-1"), QStringLiteral("timeline/sync")),
            parsedGap, &response, &dispatchError)
        || response.kind != TransportResponseKind::KnownTypedError) {
        return fail(QStringLiteral("parsed response dispatch failed"), 53);
    }
    if (decodeTransportResponseRaw(
            pending(QStringLiteral("timeline-gap-1"), QStringLiteral("initialize")),
            retentionGap, &response, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidKnownMessage) {
        return fail(QStringLiteral("known typed error crossed pending methods"), 47);
    }
    if (decodeTransportResponseRaw(
            pending(QStringLiteral("timeline-gap-1"), QStringLiteral("timeline/sync")),
            QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"other-id\",\"error\":{\"code\":-32148,\"message\":\"gap\",\"data\":{\"schema_version\":\"timeline-retention-gap/0.1\"}}}"),
            &response, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidKnownMessage) {
        return fail(QStringLiteral("unmatched known typed error escaped validation"), 52);
    }

    const QString subscriptionIdentity = QStringLiteral(
        "timeline-subscription-request:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const QByteArray subscriptionFailure = QByteArrayLiteral(
        "{\"jsonrpc\":\"2.0\",\"id\":\"subscribe-1\",\"error\":{\"code\":-32150,\"message\":\"subscription failed\",\"data\":{\"schema_version\":\"timeline-subscription-failure/0.1\",\"connection_generation\":1,\"session_id\":\"session-1\",\"subscription_id\":\"subscription-1\",\"state\":\"failed\",\"stage\":\"subscribe\",\"cursor\":{\"sequence\":0,\"event_id\":null},\"watermark\":null,\"request_identity\":\"timeline-subscription-request:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"reason\":\"transport\",\"retryable\":true,\"cleanup_required\":true}}}");
    if (!decodeTransportResponseRaw(
            pending(QStringLiteral("subscribe-1"), QStringLiteral("timeline/subscribe"),
                    subscriptionIdentity),
            subscriptionFailure, &response, &dispatchError)
        || response.kind != TransportResponseKind::KnownTypedError) {
        return fail(QStringLiteral("subscription typed-error correlation failed"), 48);
    }
    if (decodeTransportResponseRaw(
            pending(QStringLiteral("subscribe-1"),
                    QStringLiteral("timeline/subscription-activate"), subscriptionIdentity),
            subscriptionFailure, &response, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidKnownMessage) {
        return fail(QStringLiteral("subscription stage was not method-bound"), 49);
    }
    if (decodeTransportResponseRaw(
            pending(QStringLiteral("subscribe-1"), QStringLiteral("timeline/subscribe"),
                    QStringLiteral("timeline-subscription-request:sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")),
            subscriptionFailure, &response, &dispatchError)
        || dispatchError.kind != TransportDispatchErrorKind::InvalidKnownMessage) {
        return fail(QStringLiteral("subscription request identity was not exact"), 50);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const int dispatchResult = verifyGeneratedDispatch();
    if (dispatchResult != 0) return dispatchResult;
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() == 2) {
        return emitFixtureIdentity(arguments.at(1));
    }
    if (arguments.size() == 3 && arguments.at(1) == QStringLiteral("--corpus")) {
        return emitCorpusIdentity(arguments.at(2));
    }
    std::cerr << "usage: AegisyAapTransportGeneratedTypesTest [--corpus] <path>\n";
    return 2;
}
