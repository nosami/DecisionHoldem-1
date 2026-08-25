#!/usr/bin/env python3
"""Wall-clock timing benchmark for depth_limited_search_demo.py's resolve().

WHAT THIS MEASURES
-------------------
"How long does it take to make a flop (or turn/river) decision, given the
opponent's range is an explicit list of N possible 2-card holdings (each with
its own weight), for both players?" This times ONLY the CFR resolve loop
(the actual "per-decision" real-time cost), for a configurable holdings-count
per side and iteration budget, on this demo's toy game.

`--holdings` maps 1:1 onto the demo's `N_CLASSES`: each "class" here stands in
for one specific, distinct hole-card combo with its own supplied probability
weight (the `cardsweight[]`/`external_cardid[]` design evidenced in the real,
unused `Bulid_Tree.h`/`Visualize_Tree.h` -- see BUILD_NOTES.md) -- NOT a
clustered bucket averaging many combos together. So `--holdings 30` means
"villain's range is exactly 30 possible specific 2-card holdings."

HOW TO READ THE RESULT -- AND WHY IT IS ONLY A ROUGH, TOY-GAME LOWER BOUND
---------------------------------------------------------------------------
This is a genuine wall-clock measurement, not a fabricated number -- but it
measures a tiny synthetic game (a handful of fold/call/bet/allin nodes per
street) in pure, unoptimized, single-threaded, interpreted Python. The real
DecisionHoldem system differs in ways that could move the real number by
large, currently-unknown factors in EITHER direction:
  * Real bet-sizing abstractions have far more actions/raise-rounds per
    street than this toy game's ~2-3, so a real subgame tree likely has many
    more information sets per iteration (slower).
  * Real per-node work also includes real cluster/hand-strength table
    lookups (`sevencards_strength.bin` etc.); this toy game uses trivial
    O(1) ordinal comparisons instead (probably comparable cost in practice,
    since the real lookups are also O(1) array/hash reads, but not verified).
  * Compiled C++ is typically ~20-100x faster per elementary operation than
    interpreted Python (faster); the real (missing) implementation's actual
    threading, if any, is unknown.
  * `Depth_limit_Search.h` -- the code that would actually do this in the
    real system -- does not exist in this repository (see BUILD_NOTES.md
    section 2.1), so there is no real implementation to measure directly;
    this number can only ever be an extrapolation, never a verification.

Run examples:
    python3 PokerAI/tools/bench_resolve_timing.py --holdings 30 --iterations 6000 --street flop
    python3 PokerAI/tools/bench_resolve_timing.py --holdings 30 --iterations 10000 --street turn
"""
import argparse
import importlib.util
import os
import time

STREET_IDX = {"flop": 0, "turn": 1, "river": 2}


def _load_demo_module(n_holdings):
    """Import depth_limited_search_demo.py with N_CLASSES patched to
    `n_holdings`, without touching or requiring edits to the real file."""
    here = os.path.dirname(os.path.abspath(__file__))
    src_path = os.path.join(here, "depth_limited_search_demo.py")
    with open(src_path) as f:
        src = f.read()
    marker = "N_CLASSES = 6"
    if marker not in src:
        raise RuntimeError("depth_limited_search_demo.py's N_CLASSES constant "
                            "has moved/changed; update this benchmark's patch string.")
    src = src.replace(marker, f"N_CLASSES = {n_holdings}", 1)
    # Prevent the module's own __main__ demo from running when exec'd.
    src = src.replace('if __name__ == "__main__":\n    main()', "")

    spec = importlib.util.spec_from_file_location("dls_bench", src_path)
    mod = importlib.util.module_from_spec(spec)
    exec(compile(src, src_path, "exec"), mod.__dict__)
    return mod


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--holdings", type=int, default=30,
                     help="number of specific, individually-weighted 2-card holdings "
                          "per player (default: 30)")
    ap.add_argument("--iterations", type=int, default=6000,
                     help="real-time resolve iteration budget (default: 6000, matching "
                          "the paper's stated flop/preflop off-tree budget)")
    ap.add_argument("--street", choices=["flop", "turn", "river"], default="flop",
                     help="street to resolve at (default: flop)")
    ap.add_argument("--blueprint-iterations", type=int, default=1500,
                     help="one-time offline blueprint training iterations, needed only "
                          "to supply this benchmark's leaf values (default: 1500)")
    args = ap.parse_args()

    street_idx = STREET_IDX[args.street]
    mod = _load_demo_module(args.holdings)

    t0 = time.time()
    bp_nodes = mod.train_blueprint(iterations=args.blueprint_iterations)
    t1 = time.time()

    leaf_fn = mod.blueprint_leaf_value_fn(bp_nodes)
    t2 = time.time()
    resolve_nodes = mod.build_resolve_nodes(street_idx=street_idx, offtree_extra=None)
    mod.warm_start_from_blueprint(resolve_nodes, bp_nodes, street_idx=street_idx)
    t3 = time.time()

    hero_range = [1.0] * args.holdings
    villain_range = [1.0] * args.holdings

    t4 = time.time()
    mod.resolve(resolve_nodes, hero_range, villain_range, street_idx,
                iterations=args.iterations, leaf_fn=leaf_fn, offtree_extra=None)
    t5 = time.time()

    print(f"holdings/side={args.holdings}  street={args.street}  iterations={args.iterations}")
    print(f"  one-time blueprint pretraining (not a per-decision cost): {t1 - t0:.2f}s "
          f"({args.blueprint_iterations} iterations)")
    print(f"  tree construction + blueprint warm-start (per-decision, but tiny/fixed cost "
          f"in this toy game): {t3 - t2:.4f}s")
    print(f"  CFR RESOLVE LOOP (the actual per-decision cost): {t5 - t4:.2f}s "
          f"({(t5 - t4) / args.iterations * 1000:.3f} ms/iteration)")
    print()
    print("This is a toy-game, single-threaded Python measurement -- see this script's "
          "module docstring and BUILD_NOTES.md for why it is only a rough order-of-magnitude "
          "lower bound, not a measurement of the real (missing) DecisionHoldem search.")


if __name__ == "__main__":
    main()
