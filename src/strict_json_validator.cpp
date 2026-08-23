#include "strict_json_validator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSet>
#include <QString>

namespace {

class Scanner
{
public:
    Scanner(const QByteArray &bytes, int maximumDepth, int maximumNodes)
        : m_bytes(bytes)
        , m_maximumDepth(maximumDepth)
        , m_maximumNodes(maximumNodes)
    {
    }

    bool accepts()
    {
        skipWhitespace();
        if (!parseValue(0)) return false;
        skipWhitespace();
        return m_offset == m_bytes.size();
    }

private:
    bool parseValue(int depth)
    {
        if (depth > m_maximumDepth || ++m_nodes > m_maximumNodes
                || m_offset >= m_bytes.size()) {
            return false;
        }
        const char next = m_bytes.at(m_offset);
        if (next == '{') return parseObject(depth);
        if (next == '[') return parseArray(depth);
        if (next == '"') {
            QString ignored;
            return parseString(&ignored);
        }
        if (next == 't') return consumeLiteral(QByteArrayLiteral("true"));
        if (next == 'f') return consumeLiteral(QByteArrayLiteral("false"));
        if (next == 'n') return consumeLiteral(QByteArrayLiteral("null"));
        return parseNumber();
    }

    bool parseObject(int depth)
    {
        ++m_offset;
        skipWhitespace();
        if (consume('}')) return true;
        QSet<QString> keys;
        while (m_offset < m_bytes.size()) {
            QString key;
            if (!parseString(&key) || keys.contains(key)) return false;
            keys.insert(key);
            skipWhitespace();
            if (!consume(':')) return false;
            skipWhitespace();
            if (!parseValue(depth + 1)) return false;
            skipWhitespace();
            if (consume('}')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseArray(int depth)
    {
        ++m_offset;
        skipWhitespace();
        if (consume(']')) return true;
        while (m_offset < m_bytes.size()) {
            if (!parseValue(depth + 1)) return false;
            skipWhitespace();
            if (consume(']')) return true;
            if (!consume(',')) return false;
            skipWhitespace();
        }
        return false;
    }

    bool parseString(QString *decoded)
    {
        if (m_offset >= m_bytes.size() || m_bytes.at(m_offset) != '"') return false;
        const qsizetype start = m_offset++;
        bool escaped = false;
        while (m_offset < m_bytes.size()) {
            const char byte = m_bytes.at(m_offset++);
            if (byte == '"' && !escaped) {
                const QByteArray token = m_bytes.mid(start, m_offset - start);
                QJsonParseError error;
                const QJsonDocument document = QJsonDocument::fromJson(
                    QByteArrayLiteral("[") + token + QByteArrayLiteral("]"), &error);
                if (error.error != QJsonParseError::NoError || !document.isArray()
                        || document.array().size() != 1
                        || !document.array().first().isString()) {
                    return false;
                }
                *decoded = document.array().first().toString();
                return true;
            }
            if (byte == '\\' && !escaped) {
                escaped = true;
            } else {
                escaped = false;
            }
        }
        return false;
    }

    bool parseNumber()
    {
        const qsizetype start = m_offset;
        while (m_offset < m_bytes.size()) {
            const char byte = m_bytes.at(m_offset);
            if ((byte >= '0' && byte <= '9') || byte == '-' || byte == '+'
                    || byte == '.' || byte == 'e' || byte == 'E') {
                ++m_offset;
                continue;
            }
            break;
        }
        return m_offset > start;
    }

    bool consumeLiteral(const QByteArray &literal)
    {
        if (m_bytes.mid(m_offset, literal.size()) != literal) return false;
        m_offset += literal.size();
        return true;
    }

    bool consume(char expected)
    {
        if (m_offset >= m_bytes.size() || m_bytes.at(m_offset) != expected) {
            return false;
        }
        ++m_offset;
        return true;
    }

    void skipWhitespace()
    {
        while (m_offset < m_bytes.size()) {
            const char byte = m_bytes.at(m_offset);
            if (byte != ' ' && byte != '\t' && byte != '\r' && byte != '\n') return;
            ++m_offset;
        }
    }

    const QByteArray &m_bytes;
    int m_maximumDepth;
    int m_maximumNodes;
    qsizetype m_offset = 0;
    int m_nodes = 0;
};

} // namespace

bool StrictJsonValidator::accepts(const QByteArray &bytes,
                                  int maximumDepth,
                                  int maximumNodes)
{
    if (bytes.isEmpty() || maximumDepth < 0 || maximumNodes < 1) return false;
    return Scanner(bytes, maximumDepth, maximumNodes).accepts();
}
