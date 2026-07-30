#include "aap_transport_runtime.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>

namespace {

using aegisy::aap::transport_generated::TransportJsonNumber;
using aegisy::aap::transport_generated::TransportJsonValue;
using aegisy::aap::transport_generated::TransportParseError;
using aegisy::aap::transport_generated::TransportParseErrorKind;
using aegisy::aap::transport_runtime::TransportIntegerConversion;
using aegisy::aap::transport_runtime::TransportProjectionError;
using aegisy::aap::transport_runtime::TransportSchemaRuntime;
using aegisy::aap::transport_runtime::isTransportJsonMathematicalInteger;
using aegisy::aap::transport_runtime::kMaxTransportJsonBytes;
using aegisy::aap::transport_runtime::parseTransportJsonRaw;
using aegisy::aap::transport_runtime::projectJsonSafeTransportValue;
using aegisy::aap::transport_runtime::transportJsonIntegerEqualsQint64;
using aegisy::aap::transport_runtime::transportJsonIntegerToQint64;

[[noreturn]] void fail(const QString &message)
{
    std::cerr << message.toStdString() << '\n';
    std::exit(1);
}

void require(bool condition, const QString &message)
{
    if (!condition) {
        fail(message);
    }
}

void expectParse(const QByteArray &raw, bool expected)
{
    TransportJsonValue value;
    QString error;
    const bool accepted = parseTransportJsonRaw(raw, &value, &error);
    if (accepted != expected) {
        fail(QStringLiteral("parse expectation failed: ") + error);
    }
}

void expectDetailedParseError(const QByteArray &raw,
                              TransportParseErrorKind kind,
                              qsizetype offset)
{
    TransportJsonValue value;
    TransportParseError error;
    const bool accepted =
        aegisy::aap::transport_runtime::parseTransportJsonRawDetailed(
            raw, &value, &error);
    if (accepted || error.kind != kind || error.offset != offset
        || error.message.isEmpty()) {
        fail(QStringLiteral("detailed parse error mismatch"));
    }
}

void expectRoot(const TransportSchemaRuntime &runtime,
                const QByteArray &raw,
                bool expected)
{
    QString error;
    const bool accepted = runtime.validateRootRaw(raw, nullptr, &error);
    if (accepted != expected) {
        fail(QStringLiteral("root expectation failed: ") + error);
    }
}

void expectDefinition(const TransportSchemaRuntime &runtime,
                      const QString &definition,
                      const QByteArray &raw,
                      bool expected)
{
    QString error;
    const bool accepted = runtime.validateDefinitionRaw(definition, raw, nullptr, &error);
    if (accepted != expected) {
        fail(QStringLiteral("definition expectation failed: ") + definition
             + QStringLiteral(": ") + error);
    }
}

QByteArray nestedArray(qsizetype depth)
{
    return QByteArray(depth, '[') + QByteArrayLiteral("null") + QByteArray(depth, ']');
}

TransportJsonNumber parseNumber(const QByteArray &raw)
{
    TransportJsonValue value;
    QString error;
    require(parseTransportJsonRaw(raw, &value, &error),
            QStringLiteral("number parse failed: ") + error);
    const auto *number = std::get_if<TransportJsonNumber>(&value.value);
    require(number != nullptr, QStringLiteral("parsed value is not a number"));
    return *number;
}

void expectIntegerConversion(const QByteArray &raw, qint64 expected)
{
    const TransportJsonNumber number = parseNumber(raw);
    qint64 actual = expected == 0 ? 1 : 0;
    require(isTransportJsonMathematicalInteger(number),
            QStringLiteral("mathematical integer was not recognized: ")
                + QString::fromLatin1(raw));
    require(transportJsonIntegerToQint64(number, &actual)
                == TransportIntegerConversion::Ok
            && actual == expected
            && transportJsonIntegerEqualsQint64(number, expected),
            QStringLiteral("integer conversion mismatch: ") + QString::fromLatin1(raw));
}

QJsonValue projectRaw(const QByteArray &raw)
{
    TransportJsonValue value;
    QString parseError;
    require(parseTransportJsonRaw(raw, &value, &parseError),
            QStringLiteral("projection input parse failed: ") + parseError);
    QJsonValue projected(QStringLiteral("unchanged"));
    TransportProjectionError projectionError = TransportProjectionError::InvalidValue;
    require(projectJsonSafeTransportValue(value, &projected, &projectionError)
                && projectionError == TransportProjectionError::None,
            QStringLiteral("safe projection failed: ") + QString::fromLatin1(raw));
    return projected;
}

void expectProjectionFailure(const QByteArray &raw,
                             TransportProjectionError expectedError)
{
    TransportJsonValue value;
    QString parseError;
    require(parseTransportJsonRaw(raw, &value, &parseError),
            QStringLiteral("projection input parse failed: ") + parseError);
    const QJsonValue sentinel(QStringLiteral("unchanged"));
    QJsonValue projected = sentinel;
    TransportProjectionError projectionError = TransportProjectionError::None;
    require(!projectJsonSafeTransportValue(value, &projected, &projectionError)
                && projectionError == expectedError && projected == sentinel,
            QStringLiteral("unsafe projection was not rejected atomically: ")
                + QString::fromLatin1(raw));
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() != 2) {
        std::cerr << "usage: aap_transport_runtime_test <aap.schema.json>\n";
        return 2;
    }
    QFile schemaFile(arguments.at(1));
    require(schemaFile.open(QIODevice::ReadOnly), QStringLiteral("cannot open transport schema"));
    QString error;
    std::unique_ptr<TransportSchemaRuntime> runtime =
        TransportSchemaRuntime::fromRawSchema(schemaFile.readAll(), &error);
    require(bool(runtime), QStringLiteral("cannot compile transport schema: ") + error);

