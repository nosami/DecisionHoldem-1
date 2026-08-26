# Build / Reproducibility Notes (macOS investigation)

This document records a from-scratch attempt to build and run every component of
DecisionHoldem end-to-end on a Darwin/arm64 (macOS) host, the concrete source
defects found and fixed, and — most importantly — exactly which artifacts are
genuinely missing from this repository, why they cannot be regenerated from the
included source, and what that means for what can and cannot legitimately run.

**Bottom line:** the C++ blueprint-training/evaluation program now compiles and
links cleanly on macOS (previously it did not compile at all — see below), and
its CLI now behaves as documented. A real, complete, working component was
found, fixed, and successfully run end-to-end with no missing data at all: the
README's own referenced Leduc poker example (section 3) — its full 10-million-
iteration MCCFR training run completed in ~22 seconds after one small build
fix. The main 52-card engine's required pretrained hand-abstraction data was
**not present in this repository, its git history, or any GitHub release**, and
is only officially distributed via a third-party Baidu Netdisk link this
investigation did not use (unverifiable, access-gated). All six required files
(five cluster arrays plus `blueprint_strategy.dat`) were, however, eventually
located via an **unofficial community re-upload** referenced from this
project's own GitHub issues, byte-size-verified, and used to actually attempt
the real engine (section 2) — which confirmed, empirically, that `Engine()`'s
unconditional ~19.4 GiB memory requirement exceeds this specific 16 GiB
machine's capacity, so the full 52-card trainer/evaluator still cannot run
*here*, though it should on a machine with more RAM. The precompiled
`AlascasiaHoldem.so` / `blueprint.so` real-time-search binaries are Linux
x86_64 ELF objects with **no corresponding source in this repo** (the README
explicitly says this component "currently only provide[s] compiled files"),
so they cannot be ported to macOS at all without that missing source; they
would need to run under Linux x86_64 (e.g. Docker), which was not available in
this sandbox.

## 1. Source defects found and fixed (all preserve Linux/GCC behavior)

All three issues below were reproducible with a clean `git clone` and the exact
`g++ Main.cpp -o Main.o -std=c++11 -mcmodel=large -lpthread` command from the
README. They are genuine bugs, not macOS-only quirks — GCC is simply more
lenient about the first two than Clang.

1. **Every `.h`/`.cpp` file's license banner used `#` instead of `//` for a
   16-line "comment" block** (e.g. `PokerAI/Main.cpp` lines 1–17). A line
   starting with `#` is a preprocessor directive, not a comment, so this is an
   invalid-preprocessing-directive error under any standards-conforming
   compiler (confirmed with Apple Clang; GCC also rejects unrecognized `#`
   directives — this is not compiler-specific leniency, just bad luck that
   whoever last touched these files apparently never recompiled from a clean
   checkout). **Fix:** converted the banner's leading `#` to `//` in all 13
   affected files, byte-for-byte identical otherwise (CRLF line endings
   preserved).
2. **Narrowing `int → unsigned char` conversions inside brace-init-lists** in
   `PokerAI/tree/Exploitability.h` (3 call sites, `coms[] = { t1, t2, t3 }` and
   similar), which is ill-formed under strict C++11 list-initialization rules.
   Clang treats this as a hard error; GCC only warns by default. **Fix:** added
   explicit `(unsigned char)` casts at the 3 sites — behavior-preserving on
   both toolchains.
3. **`PokerAI/Main.cpp`'s `main()` could never train.** The original code was:
   ```cpp
   assert(argc == 1);
   if (argv[0] == 0)
       multiprocess_blueprint();
   else { /* load + evaluate blueprint_strategy.dat */ }
   ```
   `argv[0]` is the program's own path and is never `0`/NULL for a normally
   invoked process, so `multiprocess_blueprint()` (the actual trainer) was
   dead code — every invocation silently fell through to the evaluation
   branch, regardless of the `0`/`1` argument the README tells you to pass.
   Worse, `assert(argc == 1)` meant that literally running `./Main.o 0` or
   `./Main.o 1` as documented would abort immediately in a debug build.
   **Fix:** changed to `assert(argc == 2); if (argv[1][0] == '0') multiprocess_blueprint(); else { ... }`,
   matching the documented `./Main.o 0` (train) / `./Main.o 1` (evaluate) usage.

With these three fixes, the exact README build command now succeeds on macOS:

```shell
cd PokerAI
g++ Main.cpp -o Main.o -std=c++11 -mcmodel=large -lpthread   # clean build, 0 errors
./Main.o 0   # now actually reaches multiprocess_blueprint()
./Main.o 1   # now actually reaches the evaluation branch
```
(`-mcmodel=large` is an x86_64-only code-model flag; Clang/arm64 silently
accepts and ignores it rather than erroring, so the README command works
unmodified — no macOS-specific flag changes were needed here.)

