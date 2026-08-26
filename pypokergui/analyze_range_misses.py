#!/usr/bin/env python3
"""Summarizes [DH_RANGE_MODEL] "actual villain hand=..." lines from a
play_with_slumbot.py log file (see dh_native_ai.cpp's report_actual_hand()/
dh_log_actual_hand(), and fish_player_setup.py's report_actual_hand()).

Each such line is printed once per hand, at hand-end, comparing villain's
REAL revealed hole cards (Slumbot reveals these for every hand, not just
showdowns) against this run's own tracked villain_range belief at that
point. A "RANGE MISS" means the actual holding was weighted BELOW a
uniform random guess -- i.e. narrowing had convinced itself the real hand
was less likely than having no information at all, the concrete signature
of "the opponent wasn't holding a hand we thought was in his range".

Usage:
    python3 analyze_range_misses.py /tmp/run.log
    python3 analyze_range_misses.py /tmp/run.log --only-misses
"""
import argparse
import re
import sys

LINE_RE = re.compile(
    r"\[DH_RANGE_MODEL\] actual villain hand=(?P<hand>\S{4}) "
    r"weight=(?P<weight>[\d.]+)% rank=(?P<rank>\d+)/(?P<n>\d+) "
    r"\(uniform=(?P<uniform>[\d.]+)%\) -- (?P<verdict>RANGE MISS[^.]*|within expected range)\. "
    r"Top expected:(?P<top>.*)"
)

# The "not found" / "empty range" variants don't have weight/rank at all --
# tracked separately since they indicate a deeper (likely upstream) issue,
# not just an ordinary low-probability holding.
ANOMALY_RE = re.compile(
    r"\[DH_RANGE_MODEL\] actual villain hand=(?P<hand>\S{4}) (?P<msg>.*)"
)


def parse(path):
    rows = []
    anomalies = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if m:
                rows.append(m.groupdict())
                continue
            if "[DH_RANGE_MODEL] actual villain hand=" in line:
                m2 = ANOMALY_RE.search(line)
                if m2:
                    anomalies.append(m2.groupdict())
    return rows, anomalies


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logfile", help="Path to a play_with_slumbot.py log "
                                     "(stdout+stderr combined, e.g. via "
                                     "`... 2>&1 | tee /tmp/run.log`)")
    ap.add_argument("--only-misses", action="store_true",
                     help="Only list the RANGE MISS hands, skip the summary "
                          "of hands that were within expectations")
    args = ap.parse_args()

    rows, anomalies = parse(args.logfile)
    if not rows and not anomalies:
        print("No [DH_RANGE_MODEL] actual-hand lines found in %s -- either "
              "no hands were played, or this log predates the "
              "report_actual_hand diagnostic." % args.logfile)
        sys.exit(0)

    misses = [r for r in rows if r["verdict"].startswith("RANGE MISS")]
    hits = [r for r in rows if not r["verdict"].startswith("RANGE MISS")]

    print("=== Range-miss summary for %s ===" % args.logfile)
    print("Hands with a tracked comparison: %d" % len(rows))
    print("  RANGE MISS (below uniform):    %d (%.1f%%)" %
          (len(misses), 100.0 * len(misses) / len(rows) if rows else 0.0))
    print("  Within expected range:         %d (%.1f%%)" %
          (len(hits), 100.0 * len(hits) / len(rows) if rows else 0.0))
    if anomalies:
        print("  Anomalies (empty/not-found range, see raw lines): %d" % len(anomalies))
    print()

    def fmt_row(r):
        return ("  %-4s  weight=%6s%%  rank=%5s/%-5s  uniform=%6s%%  top:%s" %
                (r["hand"], r["weight"], r["rank"], r["n"], r["uniform"], r["top"]))

    if misses:
        print("--- RANGE MISS hands (opponent held something we'd ranked "
              "below a uniform guess) ---")
        for r in sorted(misses, key=lambda r: float(r["weight"])):
            print(fmt_row(r))
        print()

    if not args.only_misses and hits:
        print("--- Within-expected-range hands ---")
        for r in hits:
            print(fmt_row(r))
        print()

    if anomalies:
        print("--- Anomalies (empty tracked range / combo not found) ---")
        for a in anomalies:
            print("  %-4s  %s" % (a["hand"], a["msg"]))


if __name__ == "__main__":
    main()
