#pragma once

#include <random>

namespace RealtimeSearch {

inline double pseudo_harmonic_prob_lower(double a, double b, double x) {
	if (b <= a || x <= a) return 1.0;
	if (x >= b) return 0.0;
	double den = (b - a) * (1.0 + x);
	if (den <= 0.0) return 1.0;
	double p = ((b - x) * (1.0 + a)) / den;
	if (p < 0.0) return 0.0;
	if (p > 1.0) return 1.0;
	return p;
}

template <typename RNG>
inline bool randomized_pseudo_harmonic(double a, double b, double x, RNG& rng) {
	std::uniform_real_distribution<double> u(0.0, 1.0);
	return u(rng) < pseudo_harmonic_prob_lower(a, b, x);
}

} // namespace RealtimeSearch
