#include "aap_transport_runtime.h"

#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>

namespace aegisy::aap::transport_runtime {
namespace {

using transport_generated::TransportJsonNumber;

using JsonArray = TransportJsonValue::Array;
using JsonObject = TransportJsonValue::Object;

struct SignedDecimal {
    bool negative = false;
    QByteArray digits = "0";
};

struct NumberParts {
    bool negative = false;
    QByteArray coefficient = "0";
    SignedDecimal scale;
};

void setError(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
}

char byteAt(QByteArrayView bytes, qsizetype index)
{
    return index >= 0 && index < bytes.size() ? bytes.at(index) : '\0';
}

QByteArray unsignedDecimal(QByteArray digits)
{
    qsizetype first = 0;
    while (first < digits.size() && digits.at(first) == '0') {
        ++first;
    }
    if (first == digits.size()) {
        return QByteArrayLiteral("0");
    }
    return digits.mid(first);
}

SignedDecimal signedDecimal(bool negative, QByteArray digits)
{
    SignedDecimal result;
    result.digits = unsignedDecimal(std::move(digits));
    result.negative = negative && result.digits != QByteArrayLiteral("0");
    return result;
}

int compareUnsignedDecimal(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size()) {
        return left.size() < right.size() ? -1 : 1;
    }
    const int comparison = left.compare(right);
    return comparison < 0 ? -1 : comparison > 0 ? 1 : 0;
}

QByteArray addUnsignedDecimal(const QByteArray &left, const QByteArray &right)
{
    QByteArray reversed;
    reversed.reserve(std::max(left.size(), right.size()) + 1);
    qsizetype leftIndex = left.size() - 1;
    qsizetype rightIndex = right.size() - 1;
    int carry = 0;
    while (leftIndex >= 0 || rightIndex >= 0 || carry != 0) {
        const int sum = (leftIndex >= 0 ? left.at(leftIndex--) - '0' : 0)
            + (rightIndex >= 0 ? right.at(rightIndex--) - '0' : 0) + carry;
        reversed.append(char('0' + (sum % 10)));
        carry = sum / 10;
    }
    std::reverse(reversed.begin(), reversed.end());
    return unsignedDecimal(std::move(reversed));
}

QByteArray subtractUnsignedDecimal(const QByteArray &left, const QByteArray &right)
{
    QByteArray reversed;
    reversed.reserve(left.size());
    qsizetype leftIndex = left.size() - 1;
    qsizetype rightIndex = right.size() - 1;
    int borrow = 0;
    while (leftIndex >= 0) {
        int digit = left.at(leftIndex--) - '0' - borrow
            - (rightIndex >= 0 ? right.at(rightIndex--) - '0' : 0);
        borrow = digit < 0 ? 1 : 0;
        if (borrow) {
            digit += 10;
        }
        reversed.append(char('0' + digit));
    }
    std::reverse(reversed.begin(), reversed.end());
    return unsignedDecimal(std::move(reversed));
}

SignedDecimal addSignedDecimal(const SignedDecimal &left, const SignedDecimal &right)
{
    if (left.negative == right.negative) {
        return signedDecimal(left.negative, addUnsignedDecimal(left.digits, right.digits));
    }
    const int comparison = compareUnsignedDecimal(left.digits, right.digits);
    if (comparison == 0) {
        return {};
    }
    return comparison > 0
        ? signedDecimal(left.negative, subtractUnsignedDecimal(left.digits, right.digits))
        : signedDecimal(right.negative, subtractUnsignedDecimal(right.digits, left.digits));
}

SignedDecimal signedFromSize(qsizetype value)
{
    return signedDecimal(false, QByteArray::number(value));
}

int compareSignedDecimal(const SignedDecimal &left, const SignedDecimal &right)
{
    if (left.negative != right.negative) {
        return left.negative ? -1 : 1;
    }
    const int comparison = compareUnsignedDecimal(left.digits, right.digits);
    return left.negative ? -comparison : comparison;
}

bool parseNumberParts(const QByteArray &lexical, NumberParts *output)
{
    qsizetype index = 0;
    const bool negative = byteAt(lexical, index) == '-';
    if (negative) {
        ++index;
    }
    const qsizetype integerStart = index;
    while (index < lexical.size() && lexical.at(index) >= '0' && lexical.at(index) <= '9') {
        ++index;
    }
    const QByteArray integerDigits = lexical.mid(integerStart, index - integerStart);
    QByteArray fractionDigits;
    if (byteAt(lexical, index) == '.') {
        const qsizetype fractionStart = ++index;
        while (index < lexical.size() && lexical.at(index) >= '0' && lexical.at(index) <= '9') {
            ++index;
        }
        fractionDigits = lexical.mid(fractionStart, index - fractionStart);
    }
    SignedDecimal exponent;
    if (byteAt(lexical, index) == 'e' || byteAt(lexical, index) == 'E') {
        ++index;
        const bool exponentNegative = byteAt(lexical, index) == '-';
        if (byteAt(lexical, index) == '+' || byteAt(lexical, index) == '-') {
            ++index;
        }
        exponent = signedDecimal(exponentNegative, lexical.mid(index));
    }

    QByteArray coefficient = unsignedDecimal(integerDigits + fractionDigits);
    qsizetype trailingZeros = 0;
    if (coefficient != QByteArrayLiteral("0")) {
        while (coefficient.endsWith('0')) {
            coefficient.chop(1);
            ++trailingZeros;
        }
    }
    SignedDecimal decimalShift = signedFromSize(trailingZeros);
    decimalShift = addSignedDecimal(decimalShift,
                                     signedDecimal(true, QByteArray::number(fractionDigits.size())));

    output->negative = negative && coefficient != QByteArrayLiteral("0");
    output->coefficient = std::move(coefficient);
    output->scale = addSignedDecimal(exponent, decimalShift);
    return true;
}

QString canonicalNumber(const NumberParts &parts)
{
    if (parts.coefficient == QByteArrayLiteral("0")) {
        return QStringLiteral("0");
    }
    QByteArray result;
    result.reserve(parts.coefficient.size() + parts.scale.digits.size() + 3);
    if (parts.negative) {
        result.append('-');
    }
    result.append(parts.coefficient);
    if (parts.scale.digits != QByteArrayLiteral("0")) {
        result.append('e');
        if (parts.scale.negative) {
            result.append('-');
        }
        result.append(parts.scale.digits);
    }
    return QString::fromLatin1(result);
}

bool isContinuation(unsigned char byte)
{
    return (byte & 0xc0U) == 0x80U;
}

bool isStrictUtf8(const QByteArray &raw)
{
    qsizetype index = 0;
    while (index < raw.size()) {
        const auto first = static_cast<unsigned char>(raw.at(index));
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            if (index + 1 >= raw.size()
                || !isContinuation(static_cast<unsigned char>(raw.at(index + 1)))) {
                return false;
            }
            index += 2;
            continue;
        }
        if (first >= 0xe0U && first <= 0xefU) {
            if (index + 2 >= raw.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(raw.at(index + 1));
            const auto third = static_cast<unsigned char>(raw.at(index + 2));
            if (!isContinuation(third)
                || (first == 0xe0U ? second < 0xa0U || second > 0xbfU
                                   : first == 0xedU ? second < 0x80U || second > 0x9fU
                                                    : !isContinuation(second))) {
                return false;
            }
            index += 3;
            continue;
        }
        if (first >= 0xf0U && first <= 0xf4U) {
            if (index + 3 >= raw.size()) {
                return false;
            }
            const auto second = static_cast<unsigned char>(raw.at(index + 1));
            const auto third = static_cast<unsigned char>(raw.at(index + 2));
            const auto fourth = static_cast<unsigned char>(raw.at(index + 3));
            if (!isContinuation(third) || !isContinuation(fourth)
                || (first == 0xf0U ? second < 0x90U || second > 0xbfU
                                   : first == 0xf4U ? second < 0x80U || second > 0x8fU
                                                    : !isContinuation(second))) {
                return false;
            }
            index += 4;
            continue;
        }
        return false;
    }
    return true;
}

class RawJsonParser {
public:
    explicit RawJsonParser(const QByteArray &raw)
        : raw_(raw)
    {
    }

