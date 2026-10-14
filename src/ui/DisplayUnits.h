#pragma once

#include <QString>
#include <cmath>
#include <utility>

namespace rf
{
inline QString decibelText(double value)
{
    if (!std::isfinite(value))
        return std::isnan(value) ? "unavailable" : value > 0 ? "∞" : "−∞";
    return QString::number(std::abs(value) < 0.05 ? 0 : value, 'f', 1);
}

inline double frequencyDivisor(double value)
{
    const double magnitude = std::abs(std::round(value));
    return magnitude >= 1e9 ? 1e9 : magnitude >= 1e6 ? 1e6 : magnitude >= 1e3 ? 1e3 : 1;
}

inline QString frequencyUnit(double divisor)
{
    return divisor == 1e9 ? "GHz" : divisor == 1e6 ? "MHz" : divisor == 1e3 ? "kHz" : "Hz";
}

inline QString frequencyNumber(double value, double divisor)
{
    // Round in Hz before scaling so even a GHz label retains one-Hz precision.
    const double rounded = std::round(value);
    const int decimals = divisor == 1e9 ? 9 : divisor == 1e6 ? 6 : divisor == 1e3 ? 3 : 0;
    QString result = QString::number(rounded == 0 ? 0 : rounded / divisor, 'f', decimals);
    if (decimals > 0) {
        while (result.endsWith('0'))
            result.chop(1);
        if (result.endsWith('.'))
            result.chop(1);
    }
    return result;
}

inline QString frequencyText(double value)
{
    const double divisor = frequencyDivisor(value);
    return frequencyNumber(value, divisor) + " " + frequencyUnit(divisor);
}

inline std::pair<double, QString> timeUnit(double seconds)
{
    const double magnitude = std::abs(seconds);
    if (magnitude == 0 || magnitude >= 1)
        return {1, "s"};
    if (magnitude >= 1e-3)
        return {1e3, "ms"};
    if (magnitude >= 1e-6)
        return {1e6, "µs"};
    return {1e9, "ns"};
}

inline QString durationText(double seconds)
{
    const auto [scale, unit] = timeUnit(seconds);
    return QString::number(seconds * scale, 'g', 10) + " " + unit;
}
} // namespace rf
