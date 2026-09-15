// Single-frame spatial decomposition, not a temporal/fixed-noise separation.
#pragma once
#include <cmath>
#include <cstddef>
#include <limits>

namespace profile_noise {

enum class Status {
    NotComputed, Ready, InvalidRegion, InsufficientPlane, NonFinite,
    RemotePreview, MissingPixels
};

struct Result {
    int width = 0, height = 0;       // actual plane dimensions, not mosaic dimensions
    size_t samples = 0;             // all selected plane pixels, including invalid ones
    double pVariance = 0, rowVariance = 0, colVariance = 0;
    Status status = Status::NotComputed;
    bool pClamped = false, rowClamped = false, colClamped = false;

    bool valid() const { return status == Status::Ready; }
    bool ratioValid() const { return valid() && pVariance > 0; }
    bool clamped() const { return pClamped || rowClamped || colClamped; }
    double pSigma() const { return std::sqrt(pVariance); }
    double rowSigma() const { return std::sqrt(rowVariance); }
    double colSigma() const { return std::sqrt(colVariance); }
    double rowRatio() const {
        return ratioValid() ? std::sqrt(rowVariance / pVariance)
                            : std::numeric_limits<double>::quiet_NaN();
    }
    double colRatio() const {
        return ratioValid() ? std::sqrt(colVariance / pVariance)
                            : std::numeric_limits<double>::quiet_NaN();
    }
    const char* reason() const {
        switch (status) {
        case Status::Ready:             return "ok";
        case Status::InvalidRegion:     return "invalid region";
        case Status::InsufficientPlane: return "plane too small (1 - 1/H - 1/W <= 0)";
        case Status::NonFinite:         return "plane contains non-finite pixels";
        case Status::RemotePreview:     return "full-resolution remote pixels required";
        case Status::MissingPixels:     return "pixels not materialized";
        default:                       return "not computed";
        }
    }
};

} // namespace profile_noise
