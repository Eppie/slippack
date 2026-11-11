#!/usr/bin/env python3
"""AFL++-style live status screen for a slippack libFuzzer run.

Reads logs/<target>.log (written by run.sh) plus runs/<target>/artifacts, and
renders a refreshing panel: throughput, coverage, corpus, and crash counts.
libFuzzer does the fuzzing; this just visualizes its stat stream.

  ./dash.py parse             # follow, refresh every 0.5s
  ./dash.py roundtrip -n 0.2  # refresh every 0.2s
  ./dash.py parse --once      # print one snapshot and exit (for logging)

'exec/s live' is computed by the dashboard between refreshes; 'exec/s (lF)' is
libFuzzer's own figure. 'data age' is seconds since libFuzzer last wrote a stat
line — it turns yellow when the underlying numbers are going stale.
"""
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ESC = "\x1b["
BOLD, DIM, GRN, YEL, RED, CYN, RST = (ESC + s for s in
    ("1m", "2m", "32m", "33m", "31m", "36m", "0m"))

# libFuzzer stat lines, both fork-mode and single-process. We pull key:value
# tokens loosely so both formats parse.
NUM = r"(\d+)"
RE_HASH = re.compile(r"#" + NUM)
RE_KV = {
    "cov":      re.compile(r"cov:\s*" + NUM),
    "ft":       re.compile(r"ft:\s*" + NUM),
    "exec_s":   re.compile(r"exec/s:\s*" + NUM),
    "rss":      re.compile(r"rss:\s*" + NUM),
    "time":     re.compile(r"time:\s*" + NUM + r"s"),
}
RE_CORP = re.compile(r"corp:\s*(\d+)(?:/(\d+)([KMG]?)b)?")
RE_OTC = re.compile(r"oom/timeout/crash:\s*(\d+)/(\d+)/(\d+)")
RE_EVENT = re.compile(r"#\d+\s+(NEW|REDUCE|pulse|INITED|DONE)")


def human(n, unit=""):
    n = float(n)
    for u in ("", "K", "M", "G", "T"):
        if n < 1000 or u == "T":
            return f"{n:.0f}{u}{unit}" if u == "" else f"{n:.1f}{u}{unit}"
        n /= 1000


def hms(sec):
    sec = int(sec)
    h, m, s = sec // 3600, (sec % 3600) // 60, sec % 60
    return f"{h:d}:{m:02d}:{s:02d}"


def parse_log(path):
    st = {"execs": 0, "cov": 0, "ft": 0, "exec_s": 0, "rss": 0,
          "corp_n": 0, "corp_b": 0, "oom": 0, "timeout": 0, "crash": 0,
          "last_event": "", "fork_time": 0, "lines": 0, "last_new": None,
          "cov_peak": 0}
    if not os.path.exists(path):
        return st
    with open(path, "r", errors="replace") as f:
        for line in f:
            if "cov:" not in line and "#" not in line:
                continue
            st["lines"] += 1
            m = RE_HASH.search(line)
            if m:
                st["execs"] = max(st["execs"], int(m.group(1)))
            for k, rx in RE_KV.items():
                m = rx.search(line)
                if m:
                    st[k] = int(m.group(1))
            m = RE_CORP.search(line)
            if m:
                st["corp_n"] = int(m.group(1))
                if m.group(2):
                    mult = {"": 1, "K": 1e3, "M": 1e6, "G": 1e9}[m.group(3)]
                    st["corp_b"] = int(m.group(2)) * mult
            m = RE_OTC.search(line)
            if m:
                st["oom"], st["timeout"], st["crash"] = map(int, m.groups())
            m = RE_KV["time"].search(line)
            if m:
                st["fork_time"] = int(m.group(1))
            m = RE_EVENT.search(line)
            if m:
                st["last_event"] = m.group(1)
            if st["cov"] > st["cov_peak"]:
                st["cov_peak"] = st["cov"]
                st["last_new"] = st["fork_time"] or st["execs"]
    return st


def count_artifacts(run):
    d = os.path.join(run, "artifacts")
    kinds = {"crash": 0, "oom": 0, "leak": 0, "timeout": 0}
    latest = None
    if os.path.isdir(d):
        for fn in os.listdir(d):
            for k in kinds:
                if fn.startswith(k + "-"):
                    kinds[k] += 1
                    p = os.path.join(d, fn)
                    mt = os.path.getmtime(p)
                    if not latest or mt > latest[0]:
                        latest = (mt, fn)
    return kinds, latest


def bar(label, value, color):
    return f"  {label:<16}{color}{value}{RST}"


