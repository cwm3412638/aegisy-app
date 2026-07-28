#include "aap_transport_runtime.h"

#include <QFile>

#include <cstdlib>
#include <iostream>
#include <memory>

namespace {

using aegisy::aap::transport_generated::TransportJsonNumber;
using aegisy::aap::transport_generated::TransportJsonValue;
using aegisy::aap::transport_generated::TransportParseError;
using aegisy::aap::transport_generated::TransportParseErrorKind;
using aegisy::aap::transport_runtime::TransportSchemaRuntime;
using aegisy::aap::transport_runtime::kMaxTransportJsonBytes;
using aegisy::aap::transport_runtime::parseTransportJsonRaw;

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

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::cerr << "usage: aap_transport_runtime_test <aap.schema.json>\n";
        return 2;
    }
    QFile schemaFile(QString::fromLocal8Bit(argv[1]));
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
    TransportJsonValue forgedString;
    forgedString.value = QString(QChar(0xd800));
    require(aegisy::aap::transport_runtime::canonicalTransportJson(forgedString).isEmpty(),
            QStringLiteral("forged surrogate string was serialized"));

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