    bool parse(TransportJsonValue *output, QString *error)
    {
        if (!output) {
            setError(error, QStringLiteral("transport JSON output is required"));
            return false;
        }
        if (raw_.size() > kMaxTransportJsonBytes) {
            setError(error, QStringLiteral("raw JSON exceeds the 4 MiB transport frame limit"));
            return false;
        }
        if (raw_.startsWith("\xef\xbb\xbf")) {
            setError(error, QStringLiteral("raw JSON must not contain a UTF-8 BOM"));
            return false;
        }
        if (!isStrictUtf8(raw_)) {
            setError(error, QStringLiteral("raw JSON is not strict UTF-8"));
            return false;
        }
        skipWhitespace();
        if (!parseValue(output, error)) {
            return false;
        }
        skipWhitespace();
        if (index_ != raw_.size()) {
            setError(error, QStringLiteral("raw JSON contains trailing bytes"));
            return false;
        }
        return true;
    }

private:
    bool parseValue(TransportJsonValue *output, QString *error)
    {
        if (++nodes_ > kMaxTransportJsonNodes) {
            setError(error, QStringLiteral("raw JSON exceeds the parser node limit"));
            return false;
        }
        const char current = byteAt(raw_, index_);
        if (current == '"') {
            QString value;
            if (!parseString(&value, error)) {
                return false;
            }
            output->value = std::move(value);
            return true;
        }
        if (current == '{') {
            return withDepth([&] { return parseObject(output, error); }, error);
        }
        if (current == '[') {
            return withDepth([&] { return parseArray(output, error); }, error);
        }
        if (consumeLiteral("true")) {
            output->value = true;
            return true;
        }
        if (consumeLiteral("false")) {
            output->value = false;
            return true;
        }
        if (consumeLiteral("null")) {
            output->value = std::monostate{};
            return true;
        }
        if (current == '-' || (current >= '0' && current <= '9')) {
            return parseNumber(output, error);
        }
        setError(error, QStringLiteral("raw JSON contains an invalid value"));
        return false;
    }

    template<typename Callback>
    bool withDepth(Callback callback, QString *error)
    {
        if (++depth_ > kMaxTransportJsonDepth) {
            --depth_;
            setError(error, QStringLiteral("raw JSON exceeds the parser depth limit"));
            return false;
        }
        const bool result = callback();
        --depth_;
        return result;
    }

    bool parseObject(TransportJsonValue *output, QString *error)
    {
        ++index_;
        skipWhitespace();
        JsonObject object;
        if (byteAt(raw_, index_) == '}') {
            ++index_;
            output->value = std::move(object);
            return true;
        }
        while (true) {
            if (byteAt(raw_, index_) != '"') {
                setError(error, QStringLiteral("raw JSON object key must be a string"));
                return false;
            }
            QString key;
            if (!parseString(&key, error)) {
                return false;
            }
            if (object.contains(key)) {
                setError(error, QStringLiteral("raw JSON object contains a duplicate key"));
                return false;
            }
            skipWhitespace();
            if (byteAt(raw_, index_) != ':') {
                setError(error, QStringLiteral("raw JSON object key is missing a colon"));
                return false;
            }
            ++index_;
            skipWhitespace();
            TransportJsonValue value;
            if (!parseValue(&value, error)) {
                return false;
            }
            object.insert(std::move(key), std::move(value));
            skipWhitespace();
            const char separator = byteAt(raw_, index_);
            ++index_;
            if (separator == '}') {
                output->value = std::move(object);
                return true;
            }
            if (separator != ',') {
                setError(error, QStringLiteral("raw JSON object entry is missing a comma"));
                return false;
            }
            skipWhitespace();
        }
    }

    bool parseArray(TransportJsonValue *output, QString *error)
    {
        ++index_;
        skipWhitespace();
        JsonArray array;
        if (byteAt(raw_, index_) == ']') {
            ++index_;
            output->value = std::move(array);
            return true;
        }
        while (true) {
            TransportJsonValue value;
            if (!parseValue(&value, error)) {
                return false;
            }
            array.append(std::move(value));
            skipWhitespace();
            const char separator = byteAt(raw_, index_);
            ++index_;
            if (separator == ']') {
                output->value = std::move(array);
                return true;
            }
            if (separator != ',') {
                setError(error, QStringLiteral("raw JSON array entry is missing a comma"));
                return false;
            }
            skipWhitespace();
        }
    }

