#pragma once

#include "PseudoHarmonic.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace BlueprintActionTranslation {

enum class Kind { Fold, Call, AllIn, Raise };

struct BettingContext {
	int total_pot = 0;
	int max_commitment = 0;
	int actor_commitment = 0;
	int actor_stack = 0;
	int last_raise = 0;
};

struct Translation {
	unsigned char lower_action = 0;
	unsigned char upper_action = 0;
	double lower_probability = 1.0;
	unsigned char sampled_action = 0;
	bool exact = true;
};

inline bool is_raise_byte(unsigned char action) {
	return action != 'd' && action != 'l' && action != 'n' &&
		(action <= 80 || action == 160);
}

inline int raise_increment(int pot_after_call, unsigned char action) {
	if (!is_raise_byte(action) || pot_after_call <= 0)
		throw std::runtime_error("BlueprintActionTranslation: invalid raise action/context");
	return action == 3
		? (pot_after_call / 400) * 100
		: (pot_after_call * static_cast<int>(action) / 200) * 100;
}

inline bool legal_raise(const BettingContext& context, unsigned char action) {
	int call = context.max_commitment - context.actor_commitment;
	if (call < 0 || context.total_pot < 0 || context.actor_stack < 0) return false;
	int increment = raise_increment(context.total_pot + call, action);
	return increment > 0 && increment >= context.last_raise &&
		context.actor_stack > call + increment + 1000;
}

template <typename RNG>
Translation translate(
	const std::vector<unsigned char>& node_actions,
	Kind kind,
	const BettingContext& context,
	int observed_new_total,
	RNG& rng)
{
	auto exact_class = [&](unsigned char wanted) {
		auto it = std::find(node_actions.begin(), node_actions.end(), wanted);
		if (it == node_actions.end())
			throw std::runtime_error("BlueprintActionTranslation: observed action class absent from node");
		Translation result;
		result.lower_action = result.upper_action = result.sampled_action = wanted;
		return result;
	};
	if (kind == Kind::Fold) return exact_class('d');
	if (kind == Kind::Call) return exact_class('l');
	if (kind == Kind::AllIn) return exact_class('n');

	int call = context.max_commitment - context.actor_commitment;
	int pot_after_call = context.total_pot + call;
	int observed_increment = observed_new_total - context.max_commitment;
	if (call < 0 || pot_after_call <= 0 || observed_increment <= 0 ||
		observed_new_total >= context.actor_commitment + context.actor_stack)
		throw std::runtime_error("BlueprintActionTranslation: invalid observed raise");

	struct Candidate { unsigned char action; int increment; double fraction; };
	std::vector<Candidate> candidates;
	for (unsigned char action : node_actions) {
		if (!is_raise_byte(action) || !legal_raise(context, action)) continue;
		int increment = raise_increment(pot_after_call, action);
		candidates.push_back({action, increment, static_cast<double>(increment) / pot_after_call});
	}
	if (candidates.empty())
		throw std::runtime_error("BlueprintActionTranslation: node has no legal raise candidates");
	std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
		if (a.fraction != b.fraction) return a.fraction < b.fraction;
		return a.action < b.action;
	});
	for (const Candidate& candidate : candidates) {
		if (candidate.increment == observed_increment) {
			Translation result;
			result.lower_action = result.upper_action = result.sampled_action = candidate.action;
			return result;
		}
	}

	double x = static_cast<double>(observed_increment) / pot_after_call;
	if (x <= candidates.front().fraction) {
		Translation result;
		result.lower_action = result.upper_action = result.sampled_action = candidates.front().action;
		result.exact = false;
		return result;
	}
	if (x >= candidates.back().fraction) {
		Translation result;
		result.lower_action = result.upper_action = result.sampled_action = candidates.back().action;
		result.exact = false;
		return result;
	}
	auto upper = std::upper_bound(candidates.begin(), candidates.end(), x,
		[](double value, const Candidate& candidate) { return value < candidate.fraction; });
	const Candidate& hi = *upper;
	const Candidate& lo = *(upper - 1);
	Translation result;
	result.lower_action = lo.action;
	result.upper_action = hi.action;
	result.lower_probability = RealtimeSearch::pseudo_harmonic_prob_lower(lo.fraction, hi.fraction, x);
	result.sampled_action = RealtimeSearch::randomized_pseudo_harmonic(lo.fraction, hi.fraction, x, rng)
		? lo.action : hi.action;
	result.exact = false;
	return result;
}

inline double interpolated_probability(
	const Translation& translation,
	double lower_action_probability,
	double upper_action_probability)
{
	return translation.lower_probability * lower_action_probability +
		(1.0 - translation.lower_probability) * upper_action_probability;
}

} // namespace BlueprintActionTranslation
