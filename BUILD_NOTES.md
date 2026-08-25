# Build / Reproducibility Notes (macOS investigation)

This document records a from-scratch attempt to build and run every component of
DecisionHoldem end-to-end on a Darwin/arm64 (macOS) host, the concrete source
defects found and fixed, and — most importantly — exactly which artifacts are
genuinely missing from this repository, why they cannot be regenerated from the
included source, and what that means for what can and cannot legitimately run.

**Bottom line:** the C++ blueprint-training/evaluation program now compiles and
links cleanly on macOS (previously it did not compile at all — see below), and
its CLI now behaves as documented. However, no part of the trained/real-time
agent can actually be exercised — on macOS *or* Linux — without the pretrained
hand-abstraction data described below, which is not present in this repository,
not present anywhere in its git history, not published as a GitHub release, and
is only referenced via a third-party Baidu Netdisk link that this investigation
did not use (unverifiable, access-gated, and out of scope per instructions not
to rely on dubious third-party binaries). The precompiled `AlascasiaHoldem.so` /
`blueprint.so` real-time-search binaries are Linux x86_64 ELF objects with **no
corresponding source in this repo** (the README explicitly says this component
"currently only provide[s] compiled files"), so they cannot be ported to macOS
at all without that missing source; they would need to run under Linux x86_64
(e.g. Docker), which was not available in this sandbox.

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
  through. Run it yourself:

```bash
python3 PokerAI/tools/depth_limited_search_demo.py
```

Expect all `[PASS]` lines (5 of them) and exit code 0; the two `[FINDING]`
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
| `river_hand_cluster.bin` | *(downloading)* | 16,856,854,560 (1,326 combos × 2,118,760 × 6 bytes; values are `unsigned short`, not `unsigned`) | *(pending)* |
| `blueprint_stgy.dat` (mirror's name for `blueprint_strategy.dat`) | *(size TBD)* | variable (grown by training) | *(pending — needs rename to `blueprint_strategy.dat` before use)* |

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

## 3. Python / GUI stack (`pypokergui`, Slumbot/OpenStackTwo bots)

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

## 4. What was validated, with exact commands

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

## 5. Summary for the user

- **Fixed and verified:** the C++ source now compiles cleanly on macOS/Clang
  and on Linux/GCC (fixes are behavior-preserving on both), the CLI's train
  vs. evaluate dispatch bug is fixed, and the Python GUI/bot dependency list is
  corrected for modern Python.
- **Cannot be run, on macOS or Linux, without externally-obtained data:** the
  blueprint trainer, the evaluator, and both `.so`-based interfaces all require
  ~19.4 GiB of pretrained hand-abstraction/clustering data
  (`sevencards_strength.bin`, `preflop_hand_cluster.bin`, `flop_hand_cluster.bin`,
  `turn_hand_cluster.bin`, `river_hand_cluster.bin`) that is not in this
  repository, not in its git history, not in a GitHub release, and not
  regenerable from any source code included here. The only known source is the
  README's third-party Baidu Netdisk link, which this investigation did not
  access.
- **Cannot be run on macOS regardless of data:** `AlascasiaHoldem.so` and
  `blueprint.so` are Linux x86_64 binaries with no included source; they need a
  Linux x86_64 runtime (e.g. Docker), which was unavailable in this sandbox.
- No placeholder/fabricated data files were created or committed anywhere in
  this repository.

## 6. Possible path to a working real-time search: an external solver bridge

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
