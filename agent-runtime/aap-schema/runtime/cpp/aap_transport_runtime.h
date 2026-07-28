#pragma once

#include "../../generated/cpp/aap_transport_types_generated.h"

#include <QByteArray>
#include <QJsonValue>
#include <QString>

#include <memory>

namespace aegisy::aap::transport_runtime {

inline constexpr qsizetype kMaxTransportJsonBytes = 4 * 1024 * 1024;
inline constexpr qsizetype kMaxTransportJsonDepth = 128;
inline constexpr qsizetype kMaxTransportJsonNodes = 65'536;
inline constexpr qint64 kMaxTransportJsonSafeInteger = 9'007'199'254'740'991LL;
inline constexpr qint64 kMinTransportJsonSafeInteger = -kMaxTransportJsonSafeInteger;

using transport_generated::TransportJsonValue;
using transport_generated::TransportJsonNumber;
using transport_generated::TransportParseError;

enum class TransportIntegerConversion {
    Ok,
    NotInteger,
    OutOfRange,
    InvalidValue,
};

enum class TransportProjectionError {
    None,
    InvalidValue,
    NumberNotInteger,
    NumberOutOfSafeRange,
};

bool parseTransportJsonRaw(const QByteArray &raw,
                           TransportJsonValue *output,
                           QString *error = nullptr);
bool parseTransportJsonRawDetailed(const QByteArray &raw,
                                   TransportJsonValue *output,
                                   TransportParseError *error = nullptr);

QByteArray canonicalTransportJson(const TransportJsonValue &value);

bool isTransportJsonMathematicalInteger(const TransportJsonNumber &number);
bool transportJsonIntegerEqualsQint64(const TransportJsonNumber &number,
                                      qint64 expected);
TransportIntegerConversion transportJsonIntegerToQint64(
    const TransportJsonNumber &number,
    qint64 *output);
// Projects validated generic payloads only. JSON-RPC error.code requires exact
// TransportJsonNumber handling and must not pass through this JSON-safe boundary.
bool projectJsonSafeTransportValue(
    const TransportJsonValue &value,
    QJsonValue *output,
    TransportProjectionError *error = nullptr);

class TransportSchemaRuntime final {
public:
    static std::unique_ptr<TransportSchemaRuntime> fromRawSchema(
        const QByteArray &schemaRaw,
        QString *error = nullptr);

    ~TransportSchemaRuntime();

    TransportSchemaRuntime(const TransportSchemaRuntime &) = delete;
    TransportSchemaRuntime &operator=(const TransportSchemaRuntime &) = delete;
    TransportSchemaRuntime(TransportSchemaRuntime &&) noexcept;
    TransportSchemaRuntime &operator=(TransportSchemaRuntime &&) noexcept;

    bool validateDefinitionRaw(const QString &definition,
                               const QByteArray &raw,
                               TransportJsonValue *output = nullptr,
                               QString *error = nullptr) const;
    bool validateDefinition(const QString &definition,
                            const TransportJsonValue &value,
                            QString *error = nullptr) const;
    bool validateRootRaw(const QByteArray &raw,
                         TransportJsonValue *output = nullptr,
                         QString *error = nullptr) const;
    bool validateRoot(const TransportJsonValue &value,
                      QString *error = nullptr) const;

private:
    class Impl;

    explicit TransportSchemaRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace aegisy::aap::transport_runtime
