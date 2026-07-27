#include "aap_transport_runtime.h"
#include "aap_transport_types_generated.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QRegularExpression>
#include <QSet>

#include <iostream>
#include <memory>

namespace {

using aegisy::aap::transport_generated::TransportJsonNumber;
using aegisy::aap::transport_generated::TransportJsonValue;
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
        || expectedBytes <= 0 || !entries || entries->size() != 99) {
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

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
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
