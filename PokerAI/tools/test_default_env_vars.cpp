//###############################################################################
//   test_default_env_vars.cpp -- REAL validation (not assumed) of
//   BUILD_NOTES.md section 52's "make the six DH_* env vars default to the
//   live SkyPoker bridge session's values" change. #includes dh_native_ai.cpp
//   directly -- same established pattern as this repo's other tools/test_*.cpp
//   files (e.g. test_bet_size_narrowing.cpp) -- giving this test direct access
//   to the real, unmodified production getter functions rather than a
//   reimplementation.
//
//   For each of the six DH_* environment variables, this:
//     1. unsetenv()s it and confirms the corresponding getter now returns/
//        uses the new hardcoded default (instead of the old default).
//     2. setenv()s it to an explicit value (a different path, or "0" for the
//        two booleans, or a different mode string for DH_TEXASSOLVER_FALLBACK)
//        and confirms that explicit value still wins over the new default --
//        i.e. the override-via-environment-variable behavior is fully intact.
//
//   The six getters under test (all defined in dh_native_ai.cpp except
//   texassolver_bridge::trigger_mode(), defined in TexasSolverBridge.h and
//   pulled in transitively):
//     - direct_blueprint_enabled()       (DH_DIRECT_BLUEPRINT)
//     - direct_blueprint_path()          (DH_BLUEPRINT_PATH)
//     - direct_blueprint_index_path()    (DH_BLUEPRINT_INDEX)
//     - river_split_dir()                (DH_RIVER_SPLIT_DIR)
//     - dh_verbose_enabled()             (DH_VERBOSE_STRATEGY)
//     - texassolver_bridge::trigger_mode() (DH_TEXASSOLVER_FALLBACK)
//
//   These are pure, stateless (no caching) reads of std::getenv() each call,
//   so they can be checked directly and repeatedly in isolation, in any
//   order, without needing to drive a full hand through restart_game() et al.
//
//   BUILD (run from PokerAI/):
//     g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o tools/test_default_env_vars tools/test_default_env_vars.cpp
//   RUN (from PokerAI/):
//     ./tools/test_default_env_vars
//###############################################################################
#include "dh_native_ai.cpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

bool g_all_ok = true;

void check(bool cond, const char* description) {
	if (cond) {
		std::printf("PASS: %s\n", description);
	} else {
		std::printf("FAIL: %s\n", description);
		g_all_ok = false;
	}
}

void check_str(const std::string& actual, const std::string& expected, const char* description) {
	if (actual == expected) {
		std::printf("PASS: %s (got \"%s\")\n", description, actual.c_str());
	} else {
		std::printf("FAIL: %s (expected \"%s\", got \"%s\")\n",
			description, expected.c_str(), actual.c_str());
		g_all_ok = false;
	}
}

} // namespace