    bool parseString(QString *output, QString *error)
    {
        ++index_;
        QString result;
        while (index_ < raw_.size()) {
            const auto byte = static_cast<unsigned char>(raw_.at(index_));
            if (byte == 0x22U) {
                ++index_;
                *output = std::move(result);
                return true;
            }
            if (byte < 0x20U) {
                setError(error, QStringLiteral("raw JSON string contains an unescaped control character"));
                return false;
            }
            if (byte == 0x5cU) {
                ++index_;
                if (!parseEscape(&result, error)) {
                    return false;
                }
                continue;
            }
            if (byte < 0x80U) {
                result.append(QChar(byte));
                ++index_;
                continue;
            }
            const qsizetype width = byte < 0xe0U ? 2 : byte < 0xf0U ? 3 : 4;
            uint codePoint = byte & (width == 2 ? 0x1fU : width == 3 ? 0x0fU : 0x07U);
            for (qsizetype offset = 1; offset < width; ++offset) {
                codePoint = (codePoint << 6U)
                    | (static_cast<unsigned char>(raw_.at(index_ + offset)) & 0x3fU);
            }
            if (codePoint <= 0xffffU) {
                result.append(QChar(ushort(codePoint)));
            } else {
                codePoint -= 0x10000U;
                result.append(QChar(ushort(0xd800U + (codePoint >> 10U))));
                result.append(QChar(ushort(0xdc00U + (codePoint & 0x3ffU))));
            }
            index_ += width;
        }
        setError(error, QStringLiteral("raw JSON string is unterminated"));
        return false;
    }

    bool parseEscape(QString *output, QString *error)
    {
        const char escape = byteAt(raw_, index_++);
        switch (escape) {
        case '"': output->append(QChar('"')); return true;
        case '\\': output->append(QChar('\\')); return true;
        case '/': output->append(QChar('/')); return true;
        case 'b': output->append(QChar('\b')); return true;
        case 'f': output->append(QChar('\f')); return true;
        case 'n': output->append(QChar('\n')); return true;
        case 'r': output->append(QChar('\r')); return true;
        case 't': output->append(QChar('\t')); return true;
        case 'u': break;
        default:
            setError(error, QStringLiteral("raw JSON string contains an invalid escape"));
            return false;
        }
        ushort high = 0;
        if (!parseHexCodeUnit(&high, error)) {
            return false;
        }
        if (high >= 0xd800U && high <= 0xdbffU) {
            if (raw_.mid(index_, 2) != QByteArrayLiteral("\\u")) {
                setError(error, QStringLiteral("escaped high surrogate is not followed by a low surrogate"));
                return false;
            }
            index_ += 2;
            ushort low = 0;
            if (!parseHexCodeUnit(&low, error) || low < 0xdc00U || low > 0xdfffU) {
                setError(error, QStringLiteral("raw JSON contains an invalid escaped surrogate pair"));
                return false;
            }
            output->append(QChar(high));
            output->append(QChar(low));
            return true;
        }
        if (high >= 0xdc00U && high <= 0xdfffU) {
            setError(error, QStringLiteral("raw JSON contains an unpaired escaped low surrogate"));
            return false;
        }
        output->append(QChar(high));
        return true;
    }

    bool parseHexCodeUnit(ushort *output, QString *error)
    {
        if (index_ + 4 > raw_.size()) {
            setError(error, QStringLiteral("raw JSON contains an incomplete Unicode escape"));
            return false;
        }
        ushort value = 0;
        for (int offset = 0; offset < 4; ++offset) {
            const char digit = raw_.at(index_ + offset);
            const int nibble = digit >= '0' && digit <= '9' ? digit - '0'
                : digit >= 'a' && digit <= 'f' ? digit - 'a' + 10
                : digit >= 'A' && digit <= 'F' ? digit - 'A' + 10
                                               : -1;
            if (nibble < 0) {
                setError(error, QStringLiteral("raw JSON contains an invalid Unicode escape"));
                return false;
            }
            value = ushort((value << 4U) | ushort(nibble));
        }
        index_ += 4;
        *output = value;
        return true;
    }

    bool parseNumber(TransportJsonValue *output, QString *error)
    {
        const qsizetype start = index_;
        if (byteAt(raw_, index_) == '-') {
            ++index_;
        }
        if (byteAt(raw_, index_) == '0') {
            ++index_;
            if (byteAt(raw_, index_) >= '0' && byteAt(raw_, index_) <= '9') {
                setError(error, QStringLiteral("raw JSON number has a leading zero"));
                return false;
            }
        } else {
            if (byteAt(raw_, index_) < '1' || byteAt(raw_, index_) > '9') {
                setError(error, QStringLiteral("raw JSON contains an invalid number"));
                return false;
            }
            while (byteAt(raw_, index_) >= '0' && byteAt(raw_, index_) <= '9') {
                ++index_;
            }
        }
        if (byteAt(raw_, index_) == '.') {
            ++index_;
            if (byteAt(raw_, index_) < '0' || byteAt(raw_, index_) > '9') {
                setError(error, QStringLiteral("raw JSON number fraction is missing digits"));
                return false;
            }
            while (byteAt(raw_, index_) >= '0' && byteAt(raw_, index_) <= '9') {
                ++index_;
            }
        }
        if (byteAt(raw_, index_) == 'e' || byteAt(raw_, index_) == 'E') {
            ++index_;
            if (byteAt(raw_, index_) == '+' || byteAt(raw_, index_) == '-') {
                ++index_;
            }
            if (byteAt(raw_, index_) < '0' || byteAt(raw_, index_) > '9') {
                setError(error, QStringLiteral("raw JSON number exponent is missing digits"));
                return false;
            }
            while (byteAt(raw_, index_) >= '0' && byteAt(raw_, index_) <= '9') {
                ++index_;
            }
        }
        const QByteArray lexical = raw_.mid(start, index_ - start);
        NumberParts parts;
        parseNumberParts(lexical, &parts);
        TransportJsonNumber number;
        number.lexical = QString::fromLatin1(lexical);
        number.canonical = canonicalNumber(parts);
        number.integer = parts.coefficient == QByteArrayLiteral("0") || !parts.scale.negative;
        output->value = std::move(number);
        return true;
    }

    bool consumeLiteral(QByteArrayView literal)
    {
        if (index_ + literal.size() > raw_.size()
            || QByteArrayView(raw_).sliced(index_, literal.size()) != literal) {
            return false;
        }
        index_ += literal.size();
        return true;
    }

    void skipWhitespace()
    {
        while (index_ < raw_.size()) {
            const char byte = raw_.at(index_);
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') {
                return;
            }
            ++index_;
        }
    }

