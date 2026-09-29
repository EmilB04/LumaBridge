// The dashboard's graphs: the last two minutes of each performance figure, one sample a
// second; and the power supply's load (how much of its rating the PC draws). Pure C++,
// tested.
#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace luma::app::perf {

// One figure's recent values, oldest first; a gap (no reading) is stored as -1.
class Series {
public:
    static constexpr size_t kLength = 120;  // two minutes at one sample a second

    void Push(double v) {
        if (values_.size() == kLength) values_.erase(values_.begin());
        values_.push_back(v);
    }
    const std::vector<double>& values() const { return values_; }
    bool Any() const {
        return std::any_of(values_.begin(), values_.end(), [](double v) { return v >= 0; });
    }
    double Max() const {
        double m = -1;
        for (double v : values_) m = std::max(m, v);
        return m;
    }

private:
    std::vector<double> values_;
};

// What the PC draws from its power supply (DC, what the supply's rating is for), estimated
// from what's measured: the processor's package and the graphics cards' board power, plus the
// rest of the PC, which reports nothing (motherboard, memory, drives, fans, USB): about 35 W,
// 5 W a drive and 2 W a fan. -1 if neither the processor nor a graphics card reports power.
struct Draw {
    double measured = -1;  // CPU + graphics, as reported
    double rest = 0;       // the estimate for the rest
    double total = -1;
};

inline Draw EstimateDraw(double cpuW, double gpuW, int drives, int fans) {
    Draw d;
    if (cpuW < 0 && gpuW < 0) return d;
    d.measured = std::max(0.0, cpuW) + std::max(0.0, gpuW);
    d.rest = 35 + 5.0 * std::max(0, drives) + 2.0 * std::max(0, fans);
    d.total = d.measured + d.rest;
    return d;
}

// How loaded the supply is (0..1+), or -1 without a rating.
inline double PsuLoad(double drawW, int ratedW) { return ratedW > 0 && drawW >= 0 ? drawW / ratedW : -1; }

}  // namespace luma::app::perf