    expectRoot(*runtime,
               QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"id\",\"result\":1e100000}"),
               true);
    expectRoot(*runtime,
               QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-1e0,\"message\":\"error\",\"data\":-1e-100000}}"),
               true);
    expectRoot(*runtime,
               QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":1.5,\"message\":\"error\"}}"),
               false);
    expectRoot(*runtime,
               QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":\"id\",\"result\":{\"key\":1,\"key\":2}}"),
               false);
    expectDefinition(*runtime, QStringLiteral("jsonRpcError"),
                     QByteArrayLiteral("{\"code\":123456789012345678901234567890,\"message\":\"error\"}"),
                     true);
    expectDefinition(*runtime, QStringLiteral("positiveTimelineAnchor"),
                     QByteArrayLiteral("{\"sequence\":0,\"event_id\":null}"), false);
    expectDefinition(*runtime, QStringLiteral("positiveTimelineAnchor"),
                     QByteArrayLiteral("{\"sequence\":1,\"event_id\":\"event:sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}"),
                     true);

    TransportJsonValue numberValue;
    require(parseTransportJsonRaw(QByteArrayLiteral("-100.000e-2"), &numberValue, &error), error);
    const auto *number = std::get_if<TransportJsonNumber>(&numberValue.value);
    require(number && number->lexical == QStringLiteral("-100.000e-2")
                && number->canonical == QStringLiteral("-1") && number->integer,
            QStringLiteral("number normalization failed"));
    require(aegisy::aap::transport_runtime::canonicalTransportJson(numberValue)
                == QByteArrayLiteral("-1"),
            QStringLiteral("canonical number serialization failed"));

    expectIntegerConversion(QByteArrayLiteral("-32148"), -32148);
    expectIntegerConversion(QByteArrayLiteral("-32148.0"), -32148);
    expectIntegerConversion(QByteArrayLiteral("-3.2148e4"), -32148);
    expectIntegerConversion(QByteArrayLiteral("9223372036854775807"),
                            std::numeric_limits<qint64>::max());
    expectIntegerConversion(QByteArrayLiteral("-9223372036854775808"),
                            std::numeric_limits<qint64>::min());
    expectIntegerConversion(QByteArrayLiteral("0e-100000"), 0);

    qint64 unchanged = 73;
    const TransportJsonNumber positiveOverflow =
        parseNumber(QByteArrayLiteral("9223372036854775808"));
    require(isTransportJsonMathematicalInteger(positiveOverflow)
                && transportJsonIntegerToQint64(positiveOverflow, &unchanged)
                    == TransportIntegerConversion::OutOfRange
                && unchanged == 73
                && !transportJsonIntegerEqualsQint64(
                    positiveOverflow, std::numeric_limits<qint64>::max()),
            QStringLiteral("positive qint64 overflow was not rejected exactly"));
    const TransportJsonNumber negativeOverflow =
        parseNumber(QByteArrayLiteral("-9223372036854775809"));
    require(transportJsonIntegerToQint64(negativeOverflow, &unchanged)
                == TransportIntegerConversion::OutOfRange
            && unchanged == 73,
            QStringLiteral("negative qint64 overflow was not rejected exactly"));
    const TransportJsonNumber hugeExponent = parseNumber(QByteArrayLiteral("1e100000"));
    require(isTransportJsonMathematicalInteger(hugeExponent)
                && transportJsonIntegerToQint64(hugeExponent, &unchanged)
                    == TransportIntegerConversion::OutOfRange
                && unchanged == 73,
            QStringLiteral("huge integer exponent was not bounded"));
    const TransportJsonNumber fraction = parseNumber(QByteArrayLiteral("1.5"));
    require(!isTransportJsonMathematicalInteger(fraction)
                && transportJsonIntegerToQint64(fraction, &unchanged)
                    == TransportIntegerConversion::NotInteger
                && unchanged == 73,
            QStringLiteral("fraction was not classified separately"));

    const QJsonValue safeMaximum = projectRaw(QByteArrayLiteral("9007199254740991"));
    const QJsonValue safeMinimum = projectRaw(QByteArrayLiteral("-9007199254740991"));
    require(safeMaximum.toVariant().toLongLong() == 9'007'199'254'740'991LL
                && safeMinimum.toVariant().toLongLong() == -9'007'199'254'740'991LL
                && projectRaw(QByteArrayLiteral("9.007199254740991e15")) == safeMaximum
                && projectRaw(QByteArrayLiteral("-9007199254740991.0")) == safeMinimum,
            QStringLiteral("JSON-safe projection boundaries were not preserved"));
    expectProjectionFailure(QByteArrayLiteral("9007199254740992"),
                            TransportProjectionError::NumberOutOfSafeRange);
    expectProjectionFailure(QByteArrayLiteral("-9007199254740992"),
                            TransportProjectionError::NumberOutOfSafeRange);
    expectProjectionFailure(QByteArrayLiteral("1e100000"),
                            TransportProjectionError::NumberOutOfSafeRange);
    expectProjectionFailure(QByteArrayLiteral("1.5"),
                            TransportProjectionError::NumberNotInteger);

    const QJsonObject nestedProjection = projectRaw(QByteArrayLiteral(
        "{\"array\":[null,true,\"text\",-9007199254740991],"
        "\"object\":{\"maximum\":9007199254740991}}"))
                                             .toObject();
    const QJsonArray nestedArrayProjection =
        nestedProjection.value(QStringLiteral("array")).toArray();
    require(nestedArrayProjection.size() == 4
                && nestedArrayProjection.at(0).isNull()
                && nestedArrayProjection.at(1).toBool()
                && nestedArrayProjection.at(2).toString() == QStringLiteral("text")
                && nestedArrayProjection.at(3).toVariant().toLongLong()
                    == -9'007'199'254'740'991LL
                && nestedProjection.value(QStringLiteral("object"))
                       .toObject()
                       .value(QStringLiteral("maximum"))
                       .toVariant()
                       .toLongLong()
                    == 9'007'199'254'740'991LL,
            QStringLiteral("nested JSON-safe projection lost a value"));

    TransportJsonValue keyOrder;
    require(parseTransportJsonRaw(
                QByteArrayLiteral("{\"\\ud800\\udc00\":1,\"\\ue000\":2}"),
                &keyOrder, &error), error);
    require(aegisy::aap::transport_runtime::canonicalTransportJson(keyOrder)
                == QByteArray("{\"\xee\x80\x80\":2,\"\xf0\x90\x80\x80\":1}"),
            QStringLiteral("canonical UTF-8 object-key ordering failed"));
    TransportJsonValue forgedNumber;
    forgedNumber.value = TransportJsonNumber{
        QStringLiteral("1"), QStringLiteral("2"), true};
    require(aegisy::aap::transport_runtime::canonicalTransportJson(forgedNumber).isEmpty(),
            QStringLiteral("forged Transport number was serialized"));
    const auto forgedNumberValue = std::get<TransportJsonNumber>(forgedNumber.value);
    require(!isTransportJsonMathematicalInteger(forgedNumberValue)
                && transportJsonIntegerToQint64(forgedNumberValue, &unchanged)
                    == TransportIntegerConversion::InvalidValue
                && unchanged == 73
                && !transportJsonIntegerEqualsQint64(forgedNumberValue, 1),
            QStringLiteral("forged Transport number was not rejected by integer helpers"));
    const TransportJsonNumber forgedIntegerFlag{
        QStringLiteral("1.5"), QStringLiteral("15e-1"), true};
    require(transportJsonIntegerToQint64(forgedIntegerFlag, &unchanged)
                == TransportIntegerConversion::InvalidValue
            && unchanged == 73,
            QStringLiteral("forged integer flag was not rejected"));
    const QJsonValue projectionSentinel(QStringLiteral("unchanged"));
    QJsonValue forgedProjection = projectionSentinel;
    TransportProjectionError projectionError = TransportProjectionError::None;
    require(!projectJsonSafeTransportValue(forgedNumber, &forgedProjection,
                                           &projectionError)
                && projectionError == TransportProjectionError::InvalidValue
                && forgedProjection == projectionSentinel,
            QStringLiteral("forged number projection changed its output"));
    require(!projectJsonSafeTransportValue(forgedNumber, nullptr, &projectionError)
                && projectionError == TransportProjectionError::InvalidValue,
            QStringLiteral("null projection output was accepted"));
    TransportJsonValue forgedString;
    forgedString.value = QString(QChar(0xd800));
    require(aegisy::aap::transport_runtime::canonicalTransportJson(forgedString).isEmpty(),
            QStringLiteral("forged surrogate string was serialized"));
    TransportJsonValue invalidNested;
    TransportJsonValue validNestedEntry;
    validNestedEntry.value = true;
    TransportJsonValue::Object invalidNestedObject;
    invalidNestedObject.insert(QStringLiteral("before"), validNestedEntry);
    invalidNestedObject.insert(QStringLiteral("invalid"), forgedString);
    invalidNested.value = invalidNestedObject;
    forgedProjection = projectionSentinel;
    projectionError = TransportProjectionError::None;
    require(!projectJsonSafeTransportValue(invalidNested, &forgedProjection,
                                           &projectionError)
                && projectionError == TransportProjectionError::InvalidValue
                && forgedProjection == projectionSentinel,
            QStringLiteral("nested invalid projection changed its output"));

    expectParse(QByteArrayLiteral("\xef\xbb\xbf{}"), false);
    expectDetailedParseError(QByteArrayLiteral("\xef\xbb\xbf{}"),
                             TransportParseErrorKind::Syntax, 0);
    expectDetailedParseError(QByteArray("\xff", 1),
                             TransportParseErrorKind::Utf8, 0);
    expectDetailedParseError(QByteArrayLiteral("01"),
                             TransportParseErrorKind::Syntax, 1);
    expectParse(QByteArray("\xed\xa0\x80", 3), false);
    expectParse(QByteArray("\"\xc0\xaf\"", 4), false);
    expectParse(QByteArray("\"\xf4\x90\x80\x80\"", 6), false);
    expectParse(QByteArrayLiteral("\"\\ud800\""), false);
    expectParse(QByteArrayLiteral("\"\\ud83d\\ude00\""), true);
    expectParse(QByteArrayLiteral("{\"a\":1,\"a\":1}"), false);
    TransportJsonValue embeddedBom;
    require(parseTransportJsonRaw(QByteArray("\"\xef\xbb\xbf\"", 5), &embeddedBom, &error), error);
    require(aegisy::aap::transport_runtime::canonicalTransportJson(embeddedBom)
                == QByteArray("\"\xef\xbb\xbf\"", 5),
            QStringLiteral("embedded U+FEFF was not preserved"));
    expectParse(nestedArray(128), true);
    expectParse(nestedArray(129), false);
    expectDetailedParseError(nestedArray(129),
                             TransportParseErrorKind::ImplementationLimit, 128);

    QByteArray exactFrame(kMaxTransportJsonBytes - 2, 'a');
    exactFrame.prepend('"');
    exactFrame.append('"');
    expectParse(exactFrame, true);
    exactFrame.insert(1, 'a');
    expectParse(exactFrame, false);
    expectDetailedParseError(exactFrame, TransportParseErrorKind::Frame,
                             exactFrame.size());

    QByteArray exactNodes = "[";
    exactNodes.reserve(5 * 65'535 + 1);
    for (qsizetype index = 0; index < 65'535; ++index) {
        if (index != 0) {
            exactNodes.append(',');
        }
        exactNodes.append("null");
    }
    exactNodes.append(']');
    expectParse(exactNodes, true);
    exactNodes.chop(1);
    exactNodes.append(",null]");
    expectParse(exactNodes, false);

    const QByteArray booleanSchema = QByteArrayLiteral(
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
        "\"$defs\":{\"allow\":true,\"deny\":false}}" );
    auto booleanRuntime = TransportSchemaRuntime::fromRawSchema(booleanSchema, &error);
    require(bool(booleanRuntime), error);
    expectDefinition(*booleanRuntime, QStringLiteral("allow"), QByteArrayLiteral("1.5"), true);
    expectDefinition(*booleanRuntime, QStringLiteral("deny"), QByteArrayLiteral("null"), false);

    const QByteArray keywordSchema = QByteArrayLiteral(
        "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\",\"$defs\":{"
        "\"boundedObject\":{\"type\":\"object\",\"minProperties\":1,\"maxProperties\":1,"
        "\"required\":[\"a\"],\"properties\":{\"a\":{\"type\":\"integer\","
        "\"minimum\":1,\"maximum\":2}},\"propertyNames\":{\"pattern\":\"^a$\"},"
        "\"additionalProperties\":false},"
        "\"boundedArray\":{\"type\":\"array\",\"minItems\":1,\"maxItems\":2,"
        "\"uniqueItems\":true,\"items\":{\"type\":\"integer\"}},"
        "\"conditional\":{\"if\":{\"type\":\"string\"},"
        "\"then\":{\"const\":\"yes\"},\"else\":{\"const\":1}}}}" );
    auto keywordRuntime = TransportSchemaRuntime::fromRawSchema(keywordSchema, &error);
    require(bool(keywordRuntime), error);
    expectDefinition(*keywordRuntime, QStringLiteral("boundedObject"),
                     QByteArrayLiteral("{\"a\":1}"), true);
    expectDefinition(*keywordRuntime, QStringLiteral("boundedObject"),
                     QByteArrayLiteral("{}"), false);
    expectDefinition(*keywordRuntime, QStringLiteral("boundedObject"),
                     QByteArrayLiteral("{\"a\":1,\"b\":1}"), false);
    expectDefinition(*keywordRuntime, QStringLiteral("boundedArray"),
                     QByteArrayLiteral("[1,2]"), true);
    expectDefinition(*keywordRuntime, QStringLiteral("boundedArray"),
                     QByteArrayLiteral("[1,1.0]"), false);
    expectDefinition(*keywordRuntime, QStringLiteral("conditional"),
                     QByteArrayLiteral("\"yes\""), true);
    expectDefinition(*keywordRuntime, QStringLiteral("conditional"),
                     QByteArrayLiteral("1e0"), true);
    expectDefinition(*keywordRuntime, QStringLiteral("conditional"),
                     QByteArrayLiteral("2"), false);

    std::cout << "aap transport C++ runtime self-test passed\n";
    return 0;
}