    const QByteArray &raw_;
    qsizetype index_ = 0;
    qsizetype depth_ = 0;
    qsizetype nodes_ = 0;
};

NumberParts numberParts(const TransportJsonNumber &number)
{
    NumberParts result;
    parseNumberParts(number.canonical.toLatin1(), &result);
    return result;
}

int compareNumbers(const TransportJsonNumber &leftNumber,
                   const TransportJsonNumber &rightNumber)
{
    const NumberParts left = numberParts(leftNumber);
    const NumberParts right = numberParts(rightNumber);
    if (left.coefficient == QByteArrayLiteral("0") || right.coefficient == QByteArrayLiteral("0")) {
        if (left.coefficient == right.coefficient) {
            return 0;
        }
        if (left.coefficient == QByteArrayLiteral("0")) {
            return right.negative ? 1 : -1;
        }
        return left.negative ? -1 : 1;
    }
    if (left.negative != right.negative) {
        return left.negative ? -1 : 1;
    }
    const SignedDecimal leftOrder = addSignedDecimal(left.scale,
                                                       signedFromSize(left.coefficient.size()));
    const SignedDecimal rightOrder = addSignedDecimal(right.scale,
                                                        signedFromSize(right.coefficient.size()));
    int comparison = compareSignedDecimal(leftOrder, rightOrder);
    if (comparison == 0) {
        const qsizetype width = std::max(left.coefficient.size(), right.coefficient.size());
        for (qsizetype index = 0; index < width && comparison == 0; ++index) {
            const char leftDigit = index < left.coefficient.size() ? left.coefficient.at(index) : '0';
            const char rightDigit = index < right.coefficient.size() ? right.coefficient.at(index) : '0';
            comparison = leftDigit < rightDigit ? -1 : leftDigit > rightDigit ? 1 : 0;
        }
    }
    return left.negative ? -comparison : comparison;
}

bool deepEqual(const TransportJsonValue &left, const TransportJsonValue &right)
{
    if (left.value.index() != right.value.index()) {
        return false;
    }
    if (std::holds_alternative<std::monostate>(left.value)) {
        return true;
    }
    if (const auto *value = std::get_if<bool>(&left.value)) {
        return *value == std::get<bool>(right.value);
    }
    if (const auto *value = std::get_if<QString>(&left.value)) {
        return *value == std::get<QString>(right.value);
    }
    if (const auto *value = std::get_if<TransportJsonNumber>(&left.value)) {
        return compareNumbers(*value, std::get<TransportJsonNumber>(right.value)) == 0;
    }
    if (const auto *array = std::get_if<JsonArray>(&left.value)) {
        const auto &other = std::get<JsonArray>(right.value);
        if (array->size() != other.size()) {
            return false;
        }
        for (qsizetype index = 0; index < array->size(); ++index) {
            if (!deepEqual(array->at(index), other.at(index))) {
                return false;
            }
        }
        return true;
    }
    const auto &object = std::get<JsonObject>(left.value);
    const auto &other = std::get<JsonObject>(right.value);
    if (object.size() != other.size()) {
        return false;
    }
    auto leftIt = object.cbegin();
    auto rightIt = other.cbegin();
    while (leftIt != object.cend()) {
        if (leftIt.key() != rightIt.key() || !deepEqual(leftIt.value(), rightIt.value())) {
            return false;
        }
        ++leftIt;
        ++rightIt;
    }
    return true;
}

QByteArray quoteJsonString(const QString &value)
{
    QByteArray output = "\"";
    const QByteArray utf8 = value.toUtf8();
    for (unsigned char byte : utf8) {
        switch (byte) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (byte < 0x20U) {
                static constexpr std::array<char, 16> hex = {
                    '0', '1', '2', '3', '4', '5', '6', '7',
                    '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
                };
                output += "\\u00";
                output += hex[(byte >> 4U) & 0x0fU];
                output += hex[byte & 0x0fU];
            } else {
                output += char(byte);
            }
        }
    }
    output += '"';
    return output;
}

QByteArray canonicalJsonUnchecked(const TransportJsonValue &value)
{
    if (std::holds_alternative<std::monostate>(value.value)) {
        return QByteArrayLiteral("null");
    }
    if (const auto *boolean = std::get_if<bool>(&value.value)) {
        return *boolean ? QByteArrayLiteral("true") : QByteArrayLiteral("false");
    }
    if (const auto *string = std::get_if<QString>(&value.value)) {
        return quoteJsonString(*string);
    }
    if (const auto *number = std::get_if<TransportJsonNumber>(&value.value)) {
        return number->canonical.toLatin1();
    }
    if (const auto *array = std::get_if<JsonArray>(&value.value)) {
        QByteArray output = "[";
        bool first = true;
        for (const auto &entry : *array) {
            if (!first) {
                output += ',';
            }
            first = false;
            output += canonicalJsonUnchecked(entry);
        }
        output += ']';
        return output;
    }
    QByteArray output = "{";
    bool first = true;
    const auto &object = std::get<JsonObject>(value.value);
    QStringList keys = object.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &left, const QString &right) {
        return left.toUtf8() < right.toUtf8();
    });
    for (const QString &key : keys) {
        if (!first) {
            output += ',';
        }
        first = false;
        output += quoteJsonString(key);
        output += ':';
        output += canonicalJsonUnchecked(object.value(key));
    }
    output += '}';
    return output;
}

bool isUnicodeScalarString(const QString &value)
{
    for (qsizetype index = 0; index < value.size(); ++index) {
        const QChar current = value.at(index);
        if (current.isHighSurrogate()) {
            if (++index >= value.size() || !value.at(index).isLowSurrogate()) {
                return false;
            }
        } else if (current.isLowSurrogate()) {
            return false;
        }
    }
    return true;
}

