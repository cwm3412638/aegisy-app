#ifndef STRICT_JSON_VALIDATOR_H
#define STRICT_JSON_VALIDATOR_H

#include <QByteArray>

class StrictJsonValidator
{
public:
    static bool accepts(const QByteArray &bytes,
                        int maximumDepth = 16,
                        int maximumNodes = 8192);
};

#endif // STRICT_JSON_VALIDATOR_H
