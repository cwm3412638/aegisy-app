#pragma once

#include "../../generated/cpp/aap_transport_types_generated.h"

#include <QByteArray>
#include <QString>

#include <memory>

namespace aegisy::aap::transport_runtime {

inline constexpr qsizetype kMaxTransportJsonBytes = 4 * 1024 * 1024;
inline constexpr qsizetype kMaxTransportJsonDepth = 128;
inline constexpr qsizetype kMaxTransportJsonNodes = 65'536;

using transport_generated::TransportJsonValue;

bool parseTransportJsonRaw(const QByteArray &raw,
                           TransportJsonValue *output,
                           QString *error = nullptr);

QByteArray canonicalTransportJson(const TransportJsonValue &value);

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
    bool validateRootRaw(const QByteArray &raw,
                         TransportJsonValue *output = nullptr,
                         QString *error = nullptr) const;

private:
    class Impl;

    explicit TransportSchemaRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace aegisy::aap::transport_runtime