# dashboard-computed instantaneous exec/s: diff the running total between
# refreshes so the rate moves even when libFuzzer's own line is between prints.
# Holds the last rate (instead of flickering to 0) until the counter advances.
_prev = {"execs": 0, "t": 0.0, "rate": 0.0}


def live_rate(execs, now):
    p = _prev
    if p["t"] and execs > p["execs"] and now > p["t"]:
        p["rate"] = (execs - p["execs"]) / (now - p["t"])
    if not p["t"] or execs > p["execs"]:
        p["execs"], p["t"] = execs, now
    return p["rate"]


def render(target, start, once):
    run = os.path.join(HERE, "runs", target)
    log = os.path.join(HERE, "logs", f"{target}.log")
    st = parse_log(log)
    kinds, latest = count_artifacts(run)
    now = time.time()
    wall = now - start
    age = now - os.path.getmtime(log) if os.path.exists(log) else 0
    rate = live_rate(st["execs"], now)
    findings = kinds["crash"] + kinds["oom"] + kinds["leak"] + kinds["timeout"]
    fcolor = RED if findings else GRN
    # green when libFuzzer emitted recently, yellow when the line is going stale
    tcolor = GRN if age < 4 else YEL

    out = []
    out.append(f"{BOLD}{CYN}┌─ slippack fuzz · {target} "
               + "─" * max(0, 42 - len(target)) + f"┐{RST}")
    line = lambda s: out.append(f"{CYN}│{RST} {s}")
    line(f"{'run time':<14}{BOLD}{hms(wall)}{RST}"
         f"      {'fuzzer clock':<14}{hms(st['fork_time'])}")
    line(f"{'data age':<14}{tcolor}{age:.1f}s{RST}"
         f"        {'log lines':<14}{st['lines']}")
    out.append(f"{CYN}├─ throughput " + "─" * 44 + f"{RST}")
    line(f"{'total execs':<14}{BOLD}{human(st['execs'])}{RST}"
         f"        {'exec/s live':<12}{tcolor}{human(rate)}{RST}")
    rss = f"{human(st['rss'])}Mb" if st['rss'] else "—"
    line(f"{'exec/s (lF)':<14}{human(st['exec_s'])}"
         f"       {'rss':<12}{rss}")
    out.append(f"{CYN}├─ coverage " + "─" * 46 + f"{RST}")
    line(f"{'edges (cov)':<14}{BOLD}{GRN}{human(st['cov'])}{RST}"
         f"        {'features':<12}{human(st['ft'])}")
    corp = f"{st['corp_n']} inputs"
    if st['corp_b']:
        corp += f" / {human(st['corp_b'])}b"
    line(f"{'corpus':<14}{corp}")
    out.append(f"{CYN}├─ findings " + "─" * 46 + f"{RST}")
    line(bar('crashes', kinds['crash'], RED if kinds['crash'] else GRN).strip())
    line(f"{'oom':<8}{kinds['oom']:<6}{'timeout':<9}{kinds['timeout']:<6}"
         f"{'leak':<6}{kinds['leak']}")
    if latest:
        line(f"{DIM}latest: {latest[1][:52]}{RST}")
    line(f"{'total saved':<14}{fcolor}{BOLD}{findings}{RST}")
    out.append(f"{CYN}└" + "─" * 57 + f"┘{RST}")
    if not once:
        out.append(f"{DIM}refreshing — Ctrl-C to exit dashboard "
                   f"(fuzzer keeps running){RST}")

    body = "\n".join(out)
    if once:
        # strip color for clean log capture
        print(re.sub(r"\x1b\[[0-9;]*m", "", body))
    else:
        sys.stdout.write(ESC + "2J" + ESC + "H" + body + "\n")
        sys.stdout.flush()


def main():
    args = [a for a in sys.argv[1:]]
    once = "--once" in args
    interval = 0.5
    if "-n" in args:
        interval = float(args[args.index("-n") + 1])
    targets = [a for a in args if not a.startswith("-")
               and a not in (str(interval),)]
    target = targets[0] if targets else "parse"
    start = time.time()
    log = os.path.join(HERE, "logs", f"{target}.log")
    if os.path.exists(log):
        stx = os.stat(log)  # anchor run time to the log's first write (birth)
        start = getattr(stx, "st_birthtime", stx.st_ctime)
    if once:
        render(target, start, True)
        return
    try:
        while True:
            render(target, start, False)
            time.sleep(interval)
    except KeyboardInterrupt:
        sys.stdout.write("\n")


if __name__ == "__main__":
    main()