bool auditTransportValue(const TransportJsonValue &value,
                         qsizetype depth,
                         qsizetype *nodes)
{
    if (++(*nodes) > kMaxTransportJsonNodes) {
        return false;
    }
    if (std::holds_alternative<std::monostate>(value.value)
        || std::holds_alternative<bool>(value.value)) {
        return true;
    }
    if (const auto *string = std::get_if<QString>(&value.value)) {
        return isUnicodeScalarString(*string);
    }
    if (const auto *number = std::get_if<TransportJsonNumber>(&value.value)) {
        const QByteArray lexical = number->lexical.toLatin1();
        if (QString::fromLatin1(lexical) != number->lexical) {
            return false;
        }
        TransportJsonValue parsed;
        QString ignored;
        if (!RawJsonParser(lexical).parse(&parsed, &ignored)) {
            return false;
        }
        const auto *parsedNumber = std::get_if<TransportJsonNumber>(&parsed.value);
        return parsedNumber && parsedNumber->lexical == number->lexical
            && parsedNumber->canonical == number->canonical
            && parsedNumber->integer == number->integer;
    }
    if (++depth > kMaxTransportJsonDepth) {
        return false;
    }
    if (const auto *array = std::get_if<JsonArray>(&value.value)) {
        for (const auto &entry : *array) {
            if (!auditTransportValue(entry, depth, nodes)) {
                return false;
            }
        }
        return true;
    }
    const auto &object = std::get<JsonObject>(value.value);
    for (auto it = object.cbegin(); it != object.cend(); ++it) {
        if (!isUnicodeScalarString(it.key()) || !auditTransportValue(it.value(), depth, nodes)) {
            return false;
        }
    }
    return true;
}

const JsonObject *asObject(const TransportJsonValue &value)
{
    return std::get_if<JsonObject>(&value.value);
}

const JsonArray *asArray(const TransportJsonValue &value)
{
    return std::get_if<JsonArray>(&value.value);
}

const QString *asString(const TransportJsonValue &value)
{
    return std::get_if<QString>(&value.value);
}

const TransportJsonNumber *asNumber(const TransportJsonValue &value)
{
    return std::get_if<TransportJsonNumber>(&value.value);
}

bool scalarLength(const QString &value, quint64 *output)
{
    quint64 count = 0;
    for (qsizetype index = 0; index < value.size(); ++index) {
        if (value.at(index).isHighSurrogate()) {
            ++index;
        }
        ++count;
    }
    *output = count;
    return true;
}

bool toNonNegativeBound(const TransportJsonValue &value, quint64 *output)
{
    const auto *number = asNumber(value);
    if (!number || !number->integer) {
        return false;
    }
    const NumberParts parts = numberParts(*number);
    if (parts.negative || parts.scale.negative) {
        return false;
    }
    if (parts.scale.digits.size() > 3) {
        return false;
    }
    bool ok = false;
    const quint64 scale = parts.scale.digits.toULongLong(&ok);
    if (!ok || scale > 18 || parts.coefficient.size() + qsizetype(scale) > 19) {
        return false;
    }
    QByteArray digits = parts.coefficient;
    digits.append(qsizetype(scale), '0');
    const quint64 result = digits.toULongLong(&ok);
    if (!ok) {
        return false;
    }
    *output = result;
    return true;
}

bool matchesType(const QString &type, const TransportJsonValue &value)
{
    if (type == QStringLiteral("null")) {
        return std::holds_alternative<std::monostate>(value.value);
    }
    if (type == QStringLiteral("boolean")) {
        return std::holds_alternative<bool>(value.value);
    }
    if (type == QStringLiteral("number")) {
        return std::holds_alternative<TransportJsonNumber>(value.value);
    }
    if (type == QStringLiteral("integer")) {
        const auto *number = asNumber(value);
        return number && number->integer;
    }
    if (type == QStringLiteral("string")) {
        return std::holds_alternative<QString>(value.value);
    }
    if (type == QStringLiteral("array")) {
        return std::holds_alternative<JsonArray>(value.value);
    }
    if (type == QStringLiteral("object")) {
        return std::holds_alternative<JsonObject>(value.value);
    }
    return false;
}

const QSet<QString> &supportedKeywords()
{
    static const QSet<QString> keywords = {
        QStringLiteral("$comment"), QStringLiteral("$defs"), QStringLiteral("$id"),
        QStringLiteral("$ref"), QStringLiteral("$schema"),
        QStringLiteral("additionalProperties"), QStringLiteral("allOf"),
        QStringLiteral("anyOf"), QStringLiteral("const"), QStringLiteral("else"),
        QStringLiteral("enum"), QStringLiteral("if"), QStringLiteral("items"),
        QStringLiteral("maximum"), QStringLiteral("maxItems"),
        QStringLiteral("maxLength"), QStringLiteral("maxProperties"),
        QStringLiteral("minimum"), QStringLiteral("minItems"),
        QStringLiteral("minLength"), QStringLiteral("minProperties"),
        QStringLiteral("not"), QStringLiteral("oneOf"),
        QStringLiteral("pattern"), QStringLiteral("properties"),
        QStringLiteral("propertyNames"), QStringLiteral("required"),
        QStringLiteral("then"), QStringLiteral("title"), QStringLiteral("type"),
        QStringLiteral("uniqueItems"),
    };
    return keywords;
}

