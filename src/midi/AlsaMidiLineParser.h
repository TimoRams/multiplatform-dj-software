#pragma once

#include <QRegularExpression>
#include <QString>

#include <algorithm>

namespace midi_internal {

struct AlsaControlChange
{
    bool valid = false;
    int channel = 0;
    int control = 0;
    int value = 0;
};

inline AlsaControlChange parseAlsaControlChange(const QString& line)
{
    static const QRegularExpression standardRx(
        R"(Control\s+change\s+(\d+)\s*,\s*(?:controller|control)\s+(\d+)\s*,\s*value\s+(-?\d+))",
        QRegularExpression::CaseInsensitiveOption);
    const auto standard = standardRx.match(line);
    if (standard.hasMatch()) {
        return {
            true,
            standard.captured(1).toInt(),
            standard.captured(2).toInt(),
            standard.captured(3).toInt()
        };
    }

    static const QRegularExpression parenthesizedRx(
        R"((?:Chan|Channel)\s+(\d+).*?\((\d+)\s+(\d+)\s+(\d+)\))",
        QRegularExpression::CaseInsensitiveOption);
    const auto parenthesized = parenthesizedRx.match(line);
    if (parenthesized.hasMatch()) {
        const int status = parenthesized.captured(2).toInt();
        if ((status & 0xf0) == 0xb0) {
            return {
                true,
                status & 0x0f,
                parenthesized.captured(3).toInt(),
                parenthesized.captured(4).toInt()
            };
        }
        return {};
    }

    static const QRegularExpression verboseRx(
        R"((?:Chan|Channel)\s+(\d+).*?(?:controller|control)\s+(\d+).*?value\s+(-?\d+))",
        QRegularExpression::CaseInsensitiveOption);
    const auto verbose = verboseRx.match(line);
    if (verbose.hasMatch()) {
        return {
            true,
            std::max(0, verbose.captured(1).toInt() - 1),
            verbose.captured(2).toInt(),
            verbose.captured(3).toInt()
        };
    }

    static const QRegularExpression numberRx(R"((\d+))");
    QList<int> numbers;
    auto matches = numberRx.globalMatch(line);
    while (matches.hasNext())
        numbers.push_back(matches.next().captured(1).toInt());
    if (numbers.size() < 3)
        return {};

    return {
        true,
        numbers.at(numbers.size() - 3),
        numbers.at(numbers.size() - 2),
        numbers.at(numbers.size() - 1)
    };
}

} // namespace midi_internal
