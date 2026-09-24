#pragma once

// Where the sun and the moon are, from the time and the place alone (low-precision formulas from the Astronomical
// Almanac, good to a fraction of a degree for the sun and about a degree for the moon). This drives the sky: it
// knows the real sunrise, sunset and twilight for any date without a network, and the moon's place and phase.

#include <algorithm>
#include <cmath>
#include <ctime>

namespace Astro {

constexpr double kDeg = M_PI / 180.0;

struct Position {
  double elevation = 0; // degrees above the horizon (negative below it)
  double azimuth = 0;   // degrees from north through east (90 east, 180 south, 270 west)
};

struct Sky {
  Position sun, moon;
  double moonPhase = 0;   // 0 new, 0.25 first quarter, 0.5 full, 0.75 last quarter
  double moonLit = 0;     // lit fraction of the disc, 0..1
  bool afternoon = false; // the sun is past its highest point (it is setting rather than rising)
};

inline double wrap360(double a) {
  a = std::fmod(a, 360.0);
  return a < 0 ? a + 360.0 : a;
}

// Equatorial coordinates (right ascension, declination; degrees) to the local horizon.
inline Position toHorizon(double ra, double dec, double daysJ2000, double lat, double lon) {
  const double gmst = wrap360(280.46061837 + 360.98564736629 * daysJ2000);
  const double ha = (gmst + lon - ra) * kDeg;
  const double la = lat * kDeg, de = dec * kDeg;
  const double sinEl = std::sin(la) * std::sin(de) + std::cos(la) * std::cos(de) * std::cos(ha);
  Position p;
  p.elevation = std::asin(std::clamp(sinEl, -1.0, 1.0)) / kDeg;
  // Azimuth from the south, westwards, then turned to count from the north.
  p.azimuth =
      wrap360(std::atan2(std::sin(ha), std::cos(ha) * std::sin(la) - std::tan(de) * std::cos(la)) / kDeg + 180.0);
  // Refraction lifts objects near the horizon by about half a degree.
  if (p.elevation > -1.0) p.elevation += 0.57 * std::clamp(1.0 - p.elevation / 10.0, 0.0, 1.0);
  return p;
}

inline Sky skyAt(std::time_t t, double lat, double lon) {
  const double d = (double)t / 86400.0 + 2440587.5 - 2451545.0; // days since J2000.0
  const double eps = (23.439 - 0.0000004 * d) * kDeg;
  auto equatorial = [&](double lonEcl, double latEcl, double &ra, double &dec) {
    const double l = lonEcl * kDeg, b = latEcl * kDeg;
    ra = wrap360(std::atan2(std::sin(l) * std::cos(eps) - std::tan(b) * std::sin(eps), std::cos(l)) / kDeg);
    dec = std::asin(std::sin(b) * std::cos(eps) + std::cos(b) * std::sin(eps) * std::sin(l)) / kDeg;
  };

  Sky s;
  // Sun
  const double g = wrap360(357.528 + 0.9856003 * d) * kDeg;
  const double sunLon = wrap360(280.460 + 0.9856474 * d + 1.915 * std::sin(g) + 0.020 * std::sin(2 * g));
  double ra, dec;
  equatorial(sunLon, 0.0, ra, dec);
  s.sun = toHorizon(ra, dec, d, lat, lon);
  s.afternoon = s.sun.azimuth > 180.0;

  // Moon
  const double mL = wrap360(218.316 + 13.176396 * d);
  const double mM = wrap360(134.963 + 13.064993 * d) * kDeg;
  const double mF = wrap360(93.272 + 13.229350 * d) * kDeg;
  const double moonLon = wrap360(mL + 6.289 * std::sin(mM));
  const double moonLat = 5.128 * std::sin(mF);
  equatorial(moonLon, moonLat, ra, dec);
  s.moon = toHorizon(ra, dec, d, lat, lon);
  s.moonPhase = wrap360(moonLon - sunLon) / 360.0;
  s.moonLit = 0.5 * (1.0 - std::cos(s.moonPhase * 2.0 * M_PI));
  return s;
}

} // namespace Astro