bool auditSchemaNode(const TransportJsonValue &node,
                     const JsonObject &definitions,
                     qsizetype recursion,
                     QString *error)
{
    if (recursion > 256) {
        setError(error, QStringLiteral("transport schema exceeds the audit recursion limit"));
        return false;
    }
    if (std::holds_alternative<bool>(node.value)) {
        return true;
    }
    const auto *object = asObject(node);
    if (!object) {
        setError(error, QStringLiteral("transport schema node must be an object or boolean"));
        return false;
    }
    for (auto it = object->cbegin(); it != object->cend(); ++it) {
        if (!supportedKeywords().contains(it.key())) {
            setError(error, QStringLiteral("transport schema uses an unsupported keyword"));
            return false;
        }
    }
    if (const auto refIt = object->constFind(QStringLiteral("$ref")); refIt != object->cend()) {
        const auto *reference = asString(refIt.value());
        static const QRegularExpression referencePattern(
            QStringLiteral("^#\\/\\$defs\\/[A-Za-z][A-Za-z0-9]*$"));
        if (!reference || !referencePattern.match(*reference).hasMatch()
            || !definitions.contains(reference->mid(8))) {
            setError(error, QStringLiteral("transport schema contains an unknown local reference"));
            return false;
        }
    }
    if (const auto typeIt = object->constFind(QStringLiteral("type")); typeIt != object->cend()) {
        const auto *type = asString(typeIt.value());
        static const QSet<QString> types = {
            QStringLiteral("null"), QStringLiteral("boolean"), QStringLiteral("number"),
            QStringLiteral("integer"), QStringLiteral("string"), QStringLiteral("array"),
            QStringLiteral("object"),
        };
        if (!type || !types.contains(*type)) {
            setError(error, QStringLiteral("transport schema contains an unsupported type"));
            return false;
        }
    }
    for (const QString &keyword : {QStringLiteral("minimum"), QStringLiteral("maximum")}) {
        const auto it = object->constFind(keyword);
        if (it != object->cend() && !asNumber(it.value())) {
            setError(error, QStringLiteral("transport schema contains a non-numeric bound"));
            return false;
        }
    }
    for (const QString &keyword : {QStringLiteral("minItems"), QStringLiteral("maxItems"),
                                   QStringLiteral("minLength"), QStringLiteral("maxLength"),
                                   QStringLiteral("minProperties"), QStringLiteral("maxProperties")}) {
        const auto it = object->constFind(keyword);
        quint64 ignored = 0;
        if (it != object->cend() && !toNonNegativeBound(it.value(), &ignored)) {
            setError(error, QStringLiteral("transport schema contains an invalid size bound"));
            return false;
        }
    }
    if (const auto patternIt = object->constFind(QStringLiteral("pattern"));
        patternIt != object->cend()) {
        const auto *pattern = asString(patternIt.value());
        if (!pattern || !QRegularExpression(*pattern).isValid()) {
            setError(error, QStringLiteral("transport schema contains an invalid pattern"));
            return false;
        }
    }
    if (const auto requiredIt = object->constFind(QStringLiteral("required"));
        requiredIt != object->cend()) {
        const auto *required = asArray(requiredIt.value());
        QSet<QString> names;
        if (!required) {
            setError(error, QStringLiteral("transport schema required must be an array"));
            return false;
        }
        for (const auto &entry : *required) {
            const auto *name = asString(entry);
            if (!name || names.contains(*name)) {
                setError(error, QStringLiteral("transport schema required contains an invalid name"));
                return false;
            }
            names.insert(*name);
        }
    }
    if (const auto enumIt = object->constFind(QStringLiteral("enum"));
        enumIt != object->cend() && !asArray(enumIt.value())) {
        setError(error, QStringLiteral("transport schema enum must be an array"));
        return false;
    }
    if (const auto uniqueIt = object->constFind(QStringLiteral("uniqueItems"));
        uniqueIt != object->cend() && !std::holds_alternative<bool>(uniqueIt.value().value)) {
        setError(error, QStringLiteral("transport schema uniqueItems must be boolean"));
        return false;
    }
    for (const QString &keyword : {QStringLiteral("allOf"), QStringLiteral("anyOf"),
                                   QStringLiteral("oneOf")}) {
        const auto it = object->constFind(keyword);
        if (it == object->cend()) {
            continue;
        }
        const auto *children = asArray(it.value());
        if (!children || children->isEmpty()) {
            setError(error, QStringLiteral("transport schema combinator must be a non-empty array"));
            return false;
        }
        for (const auto &child : *children) {
            if (!auditSchemaNode(child, definitions, recursion + 1, error)) {
                return false;
            }
        }
    }
    for (const QString &keyword : {QStringLiteral("if"), QStringLiteral("then"),
                                   QStringLiteral("else"), QStringLiteral("not"),
                                   QStringLiteral("items"), QStringLiteral("propertyNames"),
                                   QStringLiteral("additionalProperties")}) {
        const auto it = object->constFind(keyword);
        if (it != object->cend()
            && !auditSchemaNode(it.value(), definitions, recursion + 1, error)) {
            return false;
        }
    }
    for (const QString &keyword : {QStringLiteral("properties"), QStringLiteral("$defs")}) {
        const auto it = object->constFind(keyword);
        if (it == object->cend()) {
            continue;
        }
        const auto *children = asObject(it.value());
        if (!children) {
            setError(error, QStringLiteral("transport schema properties must be an object"));
            return false;
        }
        for (auto child = children->cbegin(); child != children->cend(); ++child) {
            if (!auditSchemaNode(child.value(), definitions, recursion + 1, error)) {
                return false;
            }
        }
    }
    return true;
}

void collectSchemaPatterns(const TransportJsonValue &node,
                           QMap<QString, QRegularExpression> *patterns)
{
    const auto *object = asObject(node);
    if (!object) {
        return;
    }
    if (const auto patternIt = object->constFind(QStringLiteral("pattern"));
        patternIt != object->cend()) {
        const QString pattern = *asString(patternIt.value());
        patterns->insert(pattern, QRegularExpression(pattern));
    }
    for (const QString &keyword : {QStringLiteral("allOf"), QStringLiteral("anyOf"),
                                   QStringLiteral("oneOf")}) {
        const auto it = object->constFind(keyword);
        if (it != object->cend()) {
            for (const auto &child : *asArray(it.value())) {
                collectSchemaPatterns(child, patterns);
            }
        }
    }
    for (const QString &keyword : {QStringLiteral("if"), QStringLiteral("then"),
                                   QStringLiteral("else"), QStringLiteral("not"),
                                   QStringLiteral("items"), QStringLiteral("propertyNames"),
                                   QStringLiteral("additionalProperties")}) {
        const auto it = object->constFind(keyword);
        if (it != object->cend()) {
            collectSchemaPatterns(it.value(), patterns);
        }
    }
    for (const QString &keyword : {QStringLiteral("properties"), QStringLiteral("$defs")}) {
        const auto it = object->constFind(keyword);
        if (it != object->cend()) {
            for (auto child = asObject(it.value())->cbegin(); child != asObject(it.value())->cend();
                 ++child) {
                collectSchemaPatterns(child.value(), patterns);
            }
        }
    }
}

} // namespace

class TransportSchemaRuntime::Impl {
public:
    explicit Impl(TransportJsonValue schema)
        : schema_(std::move(schema))
        , definitions_(asObject(asObject(schema_)->constFind(QStringLiteral("$defs")).value()))
    {
        collectSchemaPatterns(schema_, &patterns_);
    }

    bool validateRoot(const TransportJsonValue &value, QString *error) const
    {
        return validateNode(schema_, value, QStringLiteral("$"), 0, error);
    }