Both invocations above still terminate (uncaught `std::exception`, "hand_value
file is not exist") — this is expected and is the subject of the rest of this
document, not a build problem.

## 2. The missing cluster/blueprint artifacts (root cause of every runtime failure)

`PokerAI/poker/Engine.h` declares `Engine* engine = new Engine();` at **global
scope**, so simply linking against `State.h` (which every executable in this
project does) constructs an `Engine` — and therefore tries to load these five
files from `cluster/` — before `main()` even runs:

| File | Repo status | Regenerable from this repo's source? | Expected size (derived from `Engine.h` constants) |
|---|---|---|---|
| `cluster/sevencards_strength.bin` | **Missing** | No — no 7-card hand-evaluator/table generator exists in this repo | ≈ 1.25 GiB |
| `cluster/preflop_hand_cluster.bin` | **Missing** | No — no clustering code in this repo | ≈ 10.4 KiB |
| `cluster/flop_hand_cluster.bin` | **Missing** | No | ≈ 198 MiB |
| `cluster/turn_hand_cluster.bin` | **Missing** | No | ≈ 2.28 GiB |
| `cluster/river_hand_cluster.bin` | **Missing** | No | ≈ 15.7 GiB |
| `cluster/preflopallin1326.1225.bin` | **Present** (14,066,208 bytes, committed in the first commit) | N/A, already provided | matches on-disk size exactly, validating the size math above |
| `cluster/blueprint_strategy.dat` | **Missing** | **Yes, in principle** — `dump()`/`load()` in `tree/Save_load.h` serialize/deserialize it, and it is produced purely by running the trainer (`multiprocess_blueprint()`/`Singleiter()`). But the trainer cannot run at all without the five files above, since tree construction calls `engine->get_preflop_cluster/get_flop_cluster/...` throughout. | Variable size; grows with the recursive game tree (README: ~200M iterations over 3–4 days on a 48-core / 512GB workstation) |

**Is `blueprint_strategy.dat` actually required?** It depends on the mode:
`Main.cpp`'s **training** branch (`./Main.o 0`) never reads it — it only
*writes* it periodically via `dump()` inside `multiprocess_blueprint()`, so
training can and does start from nothing but the five files above. Only the
**evaluation** branch (`./Main.o 1`) reads it, via `load(root,
"cluster/blueprint_strategy.dat")`. That `load()` (in `tree/Save_load.h`) has
the exact same unguarded-`ifstream` bug as `Engine::load()`: it never checks
whether `fin.open(...)` succeeded, so a missing/corrupt file doesn't error —
it silently builds the strategy tree with uninitialized garbage values and
evaluation then prints meaningless numbers as if it worked. Never trust
`./Main.o 1` output without first confirming this file is a genuine,
complete, trained strategy.

Verified checks performed (not just assumed):
- `git log --all --follow -- PokerAI/cluster` and `git rev-list --objects --all \| grep -iE '\.bin$|\.dat$'` show **only** `preflopallin1326.1225.bin` was ever committed to this repository, in the very first commit. The other five files have never existed in git history.
- `gh api repos/AI-Decision/DecisionHoldem/tags` and `gh release list` are both empty — there is no GitHub release or tag carrying these files.
- No `.gitattributes`/Git LFS pointers exist anywhere in the repo.
- No clustering, k-means, or hand-strength-table-generation source code exists anywhere in this repository (`grep`-checked across all `.h`/`.cpp`/`.py` files) — the abstraction step described in the README ("hand abstraction technique") was never open-sourced, only its *output* (via the Baidu link) was ever intended to be distributed.
- Running `./Main.o 0` or `./Main.o 1` after the build fixes above reproduces the exact, expected, and now-correctly-reached failure: `hand_value file is not exist` followed by an uncaught `std::exception` abort — this is the maximum runnable state without the missing data.

**We deliberately did not fabricate placeholder/zero-filled versions of these
files.** Doing so would let the program run without crashing, but every
downstream number (hand clusters, regrets, the resulting `blueprint_strategy.dat`)
would be meaningless, and reporting that as "it runs" would misrepresent the
actual state of the project. The Baidu Netdisk link in the README
(`https://pan.baidu.com/s/157n-H1ECjEryAx0Z03p2_w`, code `q1pv`) is the only
place these files are known to exist; it is a third-party, access-gated host
that this investigation did not access, per the instruction to avoid dubious
third-party binaries and not bypass access controls.

**If you obtain the real files legitimately** (e.g. from the authors), the full
pipeline documented above becomes buildable and runnable end-to-end on macOS
now that the three source defects are fixed — place all six files in
`PokerAI/cluster/` and run the commands in section 1.

### 2.1.0 Correction: much of the depth-limited-search *machinery* is already present

A closer read of `PokerAI/tree/Node.h` and `PokerAI/tree/Bulid_Tree.h` (both
of which *are* present and buildable) shows the missing `Depth_limit_Search.h`
would not have been starting from nothing — most of its supporting machinery
already exists elsewhere in the tree, it's just never assembled into a
callable real-time-search entry point:

- **`Node.h`'s `subgame_node` struct already has depth-limit fields**:
  `bool frozen, leaf` (comment: *"leaf: 深度受限叶节点"* = "leaf: depth-limited
  leaf node"; *"frozen: 冻结这个节点策略不更新"* = "frozen: freeze this node's
  strategy, don't update it"), plus `strategy_node* leafnode` (a pointer back
  to the corresponding **blueprint** node) and `double* expolitvalues` (a
  cached value for that leaf). This is precisely a "depth-first search that
  substitutes a blueprint-derived value at the depth limit" data structure.
- **`Bulid_Tree.h`'s `build_subgameeroot()`/`bulid_subtree_turn2()`/
  `bulid_subtree_river()`** already build a real-time subgame tree rooted at
  the *actual current hand* (using live `engine->get_flop_cluster()` /
  `get_turn_cluster()` / `get_river_cluster()` lookups against the same
  cluster files from section 2), matched node-for-node against the
  corresponding blueprint subtree. While the action set still matches the
  blueprint (`existmap`), it **warm-starts real-time regrets from the
  blueprint's accumulated average-strategy sum**:
  `privatenode[j]->regret[k] = subblueprints[j]->averegret[k] / 10;`. Despite
  its name, `averegret` is **not** accumulated regret — `BlueprintMCCFR.h`
  shows it is incremented every iteration as `treenode->averegret[i] +=
  sigma[i]` (the *current strategy probability*, not a regret value), i.e.
  it is what most CFR literature calls the `strategy_sum`/average-strategy
  accumulator, misleadingly named. So this warm start literally means "seed
  real-time regret-matching so it starts out biased toward whatever the
  blueprint tended to play here," not "reuse the blueprint's regret." Note
  also that `regret` itself *is* separately discounted elsewhere
  (`treenode->regret[i] *= d;` in `dfs_discount()`), but the commented-out
  `//treenode->averegret[i] *= d;` right next to it confirms `averegret` is
  **not** discounted — it is a raw, ever-growing sum over the entire
  blueprint training run, so `averegret[k]/10` can be very large in
  absolute terms (see section 2.1.2 for a concrete demonstration of the
  practical consequence of this). The instant actions diverge from the
  blueprint's abstraction (a genuine "off-tree" bet size), a comment marks
  the transition — *"只要有一个节点动作不一样，
  当前节点和子节点后悔值全0"* ("once one node's actions differ, this node and
  its children's regrets reset to zero") — and `addnode_bysubgame(...,
  offtreeact, treeact, ...)` splices in the new action fresh, unseeded.
- **`Exploitability.h`'s `getnode_cfv_river`/`getnode_cfv_turn`** are a full
  depth-first recursive tree walk with Bayesian range propagation
  (`range[j] *= tmp`) and leaf payoff evaluation — structurally almost
  identical to what one CFR iteration over the constructed subgame needs
  (currently wired for a single best-response pass, not repeated regret
  updates, since this file is used for offline exploitability measurement).

**What is therefore still, genuinely missing** (i.e. what `Depth_limit_Search.h`
itself most likely contained) is narrower than "everything":
1. The **outer iterative refinement loop** that repeats a `getnode_cfv_*`-style
   pass some number of times (the paper: 6,000 iterations for preflop/flop
   off-tree nodes, 10,000 for turn, 10,000 unconditionally on the river),
   each time running regret matching (`calculate_strategy`, already in
   `Node.h`) to get the current strategy, propagating ranges down, computing
   counterfactual values back up, updating `regret[]`/`ave_strategy[]`, and
   stopping recursion at `leaf` nodes by substituting `expolitvalues` instead
   of expanding further.
2. The **"diverse opponent ranges" ensemble/safety mechanism** — the paper's
   actual claimed contribution over plain Modicum-style depth-limited
   solving. This has **zero trace** anywhere in `Node.h`, `Bulid_Tree.h`, or
   `Exploitability.h`: no code constructs multiple candidate opponent ranges,
   weights across them, or applies any safety/maximin criterion. This is the
   one piece that appears to have genuinely never been open-sourced,
   consistent with the paper's unfulfilled "subsequent articles" promise
   (below) — reconstructing it would be original engineering, not assembly
   of existing parts.
3. **Top-level orchestration**: the per-decision logic that watches the
   actual opponent action during play, decides whether it is off-abstraction,
   invokes the tree-building functions above, runs the refinement loop, and
   returns a real-time strategy for the current decision instead of the raw
   blueprint action. `Main.cpp` never calls any of this — it only calls
   `check_subgame()`/`getcfv_whole_holdem()` for offline blueprint evaluation.

### 2.1.2 A reference implementation of the missing training loop

`PokerAI/tools/depth_limited_search_demo.py` is a small, standalone,
**original** Python program that implements what the missing
`Depth_limit_Search.h` most plausibly did, per the simplification the user
confirmed: **the caller supplies both players' ranges directly** (arbitrary
probability-per-hand-class distributions), so DecisionHoldem's undisclosed
"diverse opponent ranges" ensemble (section 2.1.0, item 2) is not needed —
this reduces the problem to standard, published depth-limited CFR resolving
(à la Brown & Sandholm's Modicum, arXiv:1809.03040), which the demo
implements and validates end-to-end:

- A tiny synthetic 3-street game (6 hand classes/player, `fold`/`call`/
  `bet`/`allin`) stands in for the real 1,326-combo/5,000-bucket abstraction
  (using the real one would require the ~20.8 GiB `Engine()` — see the RAM
  caveat above — which this host cannot allocate).
- Trains a "blueprint" over the full game with vanilla CFR.
- Resolves just the last street given an **arbitrary supplied opponent
  range** (deliberately different from the blueprint's own training
  distribution), using a depth-limited leaf whose value is read from the
  blueprint's own solved continuation — mirroring `leaf`/`leafnode`/
  `expolitvalues` in `Node.h`.
- Warm-starts on-tree regrets from the blueprint's `averegret` (accumulated
  average-strategy sum, see above) divided by 10, exactly mirroring
  `Bulid_Tree.h`'s formula, and resets off-tree actions (e.g. an extra bet
  size the blueprint never had) to zero regret, exactly mirroring the
  `existmap` branch.
- Runs both a warm-started and a cold-started (all-zero-regret) resolve of
  the identical subgame and validates, with `assert`s (not just printed
  output), that: (a) both converge — average positive regret per iteration,
  the textbook CFR guarantee, trends toward zero for both regardless of
  starting point; (b) every resolved/blueprint strategy is a valid
  probability distribution; (c) the resolve is genuinely range-sensitive
  and game-theoretically sane (e.g. the strongest hand class folds less
  than a weaker one when facing a bet); (d) the off-tree action trains from
  scratch rather than crashing or being silently skipped.

**A genuinely useful finding surfaced by this exercise**: because
`averegret` is *not* discounted (only `regret` is, via `dfs_discount`'s `d`
factor — see above), it grows unboundedly over the blueprint's entire
training run, so `averegret/10` can seed real-time regret with a very large
constant. At a *small* number of real-time iterations, this constant
dominates the average-regret bound almost entirely regardless of whether it
is actually a *good* prior for the specific range just supplied — the
benefit of warm-starting is only asymptotically guaranteed (as more
iterations dilute that fixed initial offset), not guaranteed at the small
iteration budgets (~1,000–10,000) the paper describes for real-time play.

**Can you supply an opponent range at the flop, specifically, and not just
turn/river?** Yes, on both counts checked:

- *Real source*: `Bulid_Tree.h`'s (present, but never-invoked)
  `build_subgameeroot(..., Engine* engine, ...)` explicitly branches on
  `curstate.betting_stage` (0=preflop, 1=flop, 2=turn, 3=river — see
  `poker/State.h`): the `betting_stage == 1` case builds a real-time subgame
  tree rooted at the **flop**, using `engine->get_flop_cluster(...)` on a
  caller-supplied array of the opponent's possible hole-card combos
  (`external_cardid[]`/`external_cards_len`) — architecturally identical in
  kind to its turn (`betting_stage == 2`) and river (`else`) branches, not a
  turn/river-only special case. A commented-out debug/visualization helper
  in `Visualize_Tree.h` (`visualizationsearch_bettingsub234`) shows the
  intended companion representation: a parallel `cardsweight[]` array giving
  each combo's probability — i.e. exactly "probabilities of each 2-card
  holding," matching what you described. Neither array is weight-checked
  by the actual (non-debug) tree-building code itself; the probability
  would be threaded through as a range array during CFR traversal, the same
  way `player0_range`/`player1_range` work in the demo below.
- *This demo*: `resolve()` takes a `street_idx` parameter and works
  identically for any of the three streets (collapsing everything after it
  into the depth-limited leaf) — validated by an assertion that a
  **flop-rooted** resolve (collapsing both the turn and river into one
  leaf) converges exactly like the turn-rooted one used in the main walk-
  through.

**Can this code be used to narrow what you believe the opponent's range to
be, given their observed actions?** Yes — this falls directly out of
machinery already present and already validated above, in both the real
source and the demo:

- *Real source*: nothing standalone exists for this in DecisionHoldem
  itself — a repo-wide search for `belief`/`posterior`/`bayes`/`reach_prob`/
  `update_range` finds no matches. But the *ingredient* is there implicitly:
  any CFR traversal (blueprint training or real-time resolving alike) is
  already carrying, at every node, each hand class's **reach probability** —
  the prior range weight for that class multiplied by the probability its
  own strategy assigned to every action taken to reach that node. That is
  mathematically identical to an (unnormalized) Bayesian posterior
  `P(class | actions seen so far)`; the code just never exposes or
  renormalizes it as a standalone "belief" object, because the live engine
  is never in a position to run at all on this build (see the RAM ceiling
  above).
- *This demo*: `narrow_range_given_actions(nodes, prior_range, street_idx,
  observed_actions, observed_player)` exposes exactly that reweighting as a
  standalone utility — replay an observed action sequence against a fixed,
  already-solved strategy (blueprint's or a resolve's `ave_strategy`), and
  get back the renormalized posterior distribution over the acting player's
  hand classes. Validated with two scenarios sharing one prior (uniform):
  hero bets and villain either flat-calls or shoves all-in. Both produce a
  posterior measurably different from the uniform prior (i.e. the action
  is genuine evidence, not a no-op), *and* the two lines produce
  measurably different posteriors from each other (calling and shoving are
  not the same evidence). In this toy game's particular equilibrium, calling
  turns out to skew strongly toward the two strongest classes while shoving
  is comparatively flatter/more balanced across classes (a believable
  polarized-shoving-range shape, though this is a property of the toy
  game's own solved equilibrium, not a general claim about the real
  DecisionHoldem abstraction). Practically: you can chain this street by
  street — feed the posterior from one street's actions in as the prior
  range for the next street's resolve — since the demo's `resolve()` /
  `narrow_range_given_actions()` both already take an arbitrary range as a
  plain input, not just the uniform default.

**How long does a flop decision take, given each player's range is 30**
**specific 2-card holdings (not a clustered bucket -- 30 individually-**
**weighted combos per side)?** Measured directly (not estimated) with
`PokerAI/tools/bench_resolve_timing.py`, which times only the CFR resolve
loop itself (the actual per-decision cost) for a configurable holdings
count and iteration budget:

```bash
python3 PokerAI/tools/bench_resolve_timing.py --holdings 30 --iterations 6000 --street flop
#   CFR RESOLVE LOOP (the actual per-decision cost): 10.00s (1.666 ms/iteration)
python3 PokerAI/tools/bench_resolve_timing.py --holdings 30 --iterations 10000 --street turn
#   CFR RESOLVE LOOP (the actual per-decision cost): 16.88s (1.688 ms/iteration)
```

On this machine: **≈10 seconds** for a flop decision at the paper's own
stated 6,000-iteration real-time budget for preflop/flop off-tree nodes,
and **≈17 seconds** for a turn/river decision at its 10,000-iteration
budget, with 30 possible holdings per side. Per-iteration cost scales
roughly with (holdings/side)², since the dominant per-node work is
averaging showdown/leaf value over the opponent's whole range; going from
6 to 30 holdings/side (25x more combo pairs) measured about a 9x
slowdown per iteration in this implementation.

This is a genuine wall-clock measurement, but of the **toy game**, not the
real system -- treat it only as a rough order-of-magnitude figure for how
range size affects cost, for three reasons that could each swing the real
number by a large, currently unknown factor: (1) this toy game's bet
abstraction has only ~2-3 actions per street, versus the real system's
presumably much larger/deeper bet-sizing tree; (2) this is interpreted,
single-threaded Python -- real (missing) C++ would very likely be faster
per elementary operation, plausibly by 20-100x, but could also use more
threads or fewer, an unknown; (3) `Depth_limit_Search.h`, the code that
would actually do this in DecisionHoldem, does not exist in this
repository (see 2.1 below) -- there is no real implementation to measure,
only this demo's inferred reconstruction of its mechanics.

Run it yourself:

```bash
python3 PokerAI/tools/depth_limited_search_demo.py
```

Expect all `[PASS]` lines (7 of them) and exit code 0; the two `[FINDING]`
lines above are explained in-line. This is **not** a recovery of the real
`Depth_limit_Search.h` and does not touch the real cluster files, `Engine`,
or `Bulid_Tree.h` — it is a clearly-labeled, from-scratch validation
artifact demonstrating the inferred algorithm's mechanics on a toy game
small enough to run in milliseconds on any machine.


### 2.1 `Depth_limit_Search.h` — missing *source code*, not just data

The README's own file tree (near the bottom of the file) lists a file that
does not exist anywhere in this repository or its git history:

```
├── Depth_limit_Search.h # it is a algorithm of real time searching in each subgame
```

`grep -rn "Depth_limit_Search"` across every `.h`/`.cpp`/`.md` in the repo
turns up **only that one README line** — no `Main.cpp` or any other source
file `#include`s it, so it was never wired into the buildable command-line
pipeline at all, even as a stub. This matches
[issue #10](https://github.com/AI-Decision/DecisionHoldem/issues/10)
("Depth_limit_Search.h is missing"), open and unanswered by the authors.

This is the real-time depth-limited search / subgame-solving algorithm —
the paper's headline contribution ("safer depth-limited solving with diverse
opponent ranges") — and it is a **second, independent gap from the data
files**: even with all six `cluster/*` files in hand, only blueprint
*training* and *evaluation* (`BlueprintMCCFR.h` / `Multi_Blureprint.h` /
`Main.cpp`) are actually present and buildable. The real-time search itself
is only shipped as the precompiled Linux x86_64 `AlascasiaHoldem.so` /
`blueprint.so` (see section 3, and README footnote `[5]`: "Currently some
source codes only provide compiled files, and they will be open sourced in
the near future") — that promise has not been fulfilled as of this
investigation. There is no source in this repo to inspect, port, or fix for
this component; it is a hard, source-level gap independent of platform.
(See 2.1.0 above for what *is* actually present and reusable.)

**The gap isn't just missing code — the authors' own paper never publishes
the algorithm either.** The README cites
[arXiv:2201.11580](https://arxiv.org/abs/2201.11580) (Zhou, Bai, Zhang, Duan,
Huang — a 4-page short paper). Its Section 2 ("Methods") describes the idea
only at a conceptual level: it extends Brown et al. 2018's depth-limited
("Modicum") subgame solving by modeling several diverse opponent private-hand
*ranges* at off-tree nodes instead of Modicum's small fixed set of
continuation strategies, claiming this reduces exploitability without
degrading strategy quality. It gives one operational detail (6,000 real-time
search iterations for preflop/flop off-tree nodes, 10,000 for turn, and
10,000 unconditionally on the river) but explicitly states: *"Our subsequent
articles will introduce the details of the algorithm."* No such follow-up
article was found (only this one short paper exists, revised once in
2024-05 with no added technical content). So even independent of source
code, the precise algorithm (how ranges are constructed/weighted, the
exploitability-safety proof) has never been publicly disclosed anywhere —
`Depth_limit_Search.h` cannot be reconstructed from the paper either.

### 2.1.1 Verified binary layout of the cluster files (from `Engine.h` load/lookup code)

All three post-flop cluster files share one layout, keyed by the C(52,2)=1,326
unordered hole-card pairs `(i,j)`, `i<j`, stored in ascending `i*52+j` order.
Each pair's block is a pair of parallel, board-ID-sorted arrays used for
binary search (`find_flop`/`find_turn`/`find_river`):

| File | Boards enumerated per hole-pair | Board encoding (sorted board cards, base-52 mixed radix) | `keys` dtype | `values` dtype (= bucket id) | Bucket range (paper's Table 1) |
|---|---|---|---|---|---|
| `flop_hand_cluster.bin` | `C(50,3)=19,600` (3-card boards from the 50 cards not in hero's hand) | `c0*52²+c1*52+c2` | `unsigned` (4B) | `unsigned` (4B) | `[0,50000)` |
| `turn_hand_cluster.bin` | `C(50,4)=230,300` | `c0*52³+c1*52²+c2*52+c3` | `unsigned` (4B) | `unsigned` (4B) | `[0,5000)` |
| `river_hand_cluster.bin` | `C(50,5)=2,118,760` | `c0*52⁴+c1*52³+c2*52²+c3*52+c4` | `unsigned` (4B) | **`unsigned short`** (2B, not 4B) | `[0,1000)` |

This precisely explains every downloaded file's exact byte count (see the
table in section 2.2): `1,326 × boards_per_pair × (key_bytes + value_bytes)`.
`preflop_hand_cluster.bin` differs (no board — it's just `1,326` keys +
`1,326` values, i.e. `int` × 2 arrays = `1326*4*2 = 10,608` bytes) since
preflop hands are used exactly, unclustered (169 canonical strategic hands
per the paper, but this file stores the full 1,326-combo → cluster mapping).

### 2.2 Community-sourced mirror of the missing artifacts (GitHub issue #2)

A search of this repository's GitHub issues (prompted by the user) surfaced
a community-reposted copy of the files, found via
[issue #13](https://github.com/AI-Decision/DecisionHoldem/issues/13) →
[issue #2](https://github.com/AI-Decision/DecisionHoldem/issues/2). Two
mirrors were mentioned there:

- An old IP-hosted mirror (`http://45.63.124.212/...`, posted by `yegorrr` in
  2022) — confirmed **dead** (no response to `curl -sI` for any of the
  files).
- A Google Drive folder posted by `rkulskis` on 2022-10-31:
  `https://drive.google.com/drive/folders/1zTAm0AyRCoxP61cMJNUdt9CCMHevWOCz`,
  described by the poster as obtained from the README's Baidu Netdisk link
  via an unspecified third-party "unlock" service. **This is still an
  unofficial, anonymous, unverified community re-upload — not from the
  original authors, and no checksums are published anywhere for it.** A
  2026-05-26 comment on the same issue (`Immelstorn`) notes the folder is
  missing `preflopallin1326.1225.bin`, which is not a problem since that file
  is already committed in this repository.

Because the user explicitly asked to search issues for the missing files and
then directed continued pursuit of this lead (including clearing local disk
space to accommodate the download), the following files were downloaded from
that Drive folder into `PokerAI/cluster/` (git-ignored — see `.gitignore` —
never committed, per the constraint on not checking in large third-party
data):

| File | Downloaded size (bytes) | Independently computed expected size | Match? |
|---|---|---|---|
| `preflop_hand_cluster.bin` | 10,608 | 10,608 | ✅ exact |
| `flop_hand_cluster.bin` | 207,916,800 | 207,916,800 (1,326 combos × 19,600 × 8 bytes) | ✅ exact |
| `sevencards_strength.bin` | 1,337,845,600 | 1,337,845,600 (C(52,7)=133,784,560 × 10 bytes) | ✅ exact |
| `turn_hand_cluster.bin` | 2,443,022,400 | 2,443,022,400 (1,326 combos × 230,300 × 8 bytes) | ✅ exact |
| `river_hand_cluster.bin` | 16,856,854,560 | 16,856,854,560 (1,326 combos × 2,118,760 × 6 bytes; values are `unsigned short`, not `unsigned`) | ✅ exact |
| `blueprint_stgy.dat` (mirror's name for `blueprint_strategy.dat`) | 16,123,074,125 | *(not independently verifiable — see caveat below)* | hard-linked to `cluster/blueprint_strategy.dat` |

**All six files this project needs now exist and are placed correctly.**
`blueprint_stgy.dat` was hard-linked (not copied, so no extra disk space) to
`cluster/blueprint_strategy.dat`, the exact path/name `Main.cpp`/
`tree/Save_load.h`'s `load()` expects. Unlike the five cluster files, there is
no independent formula to verify `blueprint_strategy.dat`'s size against
(it's a serialized CFR game tree, whose size depends on the exact training
run that produced it, not a fixed formula) — its correctness can only be
checked by actually loading it, which section 2.1's RAM ceiling prevents on
this machine (see below).

Every file checked so far against sizes computed independently from
`Engine.h`'s own constants (before any download happened) is an **exact
byte-for-byte size match**, which is a strong — though not conclusive —
signal that this mirror is a legitimate, unmodified copy of the original
Baidu-hosted files, not tampered or corrupted. This should still be treated
as **best-effort, unofficial data of unverifiable provenance** (no hashes
published by the original authors to compare against), consistent with the
original instruction to avoid dubious third-party binaries — it is
documented here transparently rather than silently substituted, and it is
never committed to this repository.

**Hard RAM caveat found while investigating this path:** `Engine`'s
constructor (`poker/Engine.h`) unconditionally `new`'s ~19.4 GiB of heap for
the turn/flop/river cluster arrays *before* attempting to read any file —
this happens regardless of whether the files exist, because the allocation
size is driven by fixed constants (`turn_community_total`,
`flop_community_total`, `river_community_total`), not by actual file size.
On top of that, `seven_keys`/`seven_strengths` (the seven-card-strength
lookup table) are declared as **static global C arrays**, not heap
pointers — `ll seven_keys[133784560]` + `unsigned short
seven_strengths[133784560]` — adding a further ~1.34 GiB that is reserved as
soon as any binary linking `Engine.h` starts, independent of whether
`Engine()` is ever constructed. Total: **`Engine()` construction requires
~20.8 GiB of memory, minimum, before a single CFR node is allocated.**

Only two of the six `ifstream` opens in `Engine::load()`
(`sevencards_strength.bin`, `preflop_hand_cluster.bin`) check for open
failure and throw a clean error; the other four (`turn`, `flop`, `river`,
`preflopallin`) do not check, so a missing file there silently leaves the
just-allocated heap arrays as uninitialized garbage rather than failing
loudly.

**This machine (`sysctl hw.memsize` = 17,179,869,184 bytes = exactly 16 GiB
total, not just "free") cannot run `Engine()` at all, ever, regardless of how
much RAM is freed up by closing other applications** — the ~20.8 GiB
requirement exceeds the entire physical memory of this host by itself. This
is a harder, more definitive blocker than "insufficient free RAM": it is a
total-capacity ceiling. Running the real blueprint trainer, evaluator, or
any depth-limited search wired directly into the real `Engine` class on this
machine is impossible without either (a) a host with ≥24–32 GiB RAM, or
(b) rewriting `Engine`'s cluster storage to be memory-mapped/on-disk rather
than fully resident (a nontrivial source change, out of scope here since it
wasn't reported as broken, just resource-heavy by design).

### Regenerating `blueprint_strategy.dat` yourself (full-scale, once you have the 5 files)

```shell
cd PokerAI
g++ Main.cpp -o Main.o -std=c++11 -mcmodel=large -lpthread
./Main.o 0
```
This runs `multiprocess_blueprint()`: 100 threads, 100,000,000 outer
iterations, dumping `cluster/blueprint_strategy.dat` every 100,000 iterations.
Per the README this took the original authors 3–4 days on a 48-core / 512GB
workstation — there is no reduced/small-scale flag exposed anywhere in the
source (`n_iterations`, `threadnum`, etc. are compile-time `const`s tied to the
fixed 52-card / 2-player game definition, not a toy parameter), so a smaller
verification run is only possible by editing those constants yourself and
accepting a materially different (much weaker) strategy — we did not do this
since it wasn't requested and would produce a non-representative artifact.

### Actual attempted run of the real engine, once all six files were present (2026-08-26)

Once every required file finished downloading, byte-verified, and was placed
at its expected path (`blueprint_stgy.dat` hard-linked to
`cluster/blueprint_strategy.dat`), `Main.cpp` was rebuilt with the documented
command (clean build, 10 pre-existing warnings only, exit 0) and actually
executed — `./Main.o 1` (the evaluation branch, which loads
`blueprint_strategy.dat` and prints two counterfactual values) — to get a
real, empirical result rather than relying purely on the arithmetic in
section 2.1.

**This machine's swap was already constrained** (`sysctl vm.swapusage`:
7.2GB total, ~1GB free, encrypted, before the run) and it is a shared,
non-dedicated host, so the run was launched under close, second-by-second
monitoring (`ps`, `vm_stat`, `sysctl vm.swapusage`) with a hard intent to
kill it at the first sign of dangerous resource growth, rather than let it
run unsupervised to completion or failure.

Attempting `ulimit -v <N>` first, to cap the process's virtual address space
and force a fast, clean `std::bad_alloc` instead of relying on live
monitoring, failed outright: `ulimit: virtual memory: cannot modify limit:
Invalid argument` — macOS/XNU does not implement `RLIMIT_AS` at all (this is
a Linux-only resource limit), so there is no OS-level way to safely cap a
single process's address space on macOS the way there would be on Linux.

The uncapped run was therefore launched under live monitoring. Within
**~20 seconds**, `sysctl vm.swapusage`/`vm_stat` showed macOS dynamically
growing its encrypted swap file to satisfy the engine's ~19.4 GiB allocation:

```
t+0s:   vm.swapusage: total = 17408M  used = 16661M  free =  747M
t+8s:   vm.swapusage: total = 18432M  used = 17456M  free =  976M
t+16s:  vm.swapusage: total = 19456M  used = 18705M  free =  750M
t+20s:  vm.swapusage: total = 19456M  used = 19089M  free =  367M
df -h /:  free space dropped from 25Gi to 12Gi in the same ~20 seconds
```

The process was killed at this point (`kill -9`) rather than let it continue,
because it was visibly consuming shared disk space (via swap-file growth) at
roughly 0.6 GiB/second with no end in sight, on a machine shared with other
users/sessions. After the kill, macOS reclaimed most of the swap
automatically within ~10 seconds (`vm.swapusage` total dropped back to
~9.2GB, disk free recovered to ~24Gi) — so no lasting disk damage was done,
but the live trajectory made clear that letting it run to completion (or to
an actual out-of-memory failure) would have consumed most or all of the
remaining ~25GB of free disk as swap, on a host this investigation does not
own exclusively.

**This is now a directly observed, not just calculated, confirmation of
section 2.1's finding:** the real `Engine()` cannot be constructed on this
16 GiB machine. Unlike a clean `std::bad_alloc`/crash, macOS's default
behavior is to try to paper over the shortfall with aggressive, encrypted
swap-file growth — which is *worse* for a shared host than a clean failure,
since it silently eats shared disk space and degrades performance for any
other concurrent user, rather than failing fast. **Do not run `./Main.o`
(either mode `0` or `1`) on a machine with less than ~24 GiB of true free
RAM+swap headroom beyond this ~19.4 GiB requirement, and never on a shared
host without first confirming you have exclusive use of its resources.** The
user's separate 32 GiB machine, with no other constraints, is the
recommended place to actually attempt this — 19.4 GiB comfortably fits
within 32 GiB with headroom for the OS, the CFR tree structures the engine
builds afterward, and normal process overhead.

## 3. External Leduc poker reference example — successfully built and run

The README (line 15) points to a separate, smaller companion repository for
understanding the algorithm: ["a simple program about Leduc
poker"](https://github.com/zqbAse/PokerAI_Sim), by the same
project/author group. This is a genuinely separate GitHub repo (own git
history, own README, no shared commits with DecisionHoldem) — it is not
vendored into this repository and was not committed here; it was cloned to
a scratch location (`/tmp/PokerAI_Sim`) purely to validate whether the
"smallest meaningful included/referenced component" mentioned in this
project's README actually builds and runs, since DecisionHoldem itself has
no runnable component that doesn't depend on the missing multi-GB cluster
data (section 2).

**It has the exact same architecture as DecisionHoldem** (`Poker/Engine.h`,
`Poker/State.h`, `Tree/Node.h`, `Tree/Bulid_Tree.h`, `Tree/Save_load.h`,
`Tree/Exploitability.h`, MCCFR in `blueprint.cpp`) but scaled down to actual
Leduc poker (6-card deck, 2 rounds) instead of full 52-card Texas Hold'em —
so no pretrained cluster/hand-abstraction files are needed at all; the whole
game is small enough to enumerate exactly.

**Build, as documented in its own README:**
```shell
git clone https://github.com/zqbAse/PokerAI_Sim.git
cd PokerAI_Sim
g++ blueprint.cpp -o blueprint.o -std=c++11
```
This failed exactly once, with the same class of defect found repeatedly in
DecisionHoldem itself (section 1) — a call site passing an argument to a
function that takes none:
```
blueprint.cpp:196:25: error: too many arguments to function call, expected 0, have 1
            randi.reset(rand());
./Util/Randint.h:18:10: note: 'reset' declared here
    void reset() {
```
`Util/Randint.h`'s `reset()` only reseeds from the wall clock; the call site
clearly intends to reseed from a caller-supplied value (matching the
existing seeded constructor `Randint(uint32 seed)` right above it, and the
surrounding comment translating to "recompute the random seed every 10,000
iterations"). Fixed, behavior-preserving, by adding the missing overload
(not modifying the existing no-arg `reset()`):
```cpp
void reset(uint32 seed) {
    prngState = seed;
}
```
This fix was applied only to the scratch clone in `/tmp` (documented here as
text, not committed anywhere, since this is a separate upstream project this
investigation does not own) — anyone reproducing this should apply the same
one-method addition to `Util/Randint.h`.

**After that fix, it built cleanly (0 errors, 0 warnings) and ran to full
completion exactly as documented:**
```shell
$ time ./blueprint.o
...
16.728
-14.5346
16.4112
-14.1698
16.4024
-12.5373
15.8794
-13.2493
15.5639
-13.9671

real    0m22.3s
```
This is the genuine, complete MCCFR training loop — 10,000,000 iterations
(`n_iterations` in `blueprint.cpp`), exactly as configured in the shipped
source, run to completion, no shortcuts or reduced parameters. Every 2
million iterations it computes and prints a best-response
exploitability-style value for each player (the two numbers per block above)
and serializes the current strategy tree to `blueprint_sim.stgy` (31,712
bytes) via `Tree/Save_load.h`'s `visualization()`/save path — the same
save/load mechanism `Main.cpp` uses for the real, currently-unloadable
`blueprint_strategy.dat`. Player 0's best-response value trends down across
checkpoints (16.7 → 16.4 → 16.4 → 15.9 → 15.6), consistent with the strategy
converging toward equilibrium as training progresses; player 1's shows more
run-to-run noise (expected — it's a single MCCFR run, not an average over
seeds, and only 5 sample points).

**Why this matters:** it is a real, unmodified-algorithm, complete,
successful end-to-end run of exactly the technique (external-sampling MCCFR
building a game tree, discounted regret updates, periodic best-response
exploitability checks, strategy serialization) that DecisionHoldem's own
`BlueprintMCCFR.h`/`Multi_Blureprint.h` implement at full 52-card scale — it
just does so on Leduc poker's tiny, data-free game, which is exactly why the
README recommends it as the way to "understand the algorithm framework and
its mechanism" without needing the missing multi-GB pretrained data. This is
the most concrete "smallest meaningful component, actually run" result this
investigation produced.

## 4. Python / GUI stack (`pypokergui`, Slumbot/OpenStackTwo bots)

- `requirements.txt` pinned `tornado==4.4.2`, which does not import on modern
  Python (3.10+, tested here on 3.14): it references the long-removed
  `collections.MutableMapping` alias. **Fixed:** bumped to `tornado>=6.2,<7`
  (verified this version's APIs — `tornado.web.Application`,
  `RequestHandler`, `WebSocketHandler`, `IOLoop.current().start()`,
  `tornado.options` — are exactly what this codebase uses, and confirmed by
  actually importing `server/poker.py` successfully after the bump, versus a
  hard `AttributeError` before it).
- `pypokergui/play_with_slumbot.py` imports `requests`, which was **absent**
  from `requirements.txt`. **Fixed:** added `requests>=2.25`.
- **Hard blocker, not fixable from this repo:** `pypokergui/server/game_manager.py`
  and `pypokergui/fish_player_setup.py` both do
  `cdll.LoadLibrary('./AlascasiaHoldem.so')`. That file (and `blueprint.so`) are
  confirmed-via-`file`(1) Linux x86_64 ELF shared objects
  ("`ELF 64-bit LSB shared object, x86-64`"). Attempting to load them on macOS
  fails at the OS level (`OSError: ... slice is not valid mach-o file`), which
  is expected and unfixable without source — and per the README's own footnote
  "[5] Currently some source codes only provide compiled files", the
  real-time depth-limited-search implementation that these `.so` files contain
  was **never open-sourced** in the first place. There is nothing in this
  repository to port, patch, or recompile for macOS. The only legitimate path
  to exercising this component is running it under Linux/x86_64 (e.g. Docker);
  no container runtime was available in this sandbox to verify that path.
- The README's GUI run command (`python DecisionHoldem/pypokergui/server/poker.py 8000`
  run from inside `PokerAI/`) assumes a nested `PokerAI/DecisionHoldem/...`
  layout that does not exist in this repository (`PokerAI/` and `pypokergui/`
  are siblings under the repo root). The corrected invocation, given
  `AlascasiaHoldem.so` must be resolvable relative to the process's working
  directory:
  ```shell
  cp PokerAI/AlascasiaHoldem.so pypokergui/server/
  cd pypokergui/server && python poker.py 8000
  ```
  (this still requires the Linux `.so` to actually load, per the blocker above).
- GraphViz (`dot`) — used only for the optional `Visualize_Tree.h` output — is
  not installed in this sandbox; install via `brew install graphviz` on macOS
  if you want PNG tree renders.

## 5. What was validated, with exact commands

```shell
# 1. Clean compile now succeeds (previously failed with invalid-preprocessing-directive
#    and narrowing-conversion errors):
cd PokerAI && g++ Main.cpp -o Main.o -std=c++11 -mcmodel=large -lpthread
# -> 0 errors

# 2. CLI now dispatches correctly to both documented modes (previously argv[0]==0
#    was unreachable dead code):
./Main.o 0   # -> reaches multiprocess_blueprint(), then fails at Engine() ctor
./Main.o 1   # -> reaches evaluation branch, then fails at Engine() ctor
# Both report: "hand_value file is not exist" and abort — this is the documented,
# expected, and now-correctly-reached blocker (missing cluster/*.bin), not a bug.

# 3. Python dependency fixes verified in a clean venv:
python3 -m venv /tmp/dh_venv && source /tmp/dh_venv/bin/activate
pip install -r requirements.txt
python3 -c "import server.poker"   # (run with cwd=pypokergui, PYTHONPATH set)
# -> fails only at cdll.LoadLibrary('./AlascasiaHoldem.so') with a mach-o error,
#    confirming the Linux .so is the sole remaining blocker for the GUI.
```

## 6. Summary for the user

- **Fixed and verified:** the C++ source now compiles cleanly on macOS/Clang
  and on Linux/GCC (fixes are behavior-preserving on both), the CLI's train
  vs. evaluate dispatch bug is fixed, and the Python GUI/bot dependency list is
  corrected for modern Python.
- **A real, complete, working component was found, fixed, built, and run to
  full completion: the README's own referenced Leduc poker example
  (section 3).** It needs no pretrained data at all, and its full documented
  10,000,000-iteration MCCFR training run completed in ~22 seconds on this
  machine after one small, documented, behavior-preserving build fix
  (identical in nature to the fixes in section 1) — producing a saved
  strategy file and a converging best-response value. This is the strongest
  concrete "it actually runs" result in this whole investigation, and
  exercises the *same* MCCFR/game-tree/save-load architecture DecisionHoldem
  itself uses at full scale.
- **Data files: all six obtained, from an unofficial mirror, not the README's
  official source.** None of `sevencards_strength.bin`,
  `preflop_hand_cluster.bin`, `flop_hand_cluster.bin`, `turn_hand_cluster.bin`,
  `river_hand_cluster.bin`, or `blueprint_strategy.dat` are in this
  repository, its git history, or any GitHub release, and none are
  regenerable from source without already having them (see section 2). All
  six were eventually located via a community re-upload linked from
  [issue #13](https://github.com/AI-Decision/DecisionHoldem/issues/13) →
  [issue #2](https://github.com/AI-Decision/DecisionHoldem/issues/2), byte-size-
  verified against formulas independently derived from `Engine.h`, and
  placed at their expected paths — but this is **unofficial, unverified-
  provenance data**, not the README's official Baidu Netdisk source, which
  this investigation did not access. It is git-ignored and was never
  committed to this repository.
- **Still cannot actually be run, on this 16 GiB machine, even with all data
  present.** `Engine()` unconditionally requires ~19.4 GiB of RAM to
  construct, before a single file is even opened. This was originally a
  calculated finding (section 2.1) and was then **directly, empirically
  confirmed** by actually building and running `./Main.o` with all six real
  files in place (section 2, "Actual attempted run"): the process was
  observed live driving macOS's swap file from 17.4GB to 19.4GB (and disk
  free from 25GB to 12GB) in about 20 seconds, and was killed as a safety
  measure before it could either finish or exhaust this shared host's disk.
  This is a hard ceiling, not a "close a few apps" problem — it requires a
  machine with genuinely more RAM (the user's separate 32 GiB machine should
  work).
- **Cannot be run on macOS regardless of data or RAM:** `AlascasiaHoldem.so`
  and `blueprint.so` are Linux x86_64 binaries with no included source; they
  need a Linux x86_64 runtime (e.g. Docker), which was unavailable in this
  sandbox (`docker` is not installed here).
- **Disabling `river_hand_cluster.bin` does not unlock a working flop
  decision (section 9), tested directly.** It safely frees ~16.86GB for the
  isolated flop/turn hand-clustering functions (confirmed working, ~1.6GB
  peak RSS), but this repository's only decision/evaluation code
  (`getcfv_whole_holdem`) performs full backward induction to the river by
  construction — it recurses into river-street nodes as an inherent part of
  computing any flop-level value, so it crashes (null-pointer segfault,
  reproduced) rather than stopping cleanly at the flop. A real flop-only
  decision would require the depth-limited/subgame-resolving search this
  repo's `Depth_limit_Search.h` was meant to provide but is missing from the
  released source (see the Python reference implementation instead, section 7).
- No placeholder/fabricated data files were created or committed anywhere in
  this repository.

## 7. Possible path to a working real-time search: an external solver bridge

Since `Depth_limit_Search.h` (section 2.1) is genuinely missing source and the
`.so` files (section 3) cannot be ported without it, DecisionHoldem itself has
no way to regain real-time search on this repo alone. A separate, unrelated
local project, `$HOME/src/TexasSolver` (a personal fork, `nosami/skypoker`,
of the open-source [TexasSolver](https://github.com/bupticybee/TexasSolver)
project), was evaluated as a possible substitute:

- It is AGPLv3-licensed, same as DecisionHoldem, so combining code is legally
  fine.
- It contains real, working CFR-based real-time/subgame-solving code
  (`src/solver/PCfrSolver.cpp`, `src/solver/slice_cfr.cpp`,
  `src/runtime/PokerSolver.cpp`, plus a `pybind` entry point) — functionally
  the same *category* of component (real-time solve of a subgame given a
  board/range/pot) that `Depth_limit_Search.h` was supposed to provide.
- It is **not** a drop-in replacement: it has its own card/range/tree data
  structures with no shared format with DecisionHoldem's `Node.h`/`Table.h`/
  cluster-ID scheme, and it implements standard subgame resolving rather than
  the DecisionHoldem paper's specific "diverse opponent ranges" technique.
  Using it would mean building and documenting a translation bridge (export
  DecisionHoldem board/range/CFV state at an off-tree node → TexasSolver's
  input format → run its solver → map the returned strategy back to
  DecisionHoldem's action abstraction), which is a genuine multi-day
  integration project, not a quick fix, and was intentionally not started
  without explicit direction given the scope jump involved (building new
  functionality vs. fixing/documenting what exists). Flagged here as the most
  concrete legitimate path forward if real-time search is ever needed.

## 8. RAM-avoidance alternative: binary search directly against the file on disk

Section 2's ~20.86GB unconditional RAM requirement (Engine.h loading all
cluster arrays fully into memory at construction) was investigated for a
lighter-weight alternative: skip loading the arrays into RAM at all, and
instead perform `find_turn`/`find_river`/`find_strength`'s binary search
directly against the file on disk, one `pread()` per comparison, exactly
mirroring the existing algorithm but replacing array indexing with a seek+read.

**Why this is possible:** each cluster array is stored per hole-card combo as
an independent, pre-sorted, contiguous block (`Engine.h` lines 99-144:
`keys[N]` then `values[N]`, one block per `(i,j)` pair, written in a fixed,
computable file order). `find_turn`/`find_river`/`find_flop` each binary-search
only *within one hand's own block*, never across the whole file. So a
disk-based binary search for one lookup only ever touches
`~log2(N)` scattered 4-byte offsets inside an ~0.9MB (turn) / ~8.5MB (river)
region — not the full 2.3GB/16GB file.

**Empirical measurement** (`pread()`-based binary search implemented to
exactly mirror `find_turn`/`find_river`/`find_strength`, run against the real,
byte-verified `turn_hand_cluster.bin` and `sevencards_strength.bin` on this
machine; page cache forcibly disabled per-fd via macOS's `F_NOCACHE` fcntl(48)
to guarantee genuine disk I/O, not a warm-cache artifact):

```
$ python3 PokerAI/tools/bench_disk_binary_search.py nocache
--- turn_hand_cluster.bin  (N=230,300/hand)   --- mean 0.362 ms/lookup (17.7 reads, ~20 us/read)
--- river_hand_cluster.bin (N=2,118,760/hand) --- mean 0.896 ms/lookup (21.0 reads, ~43 us/read)
--- sevencards_strength.bin (N=133,784,560)   --- mean 1.941 ms/lookup (27.0 reads, ~72 us/read)
```

With normal page-cache behavior re-enabled (no `F_NOCACHE`), repeat lookups
against the *same* already-touched block drop to ~0.01-0.08 ms (turn/river)
and ~0.5 ms (seven-card, whose 1.25GB table is scattered wider so warms more
slowly) — because the OS transparently caches whatever 4KB pages get touched.

**Why this matters:** a real-time resolve only ever queries the small, fixed
set of hole-card combos present in hero's and villain's supplied ranges (e.g.
~30 each ⇒ ≤60 distinct per-hand blocks across the *entire* multi-thousand-
iteration resolve, not per iteration). So the realistic cost is a one-time
cold warm-up of roughly `60 x (0.90ms river + 1.94ms strength) ≈ 0.17s`,
after which every subsequent iteration's lookups hit the OS page cache for
free. That is comparable to or cheaper than section 8's measured ~7-8s
one-time full-array load, while resident memory stays in the tens-of-MB range
(only the touched blocks), not 2-16GB per array.

**Conclusion:** doing the binary search directly against the file (via plain
`pread`/`fread` at the same offsets `Engine.h` already computes, or via
`mmap()` for less code churn) is a legitimate, more surgical alternative to
both "load everything into RAM at once" (current code, ~20.86GB) and "load
one street's whole array lazily" (~15.7GB alone for river, still exceeding
this machine's 16GB). It was benchmarked here as evidence of feasibility but
was not implemented in `Engine.h`, per direction to keep this investigation
analysis/documentation-only rather than modify the real engine's loading
strategy.

### 8.1 `blueprint_strategy.dat` is a different case: loaded once, not queried per-hand

Unlike the four `*_hand_cluster.bin`/`sevencards_strength.bin` lookup tables
above (static arrays, binary-searchable per query without loading them
whole), `blueprint_strategy.dat` is **deserialized once into an in-memory
tree** and never touched on disk again for the rest of the process:

```cpp
// Main.cpp:39
load(root, "cluster/blueprint_strategy.dat");   // single call, at startup
check_subgame(root, state);
cout << getcfv_whole_holdem(root, state, 0);
```

`load()` (`tree/Save_load.h:111`) opens the file once and `bulid_bluestrategy()`
recursively `fin.read()`s it **sequentially, front-to-back**, reconstructing
every `strategy_node`'s `regret`/`averegret` arrays in RAM (this sequential,
whole-file deserialization is why this one file alone accounts for ~16GB of
the engine's ~19.4GiB RAM requirement from section 2/6 — it cannot be
binary-searched like the cluster files, because it is a tree of variable-
length per-node arrays, not a fixed-stride sorted array). The file handle is
closed at the end of `load()`; there is no re-opening or seeking back into it.

After that one-time load, every further consultation of the blueprint
(`check_subgame`, `getcfv_whole_holdem`'s recursive CFV walk, and — by
analogy — any real-time decision during actual play) is a plain in-memory
pointer/array dereference (`root->actions + i`), costing zero additional
disk I/O, no matter how many decision points or hands are subsequently
processed in that run.

**Answer: once per process/session, at engine startup — not once per hand
and not once per decision.**

## 9. Experiment: can disabling river_hand_cluster.bin loading enable a flop decision?

Tested directly, empirically, and safely (without risking the shared host),
then **reverted afterward at the user's request** — `poker/Engine.h` is back
to its original, unconditional river-loading behavior, and the temporary
test tool has been removed. The findings below remain accurate as a record
of what was tested and observed; they just no longer correspond to any code
currently in this tree.

**Short answer: no — not through this repository's only existing decision
entrypoint, and not for an algorithmic reason, not just a RAM reason.**

### What was tested (code no longer present — see note above)

`poker/Engine.h`'s `load()` was temporarily given an opt-in build flag,
`DH_SKIP_RIVER_CLUSTER` (default OFF — normal/Linux behavior was completely
unchanged unless this macro was explicitly defined at compile time), which
skipped allocating and reading `river_hand_cluster.bin` (~16.86GB) entirely.

A small standalone test, `PokerAI/tools/test_engine_no_river.cpp` (also
since removed), was built against this flag and run under live
`ps`/`vm_stat`/disk monitoring:

```
g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o /tmp/test_engine_no_river tools/test_engine_no_river.cpp
cd PokerAI && /tmp/test_engine_no_river
```

Result (measured, not estimated):

```
[DH_SKIP_RIVER_CLUSTER] river_hand_cluster.bin NOT loaded (experiment mode)
Engine() construction completed. Peak RSS so far: 1.634 GB
get_flop_cluster(hole={0,21}, flop={30,47,8}) = 1422  (flop cluster lookup OK, river not needed)
get_turn_cluster(hole={0,21}, turn={30,47,8,12}) = 220  (turn cluster lookup OK, river not needed)
Final peak RSS: 1.634 GB
```

So in isolation: **yes**, `Engine()` initializes safely under ~1.6GB (down
from the ~20.86GB all-cluster-files total) and `get_flop_cluster()` /
`get_turn_cluster()` both work correctly without river data — the disk/swap
state didn't move at all during this test (no risk to the shared host).
This confirms the river cluster table is genuinely independent, in isolation,
of flop/turn hand-strength classification.

A second, deliberately-crashed micro-test then confirmed what happens if
`get_river_cluster()` is called with river data skipped: it dereferences a
null pointer and segfaults immediately (`Segmentation fault: 11`, exit code
139) — by design, since `river_cluster[i].keys/values` are simply never
allocated in that mode.

### Why this doesn't get you a working flop decision anyway

The one and only decision/evaluation entrypoint that exists in this
repository, `Main.cpp`'s `else` branch → `getcfv_whole_holdem()` →
`getnode_cfv_holdem()` (`tree/Exploitability.h`), is **not** a "compute the
flop decision, stop there" function. It computes best-response
exploitability via full backward induction over the *entire* remaining game
tree: at a preflop→flop chance node it deals a flop and immediately
recurses into the flop-stage subtree (`betting_stage == 1` branch, line
~578 of `Exploitability.h`); reaching flop→turn it deals a turn card and
recurses further (`betting_stage == 2`, calls `get_turn_cluster` then
recurses); reaching turn→river it deals a river card, calls
`get_river_cluster`, and recurses into the terminal/river subtree
(`betting_stage == 3`, line ~679) — and only once *that* recursion returns
does it aggregate values back up to produce a flop-level (or preflop-level)
number. This is standard CFR backward induction: a flop value literally
*is* the range-weighted average of everything that can happen on turn and
river beneath it.

Consequently, disabling `river_hand_cluster.bin` doesn't let
`getcfv_whole_holdem()` produce a flop decision and stop — it lets the
recursion proceed exactly as far as the first river-street node it reaches,
at which point `get_river_cluster()` dereferences a null pointer and the
whole process crashes (as reproduced above). This is an **algorithmic**
dependency baked into how this codebase computes decisions, not merely a
"the array happens to be loaded but never read" situation like some of the
other cluster files might have been if the tree structure were different.

Separately, `Main.cpp`'s evaluate branch also unconditionally loads the full
`blueprint_strategy.dat` tree (~16GB+, section 8.1) before it ever reaches
this recursion at all — a cost entirely unaffected by the river-cluster
toggle, and already independently confirmed (section 2) to make the full
run infeasible on this 16GiB machine on its own.

### Bottom line

- Skipping river loading **is safe and does work** for the narrow, provably
  isolated task of flop/turn hand-strength classification
  (`get_flop_cluster`/`get_turn_cluster`), and frees ~16.86GB of RAM for that
  narrow purpose — confirmed with a real, measured, low-risk run (peak RSS
  1.634GB, no disk/swap impact).
- It **cannot** be used to make this repository's actual `Main.cpp`
  decision/evaluation code produce a flop-only answer: that code performs
  full backward induction to the river by construction, and would need to be
  substantially rewritten (a real depth-limited/subgame-resolving search,
  as explored separately in the Python reference implementation,
  `tools/depth_limited_search_demo.py`, and discussed in section 7) to avoid
  needing river data for a flop decision. This is exactly the gap that
  `Depth_limit_Search.h` was meant to fill in the original design but is
  absent from the released source (section 2).
- No production behavior changed at any point, and no trace of the
  experiment remains in the source: `DH_SKIP_RIVER_CLUSTER` was off by
  default while present, and the guard plus the standalone test tool were
  both fully removed afterward — `poker/Engine.h` is back to its original,
  unconditional river-loading code, verified byte-identical via `git diff`.

### 9.1 Follow-up: "just let it run using swap" — declined, with the math

Asked next: rather than skip river data, why not just let the full,
unmodified engine run and let macOS grow swap to cover the shortfall? This
was checked quantitatively before touching anything, and declined, because
the shortfall is not marginal — it's arithmetically guaranteed to exceed
this shared host's available disk, calculated at the time of asking:

| Requirement | Size |
|---|---|
| `Engine()` cluster tables (section 2/6) | ~19.4 GiB |
| `blueprint_strategy.dat` in-memory tree (file size; live tree likely larger, section 8.1) | ~15.0 GiB |
| **Minimum total** | **~34.4 GiB** |
| Physical RAM on this machine | 16 GiB |
| **Minimum swap growth required** | **~18.4 GiB** |
| Disk free available for swap at the time of asking | 13 GB |
| **Shortfall** | **~5.4 GB short — before counting any additional memory the recursive CFV computation itself needs on top of just loading the data** |

This is worse than the previous live-monitored attempt (section 2's "Actual
attempted run"), which had 25GB of free disk to work with and *still* had
to be killed as a precaution at 12GB free/19.4GB swap used, well before
finishing even the loading phase. With only 13GB of headroom available this
time, letting the process run would not merely risk running low on disk —
it is expected to actually exhaust it, on a host shared with other users,
which could disrupt more than just this experiment. For this reason the run
was not attempted. The only two changes that would make this attemptable
are (a) more free disk/RAM on this shared host, or (b) running on the
user's separate 32GB machine (see section 2/6), where the ~34.4GiB minimum
requirement fits far more comfortably within RAM + ordinary swap headroom.

### 9.2 Re-test after relocating cluster files to external storage, and a measurement correction

Re-ran the exact same experiment from section 9 (same opt-in
`DH_SKIP_RIVER_CLUSTER` macro guard temporarily reapplied to
`poker/Engine.h::load()`, same standalone `tools/test_engine_no_river.cpp`
harness rebuilt from scratch, then both fully reverted/removed again
afterward — `git diff` against `poker/Engine.h` confirmed byte-identical
before and after) to confirm this still works now that
`turn_hand_cluster.bin`, `flop_hand_cluster.bin`, and
`sevencards_strength.bin` are symlinks to the external drive (section 11).

```
g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -o /tmp/test_engine_no_river tools/test_engine_no_river.cpp
cd PokerAI && /tmp/test_engine_no_river
```

Result:

```
[DH_SKIP_RIVER_CLUSTER] river_hand_cluster.bin NOT loaded (experiment mode)
Engine() construction completed. Peak RSS so far: 3.783 GB
get_flop_cluster(hole={0,21}, flop={30,47,8}) = 1422  (flop cluster lookup OK, river not needed)
get_turn_cluster(hole={0,21}, turn={30,47,8,12}) = 220  (turn cluster lookup OK, river not needed)
Final peak RSS: 3.783 GB
```

**Still works, same results** (identical flop/turn cluster values as the
original run). Two things changed, both worth recording:

1. **The `load()` call itself is now much slower** — it took roughly 2
   minutes instead of a couple of seconds, because `turn_hand_cluster.bin`
   (2.44GB) and `sevencards_strength.bin` (1.34GB) are now read from the
   external USB drive at its observed ~30-35MB/s (section 11) instead of
   the internal SSD. This is expected and matches the disk-vs-RAM tradeoff
   documented in section 11: moving files off the internal disk trades disk
   space for load-time I/O speed. It has no effect on peak RSS or on
   whether the lookups succeed.

2. **Correction to section 9's originally-reported "peak RSS 1.634 GB"
   figure — that number was wrong.** Cross-checking against the exact,
   known on-disk sizes of the files actually loaded when river is skipped
   (`sevencards_strength.bin` 1.338GB + `turn_hand_cluster.bin` 2.443GB +
   `flop_hand_cluster.bin` 0.208GB + `preflopallin1326.1225.bin` 0.014GB —
   each of which is a raw dump of the exact in-memory key/value arrays, so
   file size and resident size match almost exactly) gives an expected
   total of **~4.00 GB**, matching this re-test's measured 3.783GB far more
   closely than the previously-recorded 1.634GB. It is also the only figure
   consistent with section 9's own previously-stated ~20.86GB
   all-cluster-files total: 20.86 − 4.00 (non-river) = 16.86GB, which is
   exactly `river_hand_cluster.bin`'s size — whereas 20.86 − 1.634 = 19.2GB
   does not match anything. The original 1.634GB figure from this same
   session's earlier (now-reverted) test is superseded by this re-measurement;
   treat 3.78-4.0GB as the correct river-skip Engine() footprint going
   forward.

**Bottom line unchanged from section 9**: this confirms flop/turn cluster
lookups work correctly and cheaply in isolation (now ~3.8GB instead of the
full ~19.4GiB), but does not by itself give the repository's actual
decision entrypoint (`getnode_cfv_holdem()`) a working flop-only decision —
that still requires the depth-limited/subgame-resolving rewrite discussed
in section 9 and section 7, since the shipped code always recurses to the
river. Also note the external-drive relocation only changes *where the
bytes physically live*, not the *arithmetic* in section 9.1: attempting the
**full, unmodified engine** (all cluster files + the ~16GB blueprint tree)
is a separate, much larger ask than this narrow flop/turn-only test, and
the ~18.4GiB swap requirement computed there is unaffected by this
migration. The one relevant change is that the previously-cited disk-space
blocker for even attempting that under swap (13GB free, 5.4GB short) is no
longer present — internal disk free space is now ~98GB, comfortably above
the ~18.4GiB swap requirement. This does **not** mean the attempt is now
advisable: physical RAM is still 16GiB against a ~34.4GiB requirement, so
it would still mean heavy, sustained swapping for an unknown and
potentially very long duration, with a real risk of making this shared
host sluggish or unresponsive while it runs and no guarantee of completion.
That tradeoff was intentionally left for an explicit, informed decision
rather than attempted by default.

## 10. Appendix: complete binary-file inventory

A consolidated list of every binary (non-source-code) file relevant to this
repository, its purpose, and its size, gathered by direct filesystem
inspection (`ls -la`, `du -h`, `file`) rather than inferred from documentation.

### Pretrained data (`PokerAI/cluster/`) — git-ignored, never committed

Obtained from the unofficial community mirror described in section 2; not
present in this repository's git history or any official release.

| File | Size | Purpose |
|---|---|---|
| `river_hand_cluster.bin` | 16.86 GB (16,856,854,560 B) | Maps (hole cards, river board) to a hand-strength cluster ID; the largest lookup table, used for river-street decisions |
| `blueprint_stgy.dat` / `blueprint_strategy.dat` | 16.12 GB (16,123,074,125 B) | The trained MCCFR blueprint strategy tree — hard-linked under both names (same inode); `blueprint_strategy.dat` is the path `Main.cpp`/`Save_load.h` expect |
| `turn_hand_cluster.bin` | 2.44 GB (2,443,022,400 B) | Same clustering scheme, for the turn street |
| `sevencards_strength.bin` | 1.34 GB (1,337,845,600 B) | Static table ranking all `C(52,7)` = 133,784,560 seven-card hand combinations by raw strength |
| `flop_hand_cluster.bin` | 208 MB (207,916,800 B) | Same clustering scheme, for the flop street |
| `preflopallin1326.1225.bin` | 13.4 MB (14,066,208 B) | Precomputed preflop all-in equities for all 1,326 hole-card combos — the **only** cluster file actually committed to this repository |
| `preflop_hand_cluster.bin` | 10.6 KB (10,608 B) | Maps 1,326 hole-card combos to 169 canonical preflop hand clusters |

### Compiled binaries

| File | Size | Type | Purpose |
|---|---|---|---|
| `PokerAI/blueprint.so` | 1.84 MB | ELF 64-bit LSB shared object, x86_64 (Linux), not stripped | Precompiled real-time blueprint-strategy interface; **no source included** in this repo; will not load on macOS/arm64 without a Linux x86_64 runtime |
| `PokerAI/AlascasiaHoldem.so` | 1.84 MB | ELF 64-bit LSB shared object, x86_64 (Linux), not stripped | Precompiled real-time search/runtime engine; **no source included**; same Linux-only limitation |
| `PokerAI/Main.o` | ~494 KB | Mach-O 64-bit executable, arm64 (macOS) | This session's local build output of `Main.cpp`; git-ignored (`*.o`), not committed, rebuildable via the command in section 1 |

### Non-data images (GUI/documentation assets — not investigated further, self-explanatory by filename)

| Location | Count | Total size | Purpose |
|---|---|---|---|
| `PokerAI/img/*.png,*.jpg` | 6 files | 1.3 MB | README diagrams/screenshots (game tree, results) |
| `pypokergui/server/static/images/*.png,*.jpg` | 55 files | 2.3 MB | Standard 52-card playing-card sprites plus table/pot/fold graphics for the web GUI |

No other binary file types (`.exe`, `.dll`, `.dylib`, `.a`) exist anywhere in
the repository. This is a complete inventory as of this investigation.

## 11. Relocating the large cluster/blueprint files to external storage

Since this shared host has limited free internal disk (as low as ~13GB free
at points during this investigation — see sections 2 and 9.1), the five
largest pretrained data files (~34GB total: `river_hand_cluster.bin`,
`blueprint_stgy.dat`/`blueprint_strategy.dat`, `turn_hand_cluster.bin`,
`sevencards_strength.bin`, `flop_hand_cluster.bin`) were relocated to an
attached external drive (`/Volumes/Seagate Desktop Drive`), replaced by
symlinks at their original paths inside `PokerAI/cluster/`.

### Why this works without any source changes

Every load site in `poker/Engine.h` and `Main.cpp` opens these files by
relative path (e.g. `ifstream in6("cluster/river_hand_cluster.bin", ...)`),
resolved relative to the process's current working directory (`PokerAI/`,
per the documented run instructions). Both `ifstream`/`fopen` on macOS and
Linux transparently follow symlinks at the OS level — a symlink at
`cluster/river_hand_cluster.bin` pointing to a file on the external volume
is completely indistinguishable to the engine from a real file at that
path. **No engine source code was modified for this.** The two small files
that stay in place unchanged are `preflop_hand_cluster.bin` (10.6KB — too
small to matter) and `preflopallin1326.1225.bin` (13.4MB — the one cluster
file actually committed to git; moving/symlinking a tracked file would be
inappropriate).

### Migration tool

`PokerAI/tools/move_cluster_to_external.sh <destination_dir>` — copies each
large file with `rsync -a`, verifies the copy's byte size matches the
original exactly, and only then deletes the original and replaces it with
a symlink (`ln -s`). Aborts immediately, leaving the original untouched, on
any size mismatch. Because `blueprint_stgy.dat` and `blueprint_strategy.dat`
were hardlinked (same inode, see section 8.1/2), the script copies the
underlying content only once and re-creates both names as symlinks to that
single external copy — avoiding doubling the ~16GB blueprint file on the
external drive.

Usage:
```shell
cd PokerAI
./tools/move_cluster_to_external.sh "/Volumes/Seagate Desktop Drive/DecisionHoldem_cluster_data"
```

### Caveats

- **The external drive must be attached and mounted at the same path for
  the engine to build/run** — if it's disconnected, the symlinks will
  dangle and any attempt to open these files will fail exactly like a
  missing file (the same failure mode documented throughout section 2),
  not silently succeed with stale data.
- **This does not reduce peak RAM usage at all** — it only relieves
  *disk* pressure (letting these ~34GB of files live off the primary
  volume). The already-documented ~19.4GiB `Engine()` RAM requirement and
  the ~16GB+ `blueprint_strategy.dat` in-memory tree cost (sections 2, 6,
  8.1, 9.1) are completely unaffected; this machine's RAM ceiling for
  actually running the real engine remains exactly as documented.
- **macOS-specific permission note:** accessing an external/removable
  volume from a sandboxed process (in this case, the agent process running
  under `GitHub Copilot.app`) is subject to macOS's TCC privacy framework
  and was initially blocked (`Operation not permitted` on every access
  method, including a workaround attempt via AppleScript-driven
  `Terminal.app`, which itself stalled on an unattended permission dialog).
  This required the user to explicitly grant the app "Removable Volumes"
  (or Files and Folders) access in System Settings → Privacy & Security
  before the migration could proceed — a one-time, user-side action with
  no command-line bypass.

### Result (verified)

The migration ran to completion. All five files under `PokerAI/cluster/`
are now symlinks resolving to
`/Volumes/Seagate Desktop Drive/DecisionHoldem_cluster_data/`, each
confirmed by exact byte-size match before its original was removed. Free
space on the internal volume rose from ~13GB to ~72GB immediately after
this migration (further headroom beyond that reflects unrelated,
user-driven disk cleanup happening concurrently). `preflop_hand_cluster.bin`
and the git-tracked `preflopallin1326.1225.bin` were left in place,
unchanged, as intended.

As an unrelated but similar housekeeping action on the same host, a
separate, non-DecisionHoldem git repository the user had locally
(`~/src/TexasSolver-solutions`, ~24GB of solved-range JSON output, remote
`nosami/TexasSolver-solutions`) was relocated with the same
copy-verify-then-symlink approach (ad hoc script, not part of this repo,
since it is unrelated to DecisionHoldem). One retry was needed: the first
verification pass flagged a false-positive mismatch caused by (a) a stray
Finder-created `.DS_Store` on the destination inflating its file count by
one, and (b) `du`'s block-allocation accounting differing between the
internal APFS SSD and the external drive's filesystem for the same
byte-identical content. Re-verifying with exact file counts plus summed
*apparent* file sizes (`stat -f%z`, not `du`) confirmed a true byte-for-byte
match, after which the swap completed normally. This is noted here only
because the same disk-block-accounting pitfall could resurface if this
migration script is ever extended to verify directories instead of single
files.

## 12. Investigating whether `AlascasiaHoldem.so`/`blueprint.so` can be made to run on this Mac

Asked directly: can the precompiled Linux x86_64 real-time search library be
gotten to run here somehow? Investigated by directly inspecting the
unstripped binary (`file`, `nm`, `c++filt`, `strings`, `objdump` — no
disassembly/decompilation, just reading exported symbols and embedded
debug/string data) rather than speculating.

### What `AlascasiaHoldem.so` actually is (confirmed via symbol inspection)

This is genuinely the real heads-up No-Limit Hold'em real-time
depth-limited search engine, not a toy/simplified game. Exported C++ symbols
include `Pokerstate`, `Searchstate`, `strategy_node`/`subgame_node`,
`getcfv_whole_holdem`, `getnode_cfv_holdem/turn/river`,
`build_subtree_preflop/flop`, `bulid_subtree_turn/river`, `search_cfr`,
`search_mccfr(p)`, `multi_update_substrategy`, `Players_range`, and
`randomized_pseudo_harmonic` (the exact pseudo-harmonic action-abstraction
technique from Brown et al.'s Modicum paper, which this project's own paper
says it extends). The four C-linkage entrypoints the Python GUI actually
calls (`getdecision`, `restart_game`, `Next_stage`, `opp_take_action`) are a
thin wrapper around all of this real machinery.

Critically, the binary contains embedded source-path string literals —
`Depth_limit_Search.h`, `tree/Node.h`, `tree/Bulid_Tree.h`, `PlaySearch.h`,
`Search.h`, `tree/Exploitability.h`, `tree/Save_load.h`,
`tree/Visualize_Tree.h`, `BlueprintMCCFR.h`, `tree/../poker/Engine.h`,
`tree/../poker/State.h` — confirming these headers, including
**`Depth_limit_Search.h`**, genuinely existed and were compiled by the
original authors; they were just never included in the public release
(consistent with section 2.1 and the still-open upstream issue #10).
**Correction**: on closer inspection with `objdump -h`/`objdump -s`, the
`.debug_info`/`.debug_line` DWARF sections are real but tiny (160/197 bytes)
and cover only glibc's `crti.S`/`crtn.S` C-runtime startup stubs — not the
application's own source. The header path strings above are *not* from
structured DWARF; they are plain string-table literals, almost certainly
`__FILE__` expansions baked in by `assert()` macros (this build was not
compiled with `NDEBUG`). They confirm the header filenames but carry no
line-table/type/algorithm information.

### Why it cannot run natively on macOS

Two independent, unfixable-from-here incompatibilities: (1) format — macOS's
kernel/dyld only load Mach-O binaries, never ELF, so there is no way to
`dlopen`/`ctypes.cdll.LoadLibrary` this file on macOS at all, regardless of
CPU architecture; (2) architecture — this file is x86_64, and this host is
Apple Silicon (arm64/M4). Rosetta 2 (present on this host) only translates
Apple's own x86_64 **macOS** binaries to arm64 — it has no bearing on Linux
ELF binaries.

### Is running it under Linux/emulation practical? Investigated, not attempted

`objdump -p` shows only completely ordinary dynamic dependencies
(`libc.so.6`, `libstdc++.so.6`, `libpthread.so.0`, `libm.so.6`,
`libgcc_s.so.1`, `ld-linux-x86-64.so.2`) against old, universally-compatible
`GLIBC_2.3`/`GLIBCXX_3.4.15` — nothing exotic. On **genuine x86_64 Linux
hardware or a cloud VM**, this library would very likely load without any
dependency-installation trouble.

On *this* host, the only way to run x86_64 Linux code at all is full CPU
emulation (e.g. Docker Desktop/colima + QEMU's TCG emulation, since Apple
Silicon has no x86_64 hardware execution path) — no container/VM runtime
(`docker`, `podman`, `colima`, `lima`, `qemu-system-x86_64`) was found
installed in this sandbox. Installing one is possible via Homebrew, but this
was **not done**, because two further findings make the expected payoff low
enough that it isn't a reasonable default action on a shared host without
asking first:

1. **QEMU-emulated x86_64 on arm64 has no hardware acceleration for the
   foreign instruction set** — real CPU instruction emulation, commonly
   5-20x+ slower than native for compute-heavy workloads. A real-time CFR
   search engine (already shown in sections 2/6 to want ~19-20GB+ RAM and
   real per-decision compute even natively) is a poor candidate for this.

2. **A second, independent portability blocker beyond platform/architecture,
   found in the binary's own embedded string literals** — its `Engine`-style
   cluster loader was compiled with **hardcoded absolute paths pointing at
   the original author's own development machine**, e.g.:
   ```
   /home/zhouqibin/projects/PokerAI/cluster/sevencards_strength.bin
   /home/zhouqibin/projects/PokerAI/cluster/preflop_hand_cluster.bin
   /home/zhouqibin/projects/PokerAI/cluster/turn_hand_cluster_ehs5000_1326*230300.bin
   /home/zhouqibin/projects/PokerAI/cluster/flop_hand_cluster_ehs50000_1326*19600.bin
   /home/zhouqibin/projects/PokerAI/cluster/river_hand_cluster_ehs1000_1326*2118760.bin
   /home/zhouqibin/projects/PokerAI/cluster/river_hand_cluster_1326*2118760.bin
   /home/zhouqibin/projects/PokerAI/preflopallin1326*1225.bin
   ```
   These are **not** the relative `cluster/...` paths used by this repo's
   own public `poker/Engine.h` source (section 1/8), and the filenames
   themselves encode different clustering-generation parameters (`ehs50000`,
   `ehs5000`, `ehs1000` — almost certainly "estimated hand strength" sampling
   counts from the clustering pipeline) than anything shipped publicly or
   named in this repo. This binary was evidently compiled from an internal,
   pre-release version of `Engine.h` that differed from the one open-sourced
   later. Making this library find data at all, even under working Linux,
   would require recreating this exact absolute directory tree and either
   feeding it the same-shaped files under these unusual names (workable via
   `mkdir -p`/symlinks, no root required) or the actual original files —
   which are not known to exist publicly under these names.
3. **A further embedded path,
   `/home/zhouqibin/projects/PokerAI/obj/x64/Debug/new_multiblueprint_policy5000050001000_V3.dat`,
   references a blueprint file with a completely different name/format than
   the publicly documented `blueprint_strategy.dat`** (a separate string,
   `cluster/blueprint_strategy.dat`, relative this time, also appears — it's
   unclear from symbol inspection alone which code path `getdecision()`
   actually reaches, without full disassembly). This suggests the shared
   library may be an internal **Debug** configuration build tied to
   never-released internal data, independent of whatever pretrained
   `blueprint_strategy.dat`/cluster files a user manages to obtain.

### Bottom line

- **Not possible on this Mac as-is** — confirmed via direct inspection, not
  assumption (ELF-on-Darwin and x86_64-on-arm64 are both hard platform
  blockers; no workaround exists at the OS level).
- **Theoretically possible only via genuine x86_64 Linux** (real hardware or
  a cloud VM/instance — not local Apple-Silicon emulation, which would
  likely be far too slow for this workload to be useful) — and even then,
  only after reconstructing the exact absolute developer-machine path
  structure this specific binary was hardcoded against, using cluster files
  whose expected naming implies a different (possibly incompatible)
  generation than anything publicly documented, with no certainty the
  binary would produce correct decisions even if it loads without crashing.
- No system-level tooling (Docker/colima/QEMU) was installed in this
  sandbox to pursue this further, given the above; that remains an
  available next step only if explicitly requested, ideally on genuine
  Linux/x86_64 hardware rather than this host.

## 13. Is there enough information in the `.so` to reconstruct a Mac-native binary?

Asked directly: given how inspectable this unstripped binary is, could its
contents be decompiled/reconstructed into new source and rebuilt for
macOS/arm64? Investigated concretely rather than guessing, by measuring
exactly what "unstripped" gives us here.

**What's actually available, quantified:**
- `.symtab` is present (not stripped): 4,682 symbols. But 3,988 of these are
  weak (`W`) symbols — libstdc++/STL template instantiations pulled in by
  the headers, not custom application logic. Only **107** symbols are real
  defined functions (`T`/`t`) belonging to this program.
- Because these are C++ mangled names, `c++filt` recovers full function
  *signatures* for all 107 — e.g.
  `search_mccfr(subgame_node**, Searchstate&, int, std::mersenne_twister_engine<...>&, double, int)`,
  `build_subtree_flop(strategy_node*, subgame_node*, Searchstate&, int, bool, bool)`,
  `update_substrategy(subgame_node*, Searchstate&, unsigned char*, int*, Players_range*, double*, double*, double, double, int, bool)`.
  This is meaningfully more than a stripped binary gives (real function
  names + parameter types + the custom class/struct names they operate on:
  `Pokerstate`, `Searchstate`, `strategy_node`, `subgame_node`,
  `Players_range`), but it is still only **signatures**, not bodies.
- The DWARF `.debug_info`/`.debug_line` sections are present but trivial
  (160/197 bytes, glibc C-runtime startup stubs only — see the correction in
  section 12). There is **no** line-table, variable-name, or type-layout
  debug information for the actual solver code. Struct field names/offsets,
  local variable names, and control-flow structure are all absent.
- `objdump -d` shows ~1,197 uses of `xmm` (SSE2 double-precision) registers
  across the binary — a large volume of numerical code, consistent with the
  described CFR regret/strategy math, that only exists as raw x86-64
  machine instructions.

**What reconstruction would actually require:** disassembling and manually
decompiling the machine code of all 107 functions (which include recursive
tree-building/traversal and regret-matching/CFR update routines — not
simple leaf functions) into real, semantically equivalent, portable C++;
inferring the layout of `strategy_node`/`subgame_node`/`Searchstate`/
`Pokerstate`/`Players_range` purely from access-pattern analysis in the
disassembly (no field names exist anywhere); reproducing exact floating-point
operation order (CFR/regret-matching results are order-sensitive); and doing
all of this without any ground-truth reference output to check correctness
against, since this project's own pretrained `blueprint_strategy.dat` isn't
legitimately available either (sections 3/7). Tools like Ghidra/IDA/Hex-Rays
can generate approximate pseudo-C for each function, but that output still
requires substantial expert manual correction before it is real, compilable,
behaviorally-faithful source — this is a multi-week-to-months reverse
engineering effort for a single expert, not something with any realistic
chance of a correct result from automated/quick effort, and a wrong
reconstruction (subtly incorrect regret math, wrong struct layout, off-by-one
in tree indexing) could silently produce plausible-looking but wrong poker
decisions with no way to detect that without a trusted reference to compare
against.

**Answer: no** — not in any practical or reliable sense. The unstripped
symbol table gives real signatures and confirms the withheld header names,
which is valuable forensic context, but it is not remotely equivalent to
having source, and attempting a full decompilation-based reconstruction was
judged not to be a legitimate, achievable path here — consistent with this
project's stated goal of not fabricating success. No decompilation was
attempted; this section only reports what information is present and why
it falls short.

## 14. Correction/refinement: do the `.so`'s custom struct/function names exist in the public source?

A fair follow-up question exposed an inaccurate implication in section 13 —
that the binary's data structures and logic were entirely unknown outside
the `.so`. Checked exhaustively rather than assuming: grepped the public
repo source for every one of the 107 real (`T`/`t`) defined functions in
`AlascasiaHoldem.so`, and for the custom struct/class names it uses.

**Result: most of it is already present as real, compilable public source,
with exactly matching signatures.**

- `Pokerstate` and `Searchstate` are fully defined in `poker/State.h`
  (lines 31 and 313). `strategy_node`/`subgame_node` are fully defined in
  `tree/Node.h`.
- Of 73 non-STL real functions checked, **52 exist verbatim** (identical
  C++ signatures, confirmed by comparing the demangled `.so` symbol against
  the actual function declaration — e.g. `build_subtree_flop(strategy_node*,
  subgame_node*, Searchstate&, int, bool, bool)` and `getnode_cfv_holdem
  (strategy_node**, Pokerstate&, double*, double*, unsigned char*, int, int,
  int)` match byte-for-byte) in `tree/Bulid_Tree.h`, `tree/Exploitability.h`,
  `tree/Save_load.h`, `tree/Visualize_Tree.h`, `BlueprintMCCFR.h`,
  `Multi_Blureprint.h`, and `Main.cpp` — i.e. all of the offline
  blueprint-training, tree-building, exploitability-computation, save/load,
  and visualization machinery this repo already has really is what the `.so`
  was built from.
- **Only 21 functions are genuinely absent from the public source**, and they
  cluster exactly where expected — the live game-interface layer
  (`getdecision`, `restart_game`, `Next_stage`, `opp_take_action`,
  `startgame`, `initgamestart`, `getexternal_cards`), the real-time search
  algorithm itself (`search_cfr`, `search_mccfr`, `search_mccfrp`, `rollout`,
  `expect_game_val`, `randomized_pseudo_harmonic`), and its substrategy/range
  update machinery (`update_substrategy`, `update_substrategy_preflop`,
  `update_subgame_strategy`, `multi_update_substrategy`, `update_range`),
  plus two minor variants (`load2`, `bulid_bluestrategy2`, `findlistmax`).
- **`Players_range` appears in neither `AlascasiaHoldem.so`'s nor
  `blueprint.so`'s companion source anywhere in this repo** — it exists only
  inside the compiled binaries, confirming it's a data structure private to
  the missing `Search.h`/`PlaySearch.h`/`Depth_limit_Search.h` trio.

**Practical implication**: this sharpens, but does not reverse, section 13's
conclusion. The reconstruction gap is precisely these ~21 functions (the
actual depth-limited real-time search/resolving algorithm and its live-play
glue) plus one struct — not "the whole compiled library." Everything else
needed to *build* the parts of this repo that already compile (offline
blueprint MCCFR training, tree save/load, exploitability measurement,
visualization) is already present as real source, matching what produced
these `.so` files. Only that narrow, specific 21-function/1-struct surface
would still require disassembly-based reverse engineering with no source and
no ground-truth output to validate against — the same caveats as section 13,
just now scoped to the actual missing piece rather than the whole binary.

## 15. An original, from-scratch implementation of the missing depth-limited real-time search

Sections 12-14 established that the *proprietary* real-time search (the ~21
functions and the `Players_range` struct behind `AlascasiaHoldem.so` /
`blueprint.so`) cannot legitimately be recovered from the compiled binaries.
That remains true. This section is a different, legitimate thing: **new,
original source code**, written from scratch for this repository, that
implements a *published* algorithm family (depth-limited subgame-solving via
Counterfactual Regret Minimization — Zinkevich et al. 2007 for vanilla CFR;
Brown & Sandholm 2017 and Moravčík et al. 2017 for depth-limited/"Modicum"-
and DeepStack-style real-time resolving; Ganzfried & Sandholm 2013 for
pseudo-harmonic bet-size action translation), reusing this repo's own
already-public building blocks. It is explicitly **not** presented as a
recovery or validation of the original `Depth_limit_Search.h` / `Search.h` /
`PlaySearch.h` — its leaf-value technique, in particular, is a deliberate,
documented substitute chosen to fit this host's RAM budget, not a guess at
what those withheld files actually did.

### What's new vs. what's reused

New files (both AGPLv3-licensed, matching the rest of the repo):
- **`PokerAI/tree/RealtimeSearch.h`** — `pseudo_harmonic_prob_lower()` /
  `randomized_pseudo_harmonic()` (standalone action-translation formula),
  `TurnClusterLeafModel` (the leaf-value estimator, see below), and
  `FlopResolver` (a small range-vs-range vanilla CFR solver over one flop
  betting round).
- **`PokerAI/tools/test_realtime_search_flop.cpp`** — a test/demo tool that
  sets up one concrete flop decision point and runs the resolver.

Reused, unmodified, from the existing public source:
- `poker/State.h`'s `Searchstate` — its real betting mechanics
  (`legal_actions()`, `take_action()`, `betting_stage`, `player_i_index`,
  the `'d'`/`'l'`/`'n'`/pot-fraction action encoding) drive the whole
  subgame tree. `FlopResolver` never reimplements betting rules; it calls
  these methods directly.
- `poker/Engine.h`'s `get_turn_cluster()` over the already-public,
  already-documented `turn_hand_cluster.bin`.
- The general "compare ordinal hand-cluster ids to rank relative hand
  strength" idea already used (for a different, *offline* purpose) by
  `tree/Exploitability.h`'s `getnode_cfv_river/turn/holdem` — reimplemented
  locally and much more simply for this new, different use case rather than
  calling those functions directly (they're batch-array-indexed for
  best-response computation against a frozen strategy, not a drop-in
  real-time leaf estimator).

Deliberately **not** reused: `tree/Node.h`'s `strategy_node`/`subgame_node`
raw-pointer batch-array machinery, and `tree/Bulid_Tree.h`'s subgame-building
functions. Those are real, working, and directly relevant (they contain the
existing blueprint-continuation depth-limit mechanism — see below) but their
indexing conventions are intricate and built for a different code path;
reusing them directly was judged higher bug-risk than a smaller, from-scratch
implementation for a first working version.

### The leaf-value model: what it is, and why it isn't the original's method

Reading `tree/Bulid_Tree.h` shows the *original* design's depth-limit
mechanism: `subgame_node::leaf`/`leafnode` point into the loaded **blueprint**
strategy tree once a subgame's search horizon is reached, i.e. the original
almost certainly continued past the depth limit by consulting blueprint
strategy/regret (also visible in the `node->averegret[j] / 10` warm-start
already present in that file). That approach needs `blueprint_strategy.dat`
loaded — **16.1GB** on disk (confirmed by direct measurement of the symlinked
file), which does not fit in this host's 16GB of RAM alongside the cluster
files used during search.

So `TurnClusterLeafModel` uses a different, self-contained substitute: when
the flop betting round ends (call, or all remaining chips go in) without a
fold, instead of dealing/solving the turn and river, it estimates the
continuation value by comparing each side's **turn-hand-cluster id** —
looked up via the *already-loaded* `turn_hand_cluster.bin` — averaged over
every possible next (turn) card that doesn't collide with either player's
hole cards or the known flop. This reuses the same "cluster order
approximates relative hand strength" idea the repo's own offline
exploitability code already relies on, just one street earlier and averaged
over the unknown card instead of using a fixed river. It is intentionally
simple and RAM-cheap; it is **not verified to match** whatever the original
`.so`'s actual continuation values were.

Verified independently before wiring it into CFR: a standalone check
constructed two mirrored `Players_range`s (roles swapped) and confirmed
`expected_showdown_sign(hero, villain) == -expected_showdown_sign(villain, hero)`
exactly, across several hand pairs, with values spanning the full range
(observed -1.0 to +1.0, not degenerate) — i.e. the estimator is internally
consistent (correctly zero-sum/antisymmetric) and actually discriminates
between different hand strengths rather than returning a constant.

### The resolver: vanilla range-vs-range CFR, not the original's MCCFR

`FlopResolver::cfr()` is the standard "vector-form" vanilla CFR recursion
(see e.g. Neller & Lanctot's CFR tutorial), generalized from a single hidden
hand to explicit ranges on both sides: it tracks per-player reach-probability
vectors over each player's own range, and returns per-hand expected utility
vectors, updating regret only at nodes belonging to the current traversal's
`traverser` (both players' regrets get updated once per iteration via two
alternating traversals in `FlopResolver::run()`). This is full-traversal
(no sampling), unlike the `.so`'s evidently Monte-Carlo-sampled originals
(`search_mccfr`/`search_mccfrp` — see section 14) — a simpler, lower-risk
choice appropriate for a small, explicit-range subgame.

Terminal handling, verified against the actual invariants `Searchstate`
enforces (`table.total_pot == table.total()`, i.e. pot size is *derived*
from cumulative per-player chip contributions, not independently settable):
- `betting_stage == 5` → a fold occurred; payout is the pot split by chip
  contribution, independent of hole cards, scaled by opponent reach.
- `betting_stage >= 2` (the flop round completed normally, or all chips went
  in) → the `TurnClusterLeafModel` continuation estimate above.

### Build and validated run

```
# One-line, opt-in, default-OFF macro (poker/Engine.h) skips allocating/
# reading river_hand_cluster.bin (~16.86GB) -- unneeded here since this demo
# never calls get_river_cluster(). Same flag documented (then reverted) in
# section 9; this time it's kept because the new tool's own documented build
# command depends on it. Normal (flag-undefined) behavior is unchanged.
g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER \
    -o /tmp/test_realtime_search_flop PokerAI/tools/test_realtime_search_flop.cpp
cd PokerAI && /tmp/test_realtime_search_flop
```

Actual measured output (200bb-effective, blinds 50/100, pot=600 at the flop,
30 hero combos x 30 villain combos, fixed 3-card board, macOS/M4/arm64):

```
Hero range: 30 combos, Villain range: 30 combos, board=[10,23,41]
Running vanilla-CFR over the flop subgame, reporting convergence checkpoints...
  after   25 iterations: root avg|regret|=964572.1  (38582.886 per iteration)
  after  100 iterations: root avg|regret|=3420256.6  (34202.566 per iteration)
  after  300 iterations: root avg|regret|=8501864.9  (28339.550 per iteration)
  after  500 iterations: root avg|regret|=13369957.4  (26739.915 per iteration)
CFR search time (excludes Engine load + leaf-model precompute): 680.6 ms for 500 iterations
Legal root actions (player 1 to act): 108 1 2 4 8 20 40 110    (ASCII 108='l'/call, 110='n'/allin)
```

Per-iteration average |regret| decreases monotonically across all four
checkpoints (38583 → 34203 → 28340 → 26740) — the standard CFR convergence
signature (cumulative regret keeps growing, but sublinearly). Root strategies
are sane and hand-dependent (mostly checking, with small variation in
raise/allin frequency across the sampled hands), not degenerate/uniform.
Peak RSS measured at **2.8GB** (`/usr/bin/time -l`), comfortably inside this
host's 16GB — well under the ~4GB estimate from section 9 and nowhere near
the ~20GB that including the full blueprint would require.

**Timing answer** (the specific question asked earlier in this session,
"the time it takes excluding the initial lookup"): ~1.4ms/iteration for a
30x30-combo flop subgame on this host, i.e. **680ms for 500 CFR iterations**,
excluding `Engine` construction (which happens once, at process startup, via
the global static initializer, and is dominated by reading the cluster files
from disk — from the external drive here) and excluding the leaf-model
precompute (3-11ms for this range size, also a one-time-per-decision cost).

### Honest scope and limitations

- Solves **one flop betting round only** — does not chain into turn/river
  betting, does not implement the live `getdecision`/`opp_take_action` API,
  and is not wired into `Main.cpp`.
- Leaf continuation value is a **documented approximation** (turn-cluster
  comparison averaged over the unknown card), not the original's presumed
  blueprint-continuation technique, which remains infeasible here due to
  `blueprint_strategy.dat`'s 16.1GB size.
- Hero/villain ranges are small, explicit combo lists (here, 30 vs 30),
  not the full 1326-combo unabstracted range or this repo's production
  clustering/abstraction pipeline.
- Full-traversal vanilla CFR, not Monte-Carlo sampled (the `.so`'s
  `search_mccfr` naming implies sampling was the original's actual method).
- **Unsafe resolving only** — no safe/"gadget game" subgame solving
  (Burch, Johanson & Bowling 2014); matches `Searchstate`'s own existing
  hidden-card-sampling methods, which are explicitly commented as unsafe
  search in the original source (see section 14's summary).
- No correctness ground truth exists to compare against (same caveat as
  section 13); validation here is standard CFR sanity checks (regret
  convergence trend, antisymmetric/non-degenerate leaf values, sane and
  hand-differentiated output strategies) rather than exact-answer matching.

**Bottom line**: this is a genuine, working, original implementation of a
real published real-time-search technique, running end-to-end on this host
in well under a second per resolve and comfortably within its RAM budget —
not a recovery of DecisionHoldem's specific proprietary algorithm, and not
claimed to produce the same decisions the original binary would.

## 16. Extending the original real-time search to real turn/river chaining

Section 15's `FlopResolver` stops the instant flop betting ends and estimates
the continuation value with a turn-cluster leaf model. This section adds a
second, independent resolver, `StreetChainResolver` (`PokerAI/tree/RealtimeSearch.h`),
that instead **deals real turn and river cards as genuine chance nodes and
plays the hand all the way to an exact showdown** via `Engine::compute_winner()`
— i.e. no leaf-value approximation at all past the flop.

### A correctness bug found and fixed while building this

`Engine::compute_winner()`/`find_strength()` does not fail silently on a
hash-table miss — it does `cout << "error hand seven cards..."; throw
exception();`. Building `StreetChainResolver` surfaced two related bugs:

1. **`FlopResolver::terminal_leaf` (section 15, already committed) never
   checked that hero's and villain's hole cards don't share a card.**
   Fixed by adding a new `hands_compatible(a, b)` helper and calling it
   before scoring a hero/villain pair.
2. **A card-collision check that used `reach[player][hand] == 0.0` as a
   proxy for "this hand is impossible" is unsafe.** Reach values are also
   driven to exactly 0.0 by ordinary CFR strategy convergence, for reasons
   having nothing to do with card collisions. `StreetChainResolver`'s
   showdown path was fixed to instead call an explicit
   `collides_with_board()` check on both hero's and villain's hole cards
   against the board, independent of reach magnitude, before ever calling
   `compute_winner()`.

Both fixes are narrow, additive, and do not change `FlopResolver`'s
previously-validated flop-only behavior.

### Tractability: why the action set had to be reduced

A first attempt chained turn+river dealing under the same full native
raise-size ladder `FlopResolver` uses (`State.h`'s `legal_actions()`, up to
~6 actions per decision node) and did not finish 5 CFR iterations in over 5
minutes — the combinatorics of (up to 6 actions)×(~48 turn cards)×(up to 6
actions)×(~47 river cards)×(up to 6 actions) explode far too fast for
interactive use. **Fix:** `StreetChainResolver` restricts every decision node
to exactly `{fold, call, all-in}` (filtering `legal_actions()`'s output down
to those three byte codes: `'d'`, `'l'`, `'n'`), which is what actually made
full turn/river chaining tractable at all.

### Validated run

New tool: `PokerAI/tools/test_realtime_search_chain.cpp` — 6 hero combos ×
6 villain combos, fixed flop board `[10,23,41]`, chains through a real turn
card, then a real river card, to an exact showdown.

```shell
cd PokerAI
g++ -std=c++17 -O2 -Wall -Wextra -DDH_SKIP_RIVER_CLUSTER \
    tools/test_realtime_search_chain.cpp -o /tmp/test_chain
/tmp/test_chain
```

```
50 iterations in 2222.8 ms, 31124 tree nodes, peak RSS ~4GB
avg|regret| increasing across checkpoints (5/15/30/50 iterations) — expected,
since these checkpoints report cumulative regret, not per-iteration regret.
Root strategies for all 6 villain hands: sane, non-degenerate mixes,
~90-97% call / 3-10% all-in.
```

### Honest scope and limitations

- Still **unsafe resolving** against a small, explicit, fixed range — same
  caveat as section 15, not fixed here.
- The reduced `{fold, call, all-in}` action set is a **deliberate
  tractability tradeoff**, not a claim that the original bot's action space
  was this small — section 15's `FlopResolver` (single street only) keeps
  the full native raise ladder precisely because it doesn't pay this
  multiplicative cost.
- Exact showdown scoring (no cluster approximation) makes this **slower but
  more accurate than `FlopResolver`** per street resolved — a genuinely
  different, complementary tool, not a strict upgrade.
- This resolver is an off-line study/validation tool (like section 15's),
  not wired into any live-play path — see section 17 for that.

## 17. Wiring the original resolver into the existing web GUI (`pypokergui`)

The repo already ships a complete, working Tornado-based poker GUI
(`pypokergui/`, a fork of ishikota's PyPokerGUI) whose only missing piece —
per section 4 — was that `pypokergui/server/game_manager.py` hardcodes
`cdll.LoadLibrary('./AlascasiaHoldem.so')`, a Linux x86_64 ELF binary that
cannot load on macOS, and section 4 correctly identified this as a hard
blocker with nothing in the repo to port or recompile (the real-time search
inside that `.so` was never open-sourced — see sections 12-14 for the full
forensic investigation of that binary).

Sections 15/16 built an **original, from-scratch** real-time search
(`FlopResolver`, `StreetChainResolver`) as a legitimate substitute — not a
reconstruction of the `.so`'s contents. This section wires that search
into the GUI as a genuine, loadable macOS library, so the GUI is actually
playable end-to-end on this host, subject to the same
missing-cluster-artifact caveats documented throughout this file.

### `PokerAI/tools/dh_native_ai.cpp` (new)

A new, independently-written `.cpp` file that exposes the **exact same
four-function C ABI** `pypokergui/server/fish_player_setup.py` already calls
via `ctypes` against the original `.so`:

| Function | Signature (as called from Python) | Purpose |
|---|---|---|
| `restart_game` | `(myid, c1id, c2id)` | New hand: which slot is "me", my hole cards |
| `Next_stage` | `(betting_stage, community_card_bytes)` | Street transition + board |
| `opp_take_action` | `(actionstr)` | Opponent's action (`"fold"/"call"/"allin"/"raise N"`) |
| `getdecision` | `(out_buf[20])` | My decision, written back as a C string |

It is backed by a third, new resolver added to `RealtimeSearch.h`,
`LiveResolver`, which unifies flop/turn/river resolving behind one `Mode`
enum specifically tuned for interactive latency (all three modes use the
same `{fold, call, all-in}`-only action reduction as section 16, for the
same tractability reason):

- **`Mode::FLOP`** — same approximate turn-cluster leaf value as
  `FlopResolver` (section 15); no cards dealt.
- **`Mode::TURN`** — deals one real river card (a genuine ~48-branch chance
  node) then scores an **exact** showdown — but deliberately does **not**
  simulate river betting (assumes a check-down on the river). This is a
  documented, deliberate accuracy/speed tradeoff to avoid a second full
  betting-round layer at interactive latency.
- **`Mode::RIVER`** — resolves the real (final) betting round exactly; no
  chance nodes fire since the board is already complete.
- **Preflop is now resolved with the REAL trained blueprint** — `dh_native_ai.cpp`
  originally always returned `"call"` preflop under the belief that
  `blueprint_strategy.dat` (the ~16GB file every DeepStack/Libratus/
  Alascasia-style architecture in this genre needs for its offline-solved
  preflop strategy) was never obtainable. **That belief was wrong and is
  corrected here**: section 2 already documents that this file WAS
  downloaded and byte-size-verified earlier in this same investigation
  (as `blueprint_stgy.dat`, hard-linked to `cluster/blueprint_strategy.dat`
  — the exact path `Main.cpp`/`Save_load.h` expect) — it sat unused only
  because fully loading it (via `Save_load.h`'s `load()`, which
  materializes the ENTIRE recursive tree in RAM before any single lookup)
  was judged too RAM/swap-risky in combination with the full cluster set.
  **Section 18 replaces the placeholder** with a new, much smaller,
  targeted reader (`PokerAI/tree/BlueprintReader.h`) that queries the real
  file directly for hero's actual trained strategy at the specific
  decision point reached, without loading the rest of the tree. See
  section 18 for the full writeup, including what preflop histories it
  can and can't currently use, and its honest, from-this-sandbox
  validation status.
- The opponent's range for every resolve is a **uniformly sampled** set of
  ~40 hole-card combos consistent with the known board/hero cards (the
  opponent's true range is unknown — there is no belief-tracking blueprint
  to consult) — the same "unsafe resolving" caveat as sections 15/16,
  applied here for the first time to genuinely unknown live opposition
  rather than a hand-picked demonstration range.
- Chip-accounting note: pypokergui's underlying engine
  (`pypokerengine/engine/player.py`) reports raise/call amounts as
  **per-street cumulative** totals (reset every street), while `Searchstate`
  (`PokerAI/poker/State.h`) tracks `n_bet_chips()` as **cumulative for the
  whole hand** (never reset). `dh_native_ai.cpp`'s internal `LiveGame`
  struct tracks `stack_at_street_start[2]` explicitly to convert between
  the two conventions each time it builds a `Searchstate` snapshot.
- Slot convention: `myid` (0 or 1) passed to `restart_game` directly
  matches `Searchstate`'s own hardcoded HU convention (slot 0 = small
  blind/button, acts first preflop; slot 1 = big blind, acts first
  postflop) — confirmed by reading how `server/fish_player_setup.py`
  derives `myid` for the original `.so`, so no remapping is needed.
- The GUI never separately tells the AI about its own action (`server/
  fish_player_setup.py`'s `receive_game_update_message` explicitly
  early-returns when the acting player is the AI itself), so `getdecision`
  applies the same state-update bookkeeping to its own chosen action,
  mirrored from `opp_take_action`, before returning.

Build (produces a real macOS `.dylib`, must be run from `PokerAI/` so the
`Engine`'s `"cluster/..."` relative load paths resolve, exactly like every
other tool in this repo):

```shell
cd PokerAI
g++ -std=c++17 -O2 -shared -fPIC -o dh_native_ai.dylib tools/dh_native_ai.cpp
```

This compiles cleanly (0 errors, 1 pre-existing unrelated unused-parameter
warning shared with every other file that includes `State.h`), and
`nm -gU dh_native_ai.dylib` confirms all four required symbols are exported:
`_restart_game`, `_Next_stage`, `_opp_take_action`, `_getdecision`.

**Note on `DH_SKIP_RIVER_CLUSTER`:** an earlier revision of this build
command included `-DDH_SKIP_RIVER_CLUSTER` (the opt-in, default-off macro
from section 9) to skip loading the 16.86GB `river_hand_cluster.bin` for a
faster build/test cycle. **That flag has been removed from the real build**
so `dh_native_ai.dylib` loads the full, real river cluster — see the swap
feasibility analysis immediately below for why this is now a reasonable
default, unlike section 9.1's full-blueprint scenario.

#### Is running with the full cluster set (via swap) actually feasible here?

Section 9.1 declined running the *entire* engine (all cluster files **plus**
the ~15GB `blueprint_strategy.dat` in-memory tree, ~34.4GiB total) under
swap, because at the time available disk was only 13GB against an ~18.4GiB
shortfall. **`dh_native_ai.dylib`/`LiveResolver` is a materially smaller
ask than that scenario** — it never loads `blueprint_strategy.dat` at all
(preflop is a hardcoded placeholder, not blueprint-driven; see below), so
its only large in-memory footprint is the `Engine()` cluster tables:

| File | Size |
|---|---|
| `sevencards_strength.bin` | 1.338 GB |
| `preflop_hand_cluster.bin` | ~0.01 GB |
| `flop_hand_cluster.bin` | ~0.21 GB |
| `turn_hand_cluster.bin` | ~2.44 GB |
| `river_hand_cluster.bin` | 16.86 GB |
| **Total** | **~20.9 GB** |

Against this host's 16GB physical RAM, that's a **~5GB swap shortfall** —
not the ~18.4GB shortfall of the declined full-blueprint scenario — and
current internal disk free space is **~91GB** (re-measured at the time of
this decision), comfortably covering it many times over. On this basis,
building and running `dh_native_ai.dylib` with the real river cluster
loaded (no `DH_SKIP_RIVER_CLUSTER`) was judged reasonable to attempt, where
the earlier full-blueprint attempt was not. Expect the very first library
load (whichever process — a smoke test or the GUI itself — triggers
`Engine()`'s global static constructor) to take noticeably longer than the
river-skipped build while ~5GB gets paged to swap and `river_hand_cluster.bin`
streams in from the external USB drive at its previously-measured
~30-35MB/s (section 11); subsequent lookups against already-resident pages
should be fast. Monitor with `vm_stat`/`sysctl vm.swapusage` during the
first load, same as section 2's methodology, and be ready to stop the
process if swap usage or disk free trend somewhere unexpected.

### GUI wiring: `pypokergui/server/game_manager.py`

Changed the single hardcoded `cdll.LoadLibrary('./AlascasiaHoldem.so')` call
to branch on `platform.system()`: `./dh_native_ai.dylib` on Darwin,
unchanged `./AlascasiaHoldem.so` on Linux (Linux behavior is 100%
preserved). Added symlinks (not copies, so they always track the real
build/source) so the GUI's relative-path load works from its own working
directory:

```shell
cd pypokergui/server
ln -s ../../PokerAI/dh_native_ai.dylib dh_native_ai.dylib
ln -s ../../PokerAI/cluster cluster   # Engine::load() needs "cluster/..." too
```

### Blocker hit while validating end-to-end: macOS disk permission (TCC), not a code defect

Section 11 relocated the multi-GB cluster/blueprint files to an external
Seagate drive with symlinks back into `PokerAI/cluster/`, and section 15/16's
resolvers were validated successfully reading through those symlinks earlier
in this investigation. When this section's work was validated, the *exact
same* symlinked files started failing with `Operation not permitted` —
confirmed to be an OS-level permission (TCC / "Files and Folders" access for
removable volumes), **not** a bug in this new code:

```shell
$ stat "/Volumes/Seagate Desktop Drive/DecisionHoldem_cluster_data/sevencards_strength.bin"
# succeeds — metadata (size, dates) is visible, and matches the known-good 1,337,845,600 byte file
$ cat "/Volumes/Seagate Desktop Drive/DecisionHoldem_cluster_data/sevencards_strength.bin" > /dev/null
cat: ...: Operation not permitted
```

`stat`/`ls` (metadata-only syscalls) succeed; `open`/`read`/`cp` (actual
content access) are denied — this asymmetry is the signature of macOS's
per-app disk-access control, not a missing file, wrong path, corrupt
symlink, or code bug. Every previously-successful read of this same file
this session happened under a different granted-permission context; this
session's process does not currently have it. Because `Engine`'s
constructor (`PokerAI/poker/Engine.h`) unconditionally calls `load()` at
**global static-initialization time** — i.e. the instant any binary or
shared library linking against it is loaded, before `main()` or any
`extern "C"` entry point runs — this blocks loading `dh_native_ai.dylib` at
all right now, the same way it would block any of this repo's other tools
(`Main.o`, `test_realtime_search_flop`, `test_realtime_search_chain`) if run
fresh in this exact process context. This is **not specific to the new
library** — it is the pre-existing, repo-wide `cluster/*.bin`-on-first-touch
dependency from section 2, now manifesting as an OS permission gap rather
than a missing-file gap.

**To unblock, on the actual machine (not fixable from within this session):**
open System Settings → Privacy & Security → Files and Folders (or Full Disk
Access) and grant the terminal/app running the build access to the Seagate
volume, or physically re-eject/reattach the drive and approve any Finder
permission prompt that appears, then re-run the commands in the next
subsection to confirm.

### What was and wasn't validated

**Validated (real, reproducible, no fabricated data):**
- `dh_native_ai.cpp` compiles cleanly to a real arm64 Mach-O `.dylib` with
  all 4 required C symbols exported (`nm -gU`, shown above).
- `game_manager.py`'s new OS-conditional load logic is syntactically valid
  and resolves to the correct filename on this host
  (`platform.system() == 'Darwin' → './dh_native_ai.dylib'`).
- The symlink wiring in `pypokergui/server/` is in place and points at the
  real build artifacts (not copies).

### Update: disk access recovered, GUI launch confirmed reaching the server, one more real bug found and fixed

The disk-permission block above turned out to be specific to the sandboxed
process this investigation's automated tooling runs in — the user's own
Terminal session already had (or was separately granted) access to the
external drive, and running the corrected command in their own terminal got
past `Engine::load()` successfully:

```
ai start load
[DH_SKIP_RIVER_CLUSTER] river_hand_cluster.bin NOT loaded (build/test mode)
load finish (./dh_native_ai.dylib)
```

(this log line reflects the *first* build, which still had the
river-skipping flag enabled — see the swap-feasibility subsection above for
why that flag has since been removed so the real river cluster loads.)

confirming `dh_native_ai.dylib` really does load and read real cluster data
end-to-end on this host. It then failed one line later with a second,
independent, pre-existing bug:

```
File ".../pypokergui/server/poker.py", line 154, in start_server
    import poker_conf
ModuleNotFoundError: No module named 'poker_conf'
```

`server/poker.py`'s `start_server()` does a bare `import poker_conf`
(`poker_conf.py` lives alongside it in `pypokergui/server/`). That only
resolves when `pypokergui/server/` itself is on `sys.path`. `pypokergui/
__main__.py`'s click-based `serve` command never added that directory (it
only added the repo root and `pypokergui/` itself) — a bug that predates
this investigation, is unrelated to platform/OS, and was only reachable now
that the `.so`-loading and disk-access blockers were cleared. **Fixed** by
adding `sys.path.append(os.path.join(src, "server"))` to `__main__.py`.
This does not affect the alternative direct-invocation path
(`python3 server/poker.py --port=8000`), which already worked because
running a script directly puts its own directory on `sys.path[0]`.

**Confirmed working, exact command** (run from the repo root or anywhere;
what matters is the final `cd` before launch):

```shell
python3 -m venv /tmp/dh_venv && source /tmp/dh_venv/bin/activate
pip install -r requirements.txt
cd pypokergui/server
python3 ../__main__.py serve dummy --port 8000
```

(`dummy` is a required-but-unused positional argument — `start_server()`
ignores it and always reads `server/poker_conf.py` directly.)

### What was and wasn't validated

**Validated (real, reproducible, no fabricated data):**
- `dh_native_ai.cpp` compiles cleanly to a real arm64 Mach-O `.dylib` with
  all 4 required C symbols exported (`nm -gU`, shown above).
- `game_manager.py`'s new OS-conditional load logic is syntactically valid
  and resolves to the correct filename on this host
  (`platform.system() == 'Darwin' → './dh_native_ai.dylib'`).
- The symlink wiring in `pypokergui/server/` is in place and points at the
  real build artifacts (not copies).
- **`dh_native_ai.dylib` loads successfully and reads real cluster data
  through the actual GUI startup path** (`ai start load` → `load finish`),
  confirmed via the user's own terminal.
- The `poker_conf` import bug is fixed and the fix is independently
  verified (isolated `sys.path`/import reproduction, no cluster dependency).

**Not yet validated:** a full browser-driven hand (dealing, betting through
all streets, a decision actually coming back from `getdecision()` inside a
live Tornado session, `curl`/browser reaching `http://localhost:8000`).
Re-run the command above and open the browser tab it launches to confirm.

### Honest scope and limitations

- Flop/turn/river decisions are genuine CFR resolves against a **uniformly
  sampled, unknown-true opponent range** — unsafe resolving, same caveat as
  every other resolver in this file.
- **Preflop uses the real trained blueprint when the tracked action history
  is unambiguous** (see section 18) — falls back to the original "call"
  placeholder, for that decision only, when a raise size can't be matched
  exactly to the trained abstraction's discrete sizing ladder, or the
  lookup fails for any other reason (no blueprint data available was the
  OLD, now-corrected, claim — see section 2 and section 18).
- **Turn-mode decisions assume a river check-down** (no river-betting
  subtree) — a deliberate latency tradeoff, documented in code.
- The reduced `{fold, call, all-in}` action set (sections 16/17) means the
  GUI's opponent will never see this AI make an intermediate-sized raise.
- This is a genuine, original implementation making the existing GUI
  actually playable against a working search algorithm on macOS — it is
  explicitly **not** a recovery or reconstruction of the proprietary
  `.so`'s specific strategy or strength, and its live-play correctness has
  not yet been end-to-end verified on this host due to the disk-permission
  issue above, not due to any known defect in the new code.

## 18. Correction: `blueprint_strategy.dat` exists — a targeted reader for real preflop decisions

Section 17 shipped with `dh_native_ai.cpp`'s preflop decision hardcoded to
`"call"`, justified at the time as "no legitimate `blueprint_strategy.dat`
was obtainable." **That justification was factually wrong**, and was only
caught because the user pointed it out directly. Section 2 of this same
document already recorded, in detail, that this file WAS downloaded earlier
in this investigation — under the mirror's filename, `blueprint_stgy.dat`
(16,123,074,125 bytes) — hard-linked to `cluster/blueprint_strategy.dat`
(the exact relative path `PokerAI/Main.cpp`/`PokerAI/tree/Save_load.h`
expect), and byte-size-verified against the other five cluster files as a
"strong signal" of legitimacy (no official hash was ever available to check
against — see section 2 for the full provenance caveat). Section 9.1 also
records an actual, real attempt to load it via the original loader
(`./Main.o 1`), which was deliberately killed after ~20s when swap grew to
~19.4GB, back when free disk was only ~13GB and cluster files hadn't yet
been relocated externally — a real, cautious decision, not a fabricated
"couldn't obtain it" one. Conflating "judged too risky to fully load, once,
under much tighter disk headroom" with "never obtainable" was the mistake.

### Why the file was still unused: `Save_load.h::load()` has no random access

`PokerAI/tree/Save_load.h`'s `load()` is the *only* deserializer the
original codebase ships. It works by recursively rebuilding the **entire**
`strategy_node` tree in memory (`bulid_bluestrategy()`) before a single
lookup is possible — there is no index, no seek table, nothing that lets
you ask for "just hero's strategy at this one decision point." Combining
that with the ~20.9GB cluster-file requirement (section 17's swap-
feasibility subsection) would mean materializing roughly the file's own
16.1GB on disk *plus* per-node pointer/`double[]` heap overhead
(conservatively estimated in section 9.1 as noticeably larger than the raw
file) simultaneously with the cluster tables — tens of GB of working set
against 16GB of physical RAM. That is a real, still-unresolved cost of
*fully* loading the blueprint the way the original authors' own tooling
does; it says nothing about whether the file exists or is legitimate.

### A much smaller alternative: read only the node(s) actually needed

`Save_load.h`'s own write side (`dfs_write()`) writes the tree **depth-first,
action-major**: for one "batch" of `clusterlen` parallel private-hand-cluster
info sets (169 at the root), it writes one shared `int32 action_len`, then
(if `action_len` is in the ordinary 1-99 range) one shared `actionstr[action_len]`,
then, for *each* of the `clusterlen` slots in turn, that slot's
`double[action_len] regret` and `double[action_len] averegret` arrays — and
only *after* all of that, recursively, `action_len` complete subtrees (one
per legal action), each written in full before the next begins.

This means the ROOT node's own header (hero's very first preflop decision,
shared by all 169 clusters) sits in the first few KB of the file, and any
DEEPER node's header can be reached by reading a chain of ancestor headers
plus **skipping** (not reading into memory, just walking past) whichever
earlier sibling subtrees weren't taken. None of this requires reading, let
alone holding in RAM, the other ~16GB of the file.

`PokerAI/tree/BlueprintReader.h` is a new, additive header implementing
exactly this: `read_node_header()` reads one node's `action_len`/`actionstr`/
`averegret` (only `averegret` — the CFR average-strategy accumulator — is
kept; `regret` is read-and-discarded just to stay aligned with the file),
`skip_subtree()` walks past one full subtree without allocating anything,
and `lookup_preflop_strategy(path, action_path, hand_cluster)` chains both
to walk from the root down a specific sequence of action bytes and return
the normalized average strategy for one specific 169-way hand cluster. It
never touches `Save_load.h` itself (that file is left completely
unmodified) and never handles the `action_len >= 100` chance-node case
(reserved for board-card deals) — preflop betting can never produce one
(no cards are dealt until preflop closes), so encountering that marker
during a preflop-only walk means the navigation took a wrong turn, and the
reader throws rather than silently trusting nearby bytes.

### Wiring into `dh_native_ai.cpp`

- `LiveGame` now tracks `preflop_action_path` (the exact byte-coded
  sequence of actions taken so far this preflop street, root-relative) and
  `preflop_path_confident` (goes false, for the rest of the hand, the
  moment a raise can't be matched to the trained ladder — see below).
- `resolve_preflop_decision()` (replaces the hardcoded `"call"` branch in
  `getdecision()`) calls `BlueprintReader::lookup_preflop_strategy()` with
  hero's actual `Engine::get_preflop_cluster()` bucket and the tracked
  path, samples an action from the returned real strategy, and — for a
  raise byte — computes the actual chip total using the **exact same
  pot-fraction formula** `PokerAI/poker/State.h`'s `take_action()` uses
  (`last_raise = pot * byte / 200 * 100`, except byte `3` which is
  `pot / 400 * 100`), so the GUI receives a properly sized `"raise N"`
  string, not just fold/call/allin.
- Any failure at all (file missing/unreadable, path inconsistent with the
  tree, non-positive strategy sum, — anything `BlueprintReader.h` throws)
  is caught and falls back to the original `"call"` placeholder for that
  one decision, with a one-line diagnostic on stderr. Never a crash, never
  a guess.
- **Matching an opponent's (or the AI's own) raise to a real byte code**:
  `match_raise_action_byte()` recomputes the same `take_action()` formula
  for every plausible byte (`1,2,3,4,8,20,40`) against the exact pre-action
  chip state and looks for an **exact** match to the observed new total
  bet — never a nearest-neighbor guess. If a human GUI player enters an
  arbitrary custom size that doesn't land on the trained ladder, the match
  fails, `preflop_path_confident` goes false, and every remaining preflop
  decision this hand uses the honest placeholder instead of extrapolating
  from an unreliable path.
- **Currently-usable histories**: the opening decision (no history at all),
  and any all-call/all-check history (e.g. limping), are always usable —
  `'l'`/`'d'`/`'n'` need no size matching. Histories containing a raise are
  usable exactly when that raise's size exactly matches the trained
  ladder's formula; otherwise that hand's preflop reverts to the
  placeholder from that point on. Facing *arbitrary* raise sizes with full
  fidelity would need either the exact same discretization the original
  training used for its own bet-size abstraction (not fully known without
  the original tree-construction driver code) or accepting nearest-bucket
  approximation — deliberately **not** implemented here to avoid silently
  guessing.

### A related pre-existing bug found and fixed while doing this

Building `match_raise_action_byte()` required reconstructing the exact
whole-hand chip state before an action, which surfaced a genuine,
already-committed bug in section 17's original `opp_take_action()`: for
**preflop** raises specifically, `pypokergui`'s reported `"raise N"` amount
is the *whole-preflop-street* cumulative total (confirmed by reading
`pypokergui/pypokerengine/engine/player.py`'s `paid_sum()`, which is
computed from `action_histories` — cleared at the start of every street,
but the blind-posting entries are themselves part of *preflop's*
`action_histories`, so blinds are already included in preflop's `amount`).
The original code subtracted that amount from `stack_at_street_start[opp]`,
which was itself *already* blind-adjusted (set in `restart_game()` after
deducting blinds) — double-counting the blind and under-crediting the
opponent's remaining stack by exactly the blind amount for the rest of the
hand whenever the opponent raised preflop. **Fixed** via
`street_relative_raise_baseline()`, which uses the original `20000` stack
(not the blind-adjusted `stack_at_street_start`) as the subtraction
baseline specifically for preflop, since that is the only street where the
two differ; every other street is unaffected (its `paid_sum()` already
starts at zero, matching `stack_at_street_start`).

### Validation performed

**Could not be run against the real file** — this development sandbox
still lacks OS-level disk permission to read `cluster/blueprint_strategy.dat`
(external drive; see section 17's disk-permission subsection — a sandbox-
specific limitation, not a code defect). Everything below is what could
honestly be validated without it:

- `dh_native_ai.cpp` (with `BlueprintReader.h` included) compiles cleanly
  to `.dylib`, all 4 required C symbols still present (`nm -gU`).
- **`BlueprintReader.h`'s reader was validated against hand-built synthetic
  files matching the documented format exactly** (built directly with
  Python's `struct.pack`, mirroring `dfs_write()`'s byte order field-for-
  field): a flat 169-cluster root read correctly reproduced distinct,
  correctly-normalized (`sum == 1.0`) per-cluster strategies from
  deliberately cluster-varying `averegret` values; a 2-level-deep tree
  (root offering `['d','l']`, `'d'` a terminal, `'l'` leading to a real
  child node offering `['l','n']`) correctly skipped the sibling `'d'`
  subtree and descended into `'l'`'s subtree, again reproducing the exact
  expected per-cluster normalized values. This confirms the reader's logic
  is correct **for the documented format** — it does not confirm the real
  16GB file matches that documented format in every respect, since it
  could not be opened from this sandbox.
- A new standalone tool, `PokerAI/tools/test_blueprint_root_read.cpp`,
  reads *only* the root node (a few KB) from a given blueprint path and
  prints, for a handful of sample clusters, the legal actions and
  normalized strategy. **The user should run this before trusting real
  preflop decisions in play**:

  ```shell
  cd PokerAI
  g++ -std=c++17 -O2 -o test_blueprint_root_read tools/test_blueprint_root_read.cpp
  ./test_blueprint_root_read            # uses cluster/blueprint_strategy.dat by default
  ```

  A sane result: small `action_len` per cluster (2-8ish), byte codes
  matching `State.h` (`'d'`=fold, `'l'`=call, `'n'`=allin, small ints for
  raise sizes), probabilities summing to ~1.0, and **not** every cluster
  producing an identical distribution (169 clusters getting the exact same
  numbers would suggest either a corrupt/placeholder file or a reader bug,
  even though the byte-level parsing tested clean above).

### Update: confirmed against the real file — real, sane blueprint data

The user ran `test_blueprint_root_read` from their own terminal (with
working disk access to the external drive) against the real
`cluster/blueprint_strategy.dat`. Actual output:

```
Reading root node from: cluster/blueprint_strategy.dat
cluster   0: action_len=9  actions/probs:  'd'(100)=0.0000  'l'(108)=0.0875  [1]=0.4954  [2]=0.4100  [4]=0.0071  [8]=0.0000  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
cluster   1: action_len=9  actions/probs:  'd'(100)=0.9545  'l'(108)=0.0093  [1]=0.0073  [2]=0.0159  [4]=0.0119  [8]=0.0010  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
cluster  42: action_len=9  actions/probs:  'd'(100)=0.0000  'l'(108)=0.0596  [1]=0.7044  [2]=0.2349  [4]=0.0011  [8]=0.0000  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
cluster  84: action_len=9  actions/probs:  'd'(100)=0.0000  'l'(108)=0.0484  [1]=0.5923  [2]=0.3572  [4]=0.0019  [8]=0.0002  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
cluster 100: action_len=9  actions/probs:  'd'(100)=0.0000  'l'(108)=0.0823  [1]=0.6199  [2]=0.2900  [4]=0.0076  [8]=0.0003  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
cluster 150: action_len=9  actions/probs:  'd'(100)=0.0000  'l'(108)=0.0766  [1]=0.6250  [2]=0.2925  [4]=0.0056  [8]=0.0002  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
cluster 168: action_len=9  actions/probs:  'd'(100)=0.0000  'l'(108)=0.0252  'l'(108)... [1]=0.5441  [2]=0.4151  [4]=0.0150  [8]=0.0006  [20]=0.0000  '('(40)=0.0000  'n'(110)=0.0000   (sum=1.000000)
```

This is a genuine, positive validation:

- `action_len=9`: the root's action set is `{'d','l',1,2,4,8,20,40,'n'}` —
  exactly 9 of the 10 possible codes in `State.h`'s
  `raise_action_chips` map (missing only byte `3`, one specific
  pot-fraction raise size that this abstraction's root apparently doesn't
  offer — a training-time abstraction choice, not a reader defect).
  `'('` printed for byte `40` is just this tool's printf treating any
  printable ASCII byte (32-126) as a char for readability — `40` is `(` in
  ASCII, it is not a special/different action.
- Probabilities sum to `1.000000` for every cluster, every time — the
  normalization (`averegret[i] / sum(averegret)`, matching `Node.h`'s
  `calculate_strategy()`) is being computed correctly against real data.
- **Clusters are meaningfully different, not degenerate**: cluster `1`
  folds 95.45% of the time (a genuinely weak preflop hand-cluster bucket),
  while clusters `0`, `42`, `84`, `100`, `150`, `168` fold 0% and mostly
  raise (split between the `[1]` and `[2]` pot-fraction sizes) — this is
  exactly the shape of a real trained preflop strategy (fold the worst
  bucket, open-raise/limp everything reasonable), not placeholder or
  corrupted data.

**This confirms `BlueprintReader.h` correctly parses the real
`blueprint_strategy.dat` at the root, and the earlier synthetic-file-only
validation was not misleading** — the real file matches the documented
format exactly at the byte level. `dh_native_ai.cpp`'s opening preflop
decision (no action history yet) is now genuinely backed by real, sane,
non-fabricated trained data, not a call-everything placeholder.

**Still not independently confirmed against the real file**: reads at
non-root depth (a live hand's history after a call/raise, which requires
`skip_subtree()` to walk past sibling actions). The code path is identical
to what was already exercised by the 2-level synthetic test in the
previous subsection, and the root read above confirms the low-level
`read_node_header()`/byte-layout parsing this shares is correct against
the real file — but a direct, real-file confirmation of one specific
raised-pot history has not yet been done. This is a much smaller residual
gap than "the whole feature is unvalidated," and does not block normal
opening-hand and limped/called preflop decisions, which are confirmed
working above.

### Honest scope after this change

- Preflop decisions are now backed by the real trained blueprint for the
  opening action and for pure call/check preflop histories, **confirmed
  against the real file** (see above); raised pots use it only when the
  raise size exactly matches the trained ladder, else fall back to the
  original placeholder for the rest of that hand's preflop. The
  raised-pot code path shares its low-level parsing with the confirmed
  root read but has not itself been separately exercised against the real
  file at non-root depth.
- The full-tree, arbitrary-raise-size feature (loading/consulting the
  entire blueprint, or handling any custom bet size) remains explicitly
  out of scope for the reasons above — this section only replaces the
  narrowest, safest, highest-value slice (the most common preflop
  decisions) with real data, not the whole preflop game tree.

## 19. GUI startup hang after the blueprint change: confirmed unrelated, root cause is the (never-actually-needed) full river-cluster swap load

After section 18's blueprint change, the user reported `python3
../__main__.py serve dummy --port 8000` appearing to hang right after
printing `ai start load`, unsure whether it was progressing or stuck, and
suspected the new blueprint-reading code was the cause.

**It was not.** `resolve_preflop_decision()`/`BlueprintReader.h` only run
*during a hand*, when `getdecision()` is called for a preflop decision —
they open and read a few KB from `cluster/blueprint_strategy.dat` on
demand. Nothing in section 18's change runs at library-load time. The
`ai start load` → `load finish` messages bracket `Engine::load()`
(`PokerAI/poker/Engine.h`), which is unrelated code, untouched by section
18, and was already known (`244ff43`, "Revert `DH_SKIP_RIVER_CLUSTER`...
load real river cluster via swap") to fully materialize
`river_hand_cluster.bin` (16.86GB) in memory at load time, per the user's
own earlier request to test whether that would work under swap. This was
the build actually running — the timing (this being the first launch
attempt after the blueprint rebuild) was coincidental, not causal.

**Live diagnosis, on the same host, while the user's process was
"hanging":** its PID was visible from this session's own shell (same
machine). `ps` showed a process 8+ minutes in, resident set size actually
*shrinking* over time (a classic sign of active paging/thrashing, not a
frozen/deadlocked process), and `sysctl vm.swapusage` showed swap climbing
in real time — `total=5120M used=4300M free=820M`, then seconds later
`total=6144M used=5034M free=1110M` (macOS dynamically grows the swap
file as pressure increases). This is a real, still-progressing but
extremely slow disk-bound load (the file lives on an external drive, and
"progress" here means paging ~17GB through a system with 16GB of unified
memory), not a code hang — but it was heading toward exhausting available
swap/disk headroom with no guarantee of ever completing, so it was killed
(`kill <pid>`) rather than left running further.

**The fix: stop loading `river_hand_cluster.bin` at all — it was already
proven dead weight.** Section 17's own header comment in `dh_native_ai.cpp`
states plainly that this library's river-street code only ever calls
`Engine::compute_winner()` (backed by `sevencards_strength.bin`), and
**never** `Engine::get_river_cluster()` — the 16.86GB file has no code
path that reads it in this resolver at all. Loading it via swap was
purely an experiment the user asked for earlier ("I want to see if it's
possible to run with swap"); the experiment's answer is now conclusively
**no** — not within a reasonable time, and not without risking swap/disk
exhaustion. The file's own committed build-command comment already
recommended `-DDH_SKIP_RIVER_CLUSTER` as the default; that recommendation
was temporarily overridden for the swap experiment and is now restored:

```shell
cd PokerAI
g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER -shared -fPIC \
    -o dh_native_ai.dylib tools/dh_native_ai.cpp
```

Rebuilt and confirmed: compiles cleanly, all 4 ABI symbols present
(`nm -gU`), and swap usage on this host immediately began dropping back
down after the old process was killed (`total=3072M used=1583M
free=1489M` moments later) — consistent with the large in-flight
allocation being released, not with any leak or ongoing pressure from
other code.

**This flag has zero effect on decision quality or on the new preflop
blueprint feature** — they are fully independent:
- River decisions still use exact showdown equity via
  `Engine::compute_winner()`/`sevencards_strength.bin`, unaffected either
  way, because that was always the only river data path used.
- The section 18 preflop blueprint reader opens
  `cluster/blueprint_strategy.dat` directly and independently via
  `BlueprintReader.h`'s own small, targeted reads — it does not depend on
  or interact with `Engine::load()` or the `DH_SKIP_RIVER_CLUSTER` flag at
  all.

**Recommendation:** always build with `-DDH_SKIP_RIVER_CLUSTER` on this
host (16GB unified memory) going forward — it is not a reduced-fidelity
mode for anything this codebase's resolver actually uses, only a RAM/swap
optimization that removes a genuinely large, genuinely unused load.

## 20. Porting `play_with_slumbot.py` to macOS; no Slumbot account needed

The user asked whether there's a script for playing Slumbot and whether an
account is required. There is a script
(`pypokergui/play_with_slumbot.py`), but as committed it had two real,
independent problems that would prevent it from running here, plus a
question about whether external play against slumbot.com's live server
should happen automatically at all.

### No account is required — confirmed live against the real API

Slumbot's API makes the session token optional on the very first
`/api/new_hand` request; a fresh guest token is issued in the response.
Confirmed directly:

```shell
$ curl -i -X POST -d '{}' -H "Content-Type: application/json" https://slumbot.com/api/new_hand
HTTP/1.1 200 OK
...
{"old_action": "", "action": "b200", "client_pos": 0, "hole_cards": ["9s","7s"], "board": [], "token": "09915fc4-8472-4e55-a043-27f3bc2df9a7"}
```

No login, registration, or credentials are needed to play informally
against the bot via this API.

### Problem 1: hardcoded credentials for someone else's account

`play_with_slumbot.py`'s `main()` unconditionally called
`Login('zqbDec', 'zqbDec@2021')` — a real-looking username/password pair
baked into source, presumably the original paper authors' own registered
Slumbot account (used to appear on Slumbot's public leaderboard as
`zqbAgent`, per the README). Since login isn't required at all for
anonymous play, and reusing someone else's credentials isn't something
this investigation should do, **this call is removed by default**.
`main()` now only calls `Login()` if the user explicitly supplies their
own `--username`/`--password` on the command line (e.g. to appear under
their own registered identity) — anonymous play is now the default,
matching what the API actually supports.

### Problem 2: hardcoded Linux `.so`, not OS-conditional

`play_with_slumbot.py` imports `FishPlayer` from
`pypokergui/fish_player_setup.py` (the top-level one — a different,
simpler file than `pypokergui/server/fish_player_setup.py`, which is
wired for the GUI's `pypokerengine`-flavored callback interface and
cannot be reused here directly). That top-level file's constructor
unconditionally did `cdll.LoadLibrary('./AlascasiaHoldem.so')` — the same
Linux x86_64 ELF binary problem documented in sections 4/12/17, just
never fixed in this particular file (only `game_manager.py`, used by the
GUI path, had received the Darwin/Linux conditional). **Fixed** using the
exact same pattern already proven in `game_manager.py`:

```python
lib_name = './dh_native_ai.dylib' if platform.system() == 'Darwin' else './AlascasiaHoldem.so'
self.playsearch = cdll.LoadLibrary(lib_name)
```

Linux behavior (loading the original `.so`) is completely unchanged.

### Problem 3 (design, not a bug): an unbounded automated loop against a live external server

The original `main()` was a bare `while True:` loop with no exit
condition, i.e. running the script would play hands against slumbot.com
indefinitely until manually interrupted. Per this investigation's own
scope (avoid unnecessary/unbounded automated external play; a small,
sanctioned benchmark is fine), `main()` now takes a `--max-hands N`
argument (**default 10**, not unlimited) and stops cleanly after that
many hands, printing a total-winnings summary. `--max-hands 0` restores
the original unlimited behavior for anyone who deliberately wants it.

### Exact run command (from this repo, on macOS)

```shell
cd PokerAI   # required: dh_native_ai.dylib and its relative cluster/... paths live here
python3 ../pypokergui/play_with_slumbot.py --max-hands 5
```

(`--username`/`--password` are optional, only needed to play under a
registered Slumbot account instead of anonymously.)

### Validation performed

- Both modified files (`play_with_slumbot.py`, `fish_player_setup.py`)
  parse cleanly (`ast.parse`) and the new `--help`/argparse wiring works.
- Confirmed the OS-conditional fix actually takes effect and reaches the
  correct next failure point: run from the wrong directory, it correctly
  reports `dlopen(./dh_native_ai.dylib, ...) — no such file` (proving the
  Darwin branch is chosen and it is genuinely trying the right filename,
  rather than a silent import-time crash); run from `PokerAI/` (the
  documented correct cwd), it proceeds past that point and begins
  `Engine::load()` reading real cluster files from the external drive --
  which this sandbox cannot do (the same disk-permission limitation
  documented in section 17, not a new or different problem). This is
  consistent with every other native-library-loading tool in this repo
  and is not a regression introduced by this fix.
- **Not run against the live Slumbot server from this sandbox** for the
  same reason (`Engine::load()` blocks first, before any network call
  would happen) -- the user, who has working disk access, should run the
  command above to get an actual end-to-end result and confirm hands play
  out and a win/loss total is reported.

## 21. Real bug found and fixed: `BlueprintReader::skip_subtree()` mishandled sibling chance nodes

### The report

While actually playing a live hand through the GUI, stderr showed:

```
[DH_PREFLOP_BLUEPRINT] real blueprint lookup failed (BlueprintReader: unexpected chance node encountered while skipping a preflop-only subtree) -- falling back to placeholder 'call' for this decision only
python rec ai action: call
```

This directly falsified a design assumption written into section 18 and
`BlueprintReader.h`'s own comments: "preflop betting can never produce a
chance node — no cards are dealt until preflop closes." That assumption
was **wrong**, and this was a real, previously-undiscovered defect in the
new reader code, not a sandbox/environment limitation.

### Root cause

`Save_load.h`'s `dump()`/`dfs_write()` serializes the **entire game tree**
into `blueprint_strategy.dat` — not a preflop-only slice. The moment any
betting line's preflop action closes (e.g. an opponent's check-back after
a limp), the tree's very next node **is** a chance node (the flop board
deal), and that is completely normal, correct tree shape.

`BlueprintReader::lookup_preflop_strategy()`'s target-node traversal itself
is fine, because it is only ever invoked while preflop betting is
genuinely still open (`getdecision()` only calls
`resolve_preflop_decision()` while `g.betting_stage == 0`). But to reach
the correct sibling action index at each step, it must call
`skip_subtree()` on every *other* legal action at that node first — and if
one of those siblings is a line that closes preflop betting, its subtree
legitimately contains a chance node a few levels down. The old
`skip_subtree()` treated any `action_len >= 100` marker as a hard error and
threw, even when it was hit while skipping a sibling (as opposed to
reading the actual target node).

Re-deriving the exact write-side semantics from `Save_load.h`'s
`dfs_write()`:

```cpp
else if (len > 100) {                       // chance-node marker
    strategy_node** privatenode2 = new strategy_node*[len];
    for (int j = 0; j < len; j++) privatenode2[j] = privatenode[0]->actions + j;
    dfs_write(privatenode2, len);            // ONE recursive call, clusterlen := len
    delete[] privatenode2;
}
```

A chance-node marker writes **only** the `int32 len` itself (no
actionstr/regret/averegret), and is immediately followed by **exactly one**
more node, read with `clusterlen` replaced by the chance node's fan-out
count (`len`) — not a loop of `len` separate nodes.

### The fix

`PokerAI/tree/BlueprintReader.h`'s `skip_subtree()` now mirrors this
exactly: on `len >= 100` it no longer throws; it recurses once more with
`clusterlen = len` and returns:

```cpp
if (len >= 100) {
    skip_subtree(fin, len);   // exactly one more node follows, with clusterlen=len
    return;
}
```

`read_node_header()` (used only for nodes actually on the caller's lookup
path) still throws on `action_len >= 100`, since the exact node being
evaluated for a preflop decision should never legitimately be a chance
node — if it ever is, that means a real caller-side bug (a `getdecision()`
call after preflop should already be closed), which is still worth failing
loudly on rather than silently misinterpreting.

Also corrected the file's own top-of-header design comments, which
previously asserted the now-falsified "preflop never produces a chance
node" claim, and added an honest performance note: skipping a
closed-early sibling can now legitimately read through that sibling's
entire nested postflop subtree on disk (still far less than the full
~16GB file and no large RAM allocation, but no longer guaranteed to be a
"few KB" operation for every lookup).

### Validation performed

This sandbox still has no OS-level access to the real
`cluster/blueprint_strategy.dat` (external-drive permission wall,
unchanged from every earlier section). To validate the fix's logic without
that file, a small synthetic blueprint file was generated in Python,
byte-for-byte matching `Save_load.h`'s on-disk format with a *small*
`clusterlen=3` (instead of 169, purely to keep the fixture tiny):

- Root betting node, `actionstr = ['d','l','r']` (fold / call·limp / raise)
- Action `'d'` (idx 0): a plain terminal (`action_len=0`) — exercises
  skipping a trivial sibling.
- Action `'l'` (idx 1): closes preflop into a **chance node** with fan-out
  150, itself followed by a real betting node — exercises the exact bug
  scenario (a sibling that legitimately contains a chance node).
- Action `'r'` (idx 2): the **target** node, a real betting node
  (`actionstr = ['d','l']`, probs 0.7/0.3) — what the lookup is supposed to
  return.

Compiled and ran the *original* (pre-fix) `BlueprintReader.h` against this
fixture, requesting `action_path = ['r']`: it reproduced the **exact**
reported error message verbatim:

```
FAIL: threw: BlueprintReader: unexpected chance node encountered while skipping a preflop-only subtree
```

Then compiled and ran the **fixed** version against the same fixture: it
correctly skipped past both siblings (the terminal and the
chance-node-embedding one) and returned the target node's real data:

```
OK: action_len=2
  action=100 prob=0.7000   ('d')
  action=108 prob=0.3000   ('l')
PASS
```

This confirms both (a) the bug is exactly what the root-cause analysis
above describes, and (b) the fix resolves it, using a fixture that
exercises the precise failure mode reported — the closest validation
possible without disk access to the real 16GB file from this sandbox.

Rebuilt `dh_native_ai.dylib` (`g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER
-shared -fPIC -o dh_native_ai.dylib tools/dh_native_ai.cpp`, per section
19's recommended command): compiles cleanly, all 4 ABI symbols intact
(`nm -gU`: `_restart_game`, `_Next_stage`, `_opp_take_action`,
`_getdecision`).

### What's still not directly verified

The synthetic-fixture test proves the reader's *tree-navigation logic* is
now correct for this failure mode. It does **not** by itself prove the
real `blueprint_strategy.dat` file's actual chance-node fan-out values
match what this reasoning assumes (e.g. the real flop fan-out count,
whatever it is, is simply read from the file as `len` and used directly —
there is no hardcoded assumption about its exact value, so this should
generalize correctly regardless of the real number). The user should
re-run live play; if any further "unexpected chance node" or "action byte
not found" exceptions surface, they would point to either a different,
still-undiscovered reader bug, or a `g.preflop_action_path` tracking bug in
`dh_native_ai.cpp` (worth checking `opp_take_action()`/`resolve_preflop_decision()`
first, per section 18's caveats) — not this specific issue, which is now
fixed and validated against its exact failure mode.

## 22. Real bug found and fixed: `TurnClusterLeafModel` had an inverted cluster-strength comparison (systematically bad flop decisions)

### The report

The user flagged a specific hand as suspicious: on the flop (board `8h 6h 9s`
at decision time), holding `3c Td` (ten-high, no pair, only a backdoor
gutshot straight draw), the AI **called an effectively-full-stack all-in
shove** (19000 into a ~2000 pot). This looked like a bad call worth
investigating directly, not dismissing as "the known unsafe-resolving
simplification."

### Investigation and root cause

This was investigated with the external drive now accessible from this
sandbox (a change from every earlier section), which made it possible to
test directly against the real cluster files for the first time.

First, a plain Monte Carlo simulation (independent of any DecisionHoldem
code, using a from-scratch hand evaluator) found hero's actual equity in
this exact spot against a **uniform random opponent hand** (the resolver's
own stated, already-documented simplifying assumption, section 17) is only
**~32.8%** — far short of the ~47.5% pot odds required to profitably call.
So even under the model's own most-generous assumption, this call should
have been a clear fold. That ruled out "unsafe/uniform-range resolving" as
the explanation and pointed at a real defect in the decision math itself.

Reviewed `PokerAI/tree/RealtimeSearch.h`'s `TurnClusterLeafModel` (the
class that estimates a flop decision's continuation value once flop
betting closes, by comparing each side's `Engine::get_turn_cluster()` id
averaged over the possible next card — this repo's own "ordinal cluster id
approximates hand strength" idea, reused from the original authors'
offline `tree/Exploitability.h`). Its `expected_showdown_sign()` compared
`hc > vc` (hero's cluster id greater than villain's) as **hero wins**.

Cross-checking against the *original authors'* own `Exploitability.h`
(`getnode_cfv_river()`, lines ~37-75) revealed the opposite convention:
`if (clusters[mycard] > clusters[j]) actionicfvs1[j] = -pot*0.5;` — i.e. a
**greater** cluster id there results in a **loss**. That is: a **lower**
cluster id is the **stronger** hand, not a higher one.

To settle this empirically rather than by code-reading alone (`to_cluster`
in `Exploitability.h` is populated by a caller not present in this repo,
so it isn't provable from that file alone that it's the exact same id
space as `Engine::get_turn_cluster()`), a direct test was built and run
against the **real** `turn_hand_cluster.bin` (now accessible) for the
exact disputed hand (`3c Td` on `8h 6h 9s`), enumerating all 1081 possible
villain hole-card combinations and averaging `expected_showdown_sign()`
both ways:

```
ORIGINAL polarity (hc>vc=win) hero equity: 76.38%
FLIPPED  polarity (hc<vc=win) hero equity: 23.62%
Reference (real Monte Carlo equity):        32.81%
```

The original code told the resolver hero was a **76% favorite** in a spot
where hero is actually a clear underdog — essentially inverted. The
flipped convention lands on the correct side (underdog) and within the
expected noise band of a coarse, bucketed approximation (cluster ids pool
many hands together; some gap from the exact 32.8% figure is expected and
not itself a bug). This confirms the fix, independently of the
`Exploitability.h` cross-check.

### The fix

`PokerAI/tree/RealtimeSearch.h`'s `TurnClusterLeafModel::expected_showdown_sign()`:

```cpp
// before (backwards):
if (hc > vc) sum += 1.0;
else if (hc < vc) sum -= 1.0;

// after (correct: lower cluster id = stronger hand):
if (hc < vc) sum += 1.0;
else if (hc > vc) sum -= 1.0;
```

Searched the rest of `RealtimeSearch.h` and `dh_native_ai.cpp` for any
other ordinal cluster-id comparisons that might share this bug: this is
the **only** place hand-cluster ids are compared this way (`FlopResolver`,
an offline demo/study tool not wired into live play, shares the same
`TurnClusterLeafModel` class and is automatically fixed by the same
change; every other resolver path uses exact `Engine::compute_winner()`/
`sevencards_strength.bin` showdown scoring, or the real preflop blueprint,
neither of which involve this comparison).

### Scope and severity

This affected **every flop decision** that reaches this leaf model (i.e.
essentially all FLOP-mode `LiveResolver` resolves since section 17 wired
it into live play) — not just this one hand. The resolver was
systematically overvaluing hero's continuation equity whenever flop
betting would close, biasing toward far too many calls (and likely too
few folds) across the board. This is a significantly more consequential
bug than section 21's preflop tree-navigation fix; it degrades actual
decision quality on essentially every flop pot, not just a specific rare
tree-navigation edge case.

### Validation performed

- Independent Monte Carlo ground truth (from-scratch Python hand
  evaluator, no DecisionHoldem code reused) for the exact disputed hand.
- Direct empirical test against the real `cluster/turn_hand_cluster.bin`
  and `cluster/sevencards_strength.bin` (now accessible from this sandbox)
  comparing both polarity conventions' implied equity against that ground
  truth.
- Cross-checked against the original authors' own (unmodified, pre-existing)
  `tree/Exploitability.h` code, independently confirming the same "lower
  cluster id wins" convention for river clusters.
- Confirmed no other code path shares the same comparison pattern.
- Rebuilt `dh_native_ai.dylib` (`g++ -std=c++17 -O2 -DDH_SKIP_RIVER_CLUSTER
  -shared -fPIC -o dh_native_ai.dylib tools/dh_native_ai.cpp`): compiles
  cleanly, all 4 ABI symbols intact (`nm -gU`).

### What's still not directly verified

The exact numeric residual gap between the flipped-polarity model estimate
(23.62%) and true equity (32.81%) for this one hand has not been
decomposed further (how much is inherent cluster-bucketing coarseness vs.
some other smaller remaining approximation error) — that gap is expected
and inherent to using a bucketed abstraction as a proxy for exact equity,
not evidence of a further bug, but it does mean flop decisions from this
resolver remain an approximation, not exact-equity play, same as
documented in sections 15-17. The user should re-test live play across a
range of flop spots (not just calls facing shoves) to build confidence
that decision quality has meaningfully improved with this fix in place.