int main() {
	// -------------------------------------------------------------------
	// 1. DH_DIRECT_BLUEPRINT -- default ON (was default OFF).
	// -------------------------------------------------------------------
	unsetenv("DH_DIRECT_BLUEPRINT");
	check(direct_blueprint_enabled() == true,
		"DH_DIRECT_BLUEPRINT unset -> direct_blueprint_enabled() defaults to true");

	setenv("DH_DIRECT_BLUEPRINT", "0", 1);
	check(direct_blueprint_enabled() == false,
		"DH_DIRECT_BLUEPRINT=0 explicit override -> direct_blueprint_enabled() is false");

	setenv("DH_DIRECT_BLUEPRINT", "1", 1);
	check(direct_blueprint_enabled() == true,
		"DH_DIRECT_BLUEPRINT=1 explicit override -> direct_blueprint_enabled() is true");

	setenv("DH_DIRECT_BLUEPRINT", "false", 1);
	check(direct_blueprint_enabled() == false,
		"DH_DIRECT_BLUEPRINT=false explicit override -> direct_blueprint_enabled() is false "
		"(unrecognized-as-true token, same pre-existing semantics as before this change)");
	unsetenv("DH_DIRECT_BLUEPRINT");

	// -------------------------------------------------------------------
	// 2. DH_BLUEPRINT_PATH -- default is this user's real local blueprint file
	//    (was "cluster/blueprint_strategy.dat").
	// -------------------------------------------------------------------
	unsetenv("DH_BLUEPRINT_PATH");
	check_str(direct_blueprint_path(), "/Users/jason/dh_local_data/blueprint_stgy.dat",
		"DH_BLUEPRINT_PATH unset -> direct_blueprint_path() defaults to the local blueprint file");

	setenv("DH_BLUEPRINT_PATH", "/tmp/example_override_blueprint.dat", 1);
	check_str(direct_blueprint_path(), "/tmp/example_override_blueprint.dat",
		"DH_BLUEPRINT_PATH explicit override -> direct_blueprint_path() honors it");
	unsetenv("DH_BLUEPRINT_PATH");

	// -------------------------------------------------------------------
	// 3. DH_BLUEPRINT_INDEX -- still derived as source + ".idx" when unset
	//    (a deliberate, unchanged design -- see the code comment at its
	//    definition); with DH_BLUEPRINT_PATH also unset/default, this
	//    resolves to the local blueprint file's real sidecar index.
	// -------------------------------------------------------------------
	unsetenv("DH_BLUEPRINT_PATH");
	unsetenv("DH_BLUEPRINT_INDEX");
	check_str(direct_blueprint_index_path(direct_blueprint_path()),
		"/Users/jason/dh_local_data/blueprint_stgy.dat.idx",
		"DH_BLUEPRINT_INDEX unset (DH_BLUEPRINT_PATH also unset) -> "
		"direct_blueprint_index_path() defaults to the local blueprint index file");

	// DH_BLUEPRINT_PATH overridden, DH_BLUEPRINT_INDEX left unset: the
	// index should still sensibly follow the overridden source, NOT fall
	// back to this user's personal index file.
	setenv("DH_BLUEPRINT_PATH", "/tmp/example_override_blueprint.dat", 1);
	check_str(direct_blueprint_index_path(direct_blueprint_path()),
		"/tmp/example_override_blueprint.dat.idx",
		"DH_BLUEPRINT_INDEX unset but DH_BLUEPRINT_PATH overridden -> "
		"direct_blueprint_index_path() derives from the overridden source, not the personal default");
	unsetenv("DH_BLUEPRINT_PATH");

	setenv("DH_BLUEPRINT_INDEX", "/tmp/example_override_blueprint.dat.idx", 1);
	check_str(direct_blueprint_index_path("/Users/jason/dh_local_data/blueprint_stgy.dat"),
		"/tmp/example_override_blueprint.dat.idx",
		"DH_BLUEPRINT_INDEX explicit override -> direct_blueprint_index_path() honors it "
		"regardless of source");
	unsetenv("DH_BLUEPRINT_INDEX");

	// -------------------------------------------------------------------
	// 4. DH_RIVER_SPLIT_DIR -- default is this user's real split-file
	//    directory (was empty/unset, i.e. the feature was off).
	// -------------------------------------------------------------------
	unsetenv("DH_RIVER_SPLIT_DIR");
	check_str(river_split_dir(), "/Users/jason/dh_local_data/river_cluster_split",
		"DH_RIVER_SPLIT_DIR unset -> river_split_dir() defaults to the local split-file directory");

	setenv("DH_RIVER_SPLIT_DIR", "/tmp/example_override_river_split", 1);
	check_str(river_split_dir(), "/tmp/example_override_river_split",
		"DH_RIVER_SPLIT_DIR explicit override -> river_split_dir() honors it");
	unsetenv("DH_RIVER_SPLIT_DIR");

	// -------------------------------------------------------------------
	// 5. DH_VERBOSE_STRATEGY -- default ON (was default OFF).
	// -------------------------------------------------------------------
	unsetenv("DH_VERBOSE_STRATEGY");
	check(dh_verbose_enabled() == true,
		"DH_VERBOSE_STRATEGY unset -> dh_verbose_enabled() defaults to true");

	setenv("DH_VERBOSE_STRATEGY", "0", 1);
	check(dh_verbose_enabled() == false,
		"DH_VERBOSE_STRATEGY=0 explicit override -> dh_verbose_enabled() is false");

	setenv("DH_VERBOSE_STRATEGY", "1", 1);
	check(dh_verbose_enabled() == true,
		"DH_VERBOSE_STRATEGY=1 explicit override -> dh_verbose_enabled() is true");

	setenv("DH_VERBOSE_STRATEGY", "", 1);
	check(dh_verbose_enabled() == false,
		"DH_VERBOSE_STRATEGY=\"\" (explicitly set but empty) -> dh_verbose_enabled() is false, "
		"same pre-existing semantics as before this change");
	unsetenv("DH_VERBOSE_STRATEGY");

	// -------------------------------------------------------------------
	// 6. DH_TEXASSOLVER_FALLBACK -- default "force" (was default "auto").
	// -------------------------------------------------------------------
	unsetenv("DH_TEXASSOLVER_FALLBACK");
	check(texassolver_bridge::trigger_mode() == texassolver_bridge::TriggerMode::FORCE,
		"DH_TEXASSOLVER_FALLBACK unset -> trigger_mode() defaults to FORCE");

	setenv("DH_TEXASSOLVER_FALLBACK", "auto", 1);
	check(texassolver_bridge::trigger_mode() == texassolver_bridge::TriggerMode::AUTO,
		"DH_TEXASSOLVER_FALLBACK=auto explicit override -> trigger_mode() is AUTO");

	setenv("DH_TEXASSOLVER_FALLBACK", "off", 1);
	check(texassolver_bridge::trigger_mode() == texassolver_bridge::TriggerMode::OFF,
		"DH_TEXASSOLVER_FALLBACK=off explicit override -> trigger_mode() is OFF");

	setenv("DH_TEXASSOLVER_FALLBACK", "force", 1);
	check(texassolver_bridge::trigger_mode() == texassolver_bridge::TriggerMode::FORCE,
		"DH_TEXASSOLVER_FALLBACK=force explicit override -> trigger_mode() is FORCE");
	unsetenv("DH_TEXASSOLVER_FALLBACK");

	std::printf(g_all_ok ? "\nALL CHECKS PASSED\n" : "\nSOME CHECKS FAILED\n");
	return g_all_ok ? 0 : 1;
}