    bool validateDefinition(const QString &definition,
                            const TransportJsonValue &value,
                            QString *error) const
    {
        const auto it = definitions_->constFind(definition);
        if (it == definitions_->cend()) {
            setError(error, QStringLiteral("unknown transport definition"));
            return false;
        }
        return validateNode(it.value(), value, QStringLiteral("$"), 0, error);
    }

private:
    bool validateNode(const TransportJsonValue &schemaNode,
                      const TransportJsonValue &value,
                      const QString &path,
                      qsizetype referenceDepth,
                      QString *error) const
    {
        if (const auto *boolean = std::get_if<bool>(&schemaNode.value)) {
            if (*boolean) {
                return true;
            }
            setError(error, path + QStringLiteral(" is rejected by a false schema"));
            return false;
        }
        const auto *schema = asObject(schemaNode);
        if (!schema) {
            setError(error, QStringLiteral("transport schema node is invalid"));
            return false;
        }
        if (const auto refIt = schema->constFind(QStringLiteral("$ref")); refIt != schema->cend()) {
            if (referenceDepth >= 256) {
                setError(error, QStringLiteral("transport schema reference recursion limit exceeded"));
                return false;
            }
            const QString reference = *asString(refIt.value());
            const auto definition = definitions_->constFind(reference.mid(8));
            if (definition == definitions_->cend()
                || !validateNode(definition.value(), value, path, referenceDepth + 1, error)) {
                return false;
            }
        }
        if (const auto typeIt = schema->constFind(QStringLiteral("type")); typeIt != schema->cend()
            && !matchesType(*asString(typeIt.value()), value)) {
            setError(error, path + QStringLiteral(" has the wrong JSON type"));
            return false;
        }
        if (const auto constIt = schema->constFind(QStringLiteral("const"));
            constIt != schema->cend() && !deepEqual(value, constIt.value())) {
            setError(error, path + QStringLiteral(" does not equal its const value"));
            return false;
        }
        if (const auto enumIt = schema->constFind(QStringLiteral("enum")); enumIt != schema->cend()) {
            const auto *entries = asArray(enumIt.value());
            bool matched = false;
            for (const auto &entry : *entries) {
                if (deepEqual(value, entry)) {
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                setError(error, path + QStringLiteral(" is outside its enum"));
                return false;
            }
        }
        if (const auto *number = asNumber(value)) {
            if (const auto minimum = schema->constFind(QStringLiteral("minimum"));
                minimum != schema->cend()
                && compareNumbers(*number, *asNumber(minimum.value())) < 0) {
                setError(error, path + QStringLiteral(" is below minimum"));
                return false;
            }
            if (const auto maximum = schema->constFind(QStringLiteral("maximum"));
                maximum != schema->cend()
                && compareNumbers(*number, *asNumber(maximum.value())) > 0) {
                setError(error, path + QStringLiteral(" is above maximum"));
                return false;
            }
        }
        if (const auto *string = asString(value)) {
            quint64 length = 0;
            scalarLength(*string, &length);
            for (const auto &[keyword, below, message] : {
                     std::tuple{QStringLiteral("minLength"), true, QStringLiteral(" is shorter than minLength")},
                     std::tuple{QStringLiteral("maxLength"), false, QStringLiteral(" is longer than maxLength")},
                 }) {
                const auto it = schema->constFind(keyword);
                quint64 bound = 0;
                if (it != schema->cend()) {
                    toNonNegativeBound(it.value(), &bound);
                    if ((below && length < bound) || (!below && length > bound)) {
                        setError(error, path + message);
                        return false;
                    }
                }
            }
            if (const auto patternIt = schema->constFind(QStringLiteral("pattern"));
                patternIt != schema->cend()
                && !patterns_.constFind(*asString(patternIt.value())).value().match(*string).hasMatch()) {
                setError(error, path + QStringLiteral(" does not match pattern"));
                return false;
            }
        }
        if (const auto *array = asArray(value)) {
            quint64 minimum = 0;
            quint64 maximum = std::numeric_limits<quint64>::max();
            if (const auto it = schema->constFind(QStringLiteral("minItems")); it != schema->cend()) {
                toNonNegativeBound(it.value(), &minimum);
            }
            if (const auto it = schema->constFind(QStringLiteral("maxItems")); it != schema->cend()) {
                toNonNegativeBound(it.value(), &maximum);
            }
            if (quint64(array->size()) < minimum || quint64(array->size()) > maximum) {
                setError(error, path + QStringLiteral(" violates array bounds"));
                return false;
            }
            if (const auto uniqueIt = schema->constFind(QStringLiteral("uniqueItems"));
                uniqueIt != schema->cend() && std::get<bool>(uniqueIt.value().value)) {
                QSet<QByteArray> identities;
                for (const auto &entry : *array) {
                    const QByteArray identity = canonicalJsonUnchecked(entry);
                    if (identities.contains(identity)) {
                        setError(error, path + QStringLiteral(" contains duplicate items"));
                        return false;
                    }
                    identities.insert(identity);
                }
            }
            if (const auto itemsIt = schema->constFind(QStringLiteral("items"));
                itemsIt != schema->cend()) {
                for (qsizetype index = 0; index < array->size(); ++index) {
                    if (!validateNode(itemsIt.value(), array->at(index),
                                      path + QStringLiteral("[") + QString::number(index)
                                          + QStringLiteral("]"),
                                      referenceDepth, error)) {
                        return false;
                    }
                }
            }
        }
        if (const auto *object = asObject(value)) {
            quint64 minimum = 0;
            quint64 maximum = std::numeric_limits<quint64>::max();
            if (const auto it = schema->constFind(QStringLiteral("minProperties"));
                it != schema->cend()) {
                toNonNegativeBound(it.value(), &minimum);
            }
            if (const auto it = schema->constFind(QStringLiteral("maxProperties"));
                it != schema->cend()) {
                toNonNegativeBound(it.value(), &maximum);
            }
            if (quint64(object->size()) < minimum || quint64(object->size()) > maximum) {
                setError(error, path + QStringLiteral(" violates object property bounds"));
                return false;
            }
            if (const auto requiredIt = schema->constFind(QStringLiteral("required"));
                requiredIt != schema->cend()) {
                for (const auto &entry : *asArray(requiredIt.value())) {
                    const QString name = *asString(entry);
                    if (!object->contains(name)) {
                        setError(error, path + QStringLiteral(" is missing a required property"));
                        return false;
                    }
                }
            }
            const JsonObject emptyProperties;
            const JsonObject *properties = &emptyProperties;
            if (const auto propertiesIt = schema->constFind(QStringLiteral("properties"));
                propertiesIt != schema->cend()) {
                properties = asObject(propertiesIt.value());
                for (auto it = properties->cbegin(); it != properties->cend(); ++it) {
                    const auto valueIt = object->constFind(it.key());
                    if (valueIt != object->cend()
                        && !validateNode(it.value(), valueIt.value(),
                                         path + QStringLiteral(".") + it.key(),
                                         referenceDepth, error)) {
                        return false;
                    }
                }
            }
            if (const auto propertyNames = schema->constFind(QStringLiteral("propertyNames"));
                propertyNames != schema->cend()) {
                for (auto it = object->cbegin(); it != object->cend(); ++it) {
                    TransportJsonValue key;
                    key.value = it.key();
                    if (!validateNode(propertyNames.value(), key,
                                      path + QStringLiteral("{key}"), referenceDepth, error)) {
                        return false;
                    }
                }
            }
            if (const auto additional = schema->constFind(QStringLiteral("additionalProperties"));
                additional != schema->cend()) {
                for (auto it = object->cbegin(); it != object->cend(); ++it) {
                    if (properties->contains(it.key())) {
                        continue;
                    }
                    if (const auto *allowed = std::get_if<bool>(&additional.value().value)) {
                        if (!*allowed) {
                            setError(error, path + QStringLiteral(" contains an unknown property"));
                            return false;
                        }
                    } else if (!validateNode(additional.value(), it.value(),
                                             path + QStringLiteral(".<additional>"),
                                             referenceDepth, error)) {
                        return false;
                    }
                }
            }
        }

        const auto branchAccepted = [&](const TransportJsonValue &branch) {
            QString ignored;
            return validateNode(branch, value, path, referenceDepth, &ignored);
        };
        if (const auto allOf = schema->constFind(QStringLiteral("allOf")); allOf != schema->cend()) {
            for (const auto &branch : *asArray(allOf.value())) {
                if (!branchAccepted(branch)) {
                    setError(error, path + QStringLiteral(" fails allOf"));
                    return false;
                }
            }
        }
        if (const auto anyOf = schema->constFind(QStringLiteral("anyOf")); anyOf != schema->cend()) {
            bool accepted = false;
            for (const auto &branch : *asArray(anyOf.value())) {
                accepted = accepted || branchAccepted(branch);
            }
            if (!accepted) {
                setError(error, path + QStringLiteral(" fails anyOf"));
                return false;
            }
        }
        if (const auto oneOf = schema->constFind(QStringLiteral("oneOf")); oneOf != schema->cend()) {
            qsizetype accepted = 0;
            for (const auto &branch : *asArray(oneOf.value())) {
                if (branchAccepted(branch)) {
                    ++accepted;
                }
            }
            if (accepted != 1) {
                setError(error, path + QStringLiteral(" fails oneOf"));
                return false;
            }
        }
        if (const auto notIt = schema->constFind(QStringLiteral("not"));
            notIt != schema->cend() && branchAccepted(notIt.value())) {
            setError(error, path + QStringLiteral(" matches not"));
            return false;
        }
        if (const auto ifIt = schema->constFind(QStringLiteral("if")); ifIt != schema->cend()) {
            const QString selected = branchAccepted(ifIt.value())
                ? QStringLiteral("then") : QStringLiteral("else");
            const auto selectedIt = schema->constFind(selected);
            if (selectedIt != schema->cend()
                && !validateNode(selectedIt.value(), value, path, referenceDepth, error)) {
                return false;
            }
        }
        return true;
    }

    TransportJsonValue schema_;
    const JsonObject *definitions_;
    QMap<QString, QRegularExpression> patterns_;
};

bool parseTransportJsonRaw(const QByteArray &raw,
                           TransportJsonValue *output,
                           QString *error)
{
    return RawJsonParser(raw).parse(output, error);
}

QByteArray canonicalTransportJson(const TransportJsonValue &value)
{
    qsizetype nodes = 0;
    if (!auditTransportValue(value, 0, &nodes)) {
        return {};
    }
    QByteArray canonical = canonicalJsonUnchecked(value);
    if (canonical.size() > kMaxTransportJsonBytes) {
        return {};
    }
    return canonical;
}

TransportSchemaRuntime::TransportSchemaRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

TransportSchemaRuntime::~TransportSchemaRuntime() = default;
TransportSchemaRuntime::TransportSchemaRuntime(TransportSchemaRuntime &&) noexcept = default;
TransportSchemaRuntime &TransportSchemaRuntime::operator=(TransportSchemaRuntime &&) noexcept = default;

std::unique_ptr<TransportSchemaRuntime> TransportSchemaRuntime::fromRawSchema(
    const QByteArray &schemaRaw,
    QString *error)
{
    TransportJsonValue schema;
    if (!parseTransportJsonRaw(schemaRaw, &schema, error)) {
        return nullptr;
    }
    const auto *root = asObject(schema);
    if (!root) {
        setError(error, QStringLiteral("transport schema root must be an object"));
        return nullptr;
    }
    const auto dialect = root->constFind(QStringLiteral("$schema"));
    if (dialect == root->cend() || !asString(dialect.value())
        || *asString(dialect.value())
            != QStringLiteral("https://json-schema.org/draft/2020-12/schema")) {
        setError(error, QStringLiteral("transport schema must use Draft 2020-12"));
        return nullptr;
    }
    const auto definitionsIt = root->constFind(QStringLiteral("$defs"));
    const auto *definitions = definitionsIt == root->cend()
        ? nullptr : asObject(definitionsIt.value());
    if (!definitions) {
        setError(error, QStringLiteral("transport schema is missing $defs"));
        return nullptr;
    }
    if (!auditSchemaNode(schema, *definitions, 0, error)) {
        return nullptr;
    }
    return std::unique_ptr<TransportSchemaRuntime>(
        new TransportSchemaRuntime(std::make_unique<Impl>(std::move(schema))));
}

bool TransportSchemaRuntime::validateDefinitionRaw(const QString &definition,
                                                    const QByteArray &raw,
                                                    TransportJsonValue *output,
                                                    QString *error) const
{
    TransportJsonValue value;
    if (!parseTransportJsonRaw(raw, &value, error)
        || !impl_->validateDefinition(definition, value, error)) {
        return false;
    }
    if (output) {
        *output = std::move(value);
    }
    return true;
}

bool TransportSchemaRuntime::validateRootRaw(const QByteArray &raw,
                                              TransportJsonValue *output,
                                              QString *error) const
{
    TransportJsonValue value;
    if (!parseTransportJsonRaw(raw, &value, error)
        || !impl_->validateRoot(value, error)) {
        return false;
    }
    if (output) {
        *output = std::move(value);
    }
    return true;
}

} // namespace aegisy::aap::transport_runtime
