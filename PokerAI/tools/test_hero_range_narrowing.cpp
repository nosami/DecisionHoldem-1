// Focused production-code test for symmetric public range tracking.
#include "dh_native_ai.cpp"

#include <cmath>
#include <cstdio>

namespace {

double weight_sum(const std::vector<WeightedHand>& range) {
	double sum = 0.0;
	for (const auto& h : range) sum += h.weight;
	return sum;
}

double max_weight_change(const std::vector<double>& before,
	const std::vector<WeightedHand>& after) {
	double result = 0.0;
	for (size_t i = 0; i < before.size(); ++i)
		result = std::max(result, std::fabs(before[i] - after[i].weight));
	return result;
}

} // namespace

int main() {
	restart_game(0, 12, 25);
	if (g.hero_range.size() != 1326 || g.villain_range.size() != 1225) {
		std::fprintf(stderr, "unexpected initial range sizes: hero=%zu villain=%zu\n",
			g.hero_range.size(), g.villain_range.size());
		return 1;
	}

	std::vector<double> preflop_before;
	for (const auto& h : g.hero_range) preflop_before.push_back(h.weight);
	if (!narrow_range_preflop(g.hero_range, 2, "hero-test")) {
		std::fprintf(stderr, "hero preflop narrowing failed\n");
		return 1;
	}
	if (std::fabs(weight_sum(g.hero_range) - 1.0) > 1e-9 ||
		max_weight_change(preflop_before, g.hero_range) <= 1e-12) {
		std::fprintf(stderr, "hero preflop range was not narrowed and normalized\n");
		return 1;
	}

	char flop[3] = {5, 18, 33};
	Next_stage(1, flop);
	if (g.hero_range.size() != 1176 || g.villain_range.size() != 1081) {
		std::fprintf(stderr, "unexpected flop range sizes: hero=%zu villain=%zu\n",
			g.hero_range.size(), g.villain_range.size());
		return 1;
	}

	auto policy = std::make_shared<IndexedBlueprint::NodePolicy>();
	policy->bucket_count = 50000;
	policy->actions = {'l', 'n'};
	policy->probabilities.resize(static_cast<size_t>(policy->bucket_count) * 2);
	for (uint32_t bucket = 0; bucket < policy->bucket_count; ++bucket) {
		double call = 0.1 + 0.8 * static_cast<double>(bucket % 101) / 100.0;
		policy->probabilities[static_cast<size_t>(bucket) * 2] = call;
		policy->probabilities[static_cast<size_t>(bucket) * 2 + 1] = 1.0 - call;
	}
	BlueprintActionTranslation::Translation translation;
	translation.lower_action = translation.upper_action = translation.sampled_action = 'l';
	std::vector<double> flop_before;
	for (const auto& h : g.hero_range) flop_before.push_back(h.weight);
	apply_direct_blueprint_likelihood(g.hero_range, policy, translation, "hero-test");
	if (std::fabs(weight_sum(g.hero_range) - 1.0) > 1e-9 ||
		max_weight_change(flop_before, g.hero_range) <= 1e-12) {
		std::fprintf(stderr, "hero postflop range was not narrowed and normalized\n");
		return 1;
	}

	size_t actual = find_hand_index(g.hero_range, 12, 25);
	if (g.hero_range[actual].c1 != 12 || g.hero_range[actual].c2 != 25) {
		std::fprintf(stderr, "actual hero hand index is incorrect\n");
		return 1;
	}

	std::vector<WeightedHand> collapsed = {
		{5, 6, 1.0},
		{7, 8, 0.0},
	};
	prune_range_for_board(collapsed, "collapse-test", false);
	if (collapsed.size() != 1 || collapsed[0].c1 != 7 ||
		std::fabs(collapsed[0].weight - 1.0) > 1e-12) {
		std::fprintf(stderr, "zero-mass board pruning did not restore a valid prior\n");
		return 1;
	}

	std::printf("hero range narrowing checks passed\n");
	return 0;
}
