#include "../tree/BlueprintActionTranslation.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <random>
#include <vector>

using namespace BlueprintActionTranslation;

int main() {
	BettingContext c;
	c.total_pot = 1000;
	c.max_commitment = 500;
	c.actor_commitment = 300;
	c.actor_stack = 100000;
	c.last_raise = 0;
	std::vector<unsigned char> actions = {'d', 'l', 3, 1, 2, 4, 'n'};
	std::mt19937_64 rng(7);

	assert(translate(actions, Kind::Fold, c, 0, rng).sampled_action == 'd');
	assert(translate(actions, Kind::Call, c, 0, rng).sampled_action == 'l');
	assert(translate(actions, Kind::AllIn, c, 0, rng).sampled_action == 'n');

	int p = c.total_pot + c.max_commitment - c.actor_commitment;
	int exact_total = c.max_commitment + raise_increment(p, 2);
	Translation exact = translate(actions, Kind::Raise, c, exact_total, rng);
	assert(exact.exact && exact.sampled_action == 2);
	assert(raise_increment(12345, 3) == (12345 / 400) * 100);

	Translation low = translate(actions, Kind::Raise, c, c.max_commitment + 1, rng);
	assert(!low.exact && low.sampled_action == 3);
	Translation high = translate(actions, Kind::Raise, c, c.max_commitment + p * 10, rng);
	assert(!high.exact && high.sampled_action == 4);

	int between = (raise_increment(p, 1) + raise_increment(p, 2)) / 2;
	std::mt19937_64 rng1(12345), rng2(12345);
	Translation bracket1 = translate(actions, Kind::Raise, c, c.max_commitment + between, rng1);
	Translation bracket2 = translate(actions, Kind::Raise, c, c.max_commitment + between, rng2);
	assert(bracket1.lower_action == 1 && bracket1.upper_action == 2);
	assert(bracket1.sampled_action == bracket2.sampled_action);
	double x = static_cast<double>(between) / p;
	double expected = RealtimeSearch::pseudo_harmonic_prob_lower(
		static_cast<double>(raise_increment(p, 1)) / p,
		static_cast<double>(raise_increment(p, 2)) / p, x);
	assert(std::fabs(bracket1.lower_probability - expected) < 1e-15);
	assert(std::fabs(interpolated_probability(bracket1, 0.2, 0.8) -
		(expected * 0.2 + (1.0 - expected) * 0.8)) < 1e-15);

	std::vector<unsigned char> no_call = {'d', 2, 'n'};
	assert([](auto fn) { try { fn(); } catch (...) { return true; } return false; }(
		[&] { translate(no_call, Kind::Call, c, 0, rng); }));
	BettingContext short_stack = c;
	short_stack.actor_stack = 1300;
	assert([](auto fn) { try { fn(); } catch (...) { return true; } return false; }(
		[&] { translate(actions, Kind::Raise, short_stack, c.max_commitment + 500, rng); }));

	std::cout << "PASS: blueprint action translation tests\n";
	return 0;
}
