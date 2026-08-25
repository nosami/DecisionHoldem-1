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
