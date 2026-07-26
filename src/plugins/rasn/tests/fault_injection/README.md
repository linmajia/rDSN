# rASN fault-injection robustness testing

This directory holds an **opt-in, POSIX-only** fault-injection harness that
stresses rASN, CodePilot, and SREPilot with [libfiu](https://blitiri.com.ar/p/libfiu/)
(`fiu-run`). It injects failures into the C library and POSIX syscall layer --
allocation, string duplication, file I/O, and networking -- and checks that the
binaries **degrade gracefully** instead of crashing or corrupting state.

It is intentionally **not** wired into the default CMake build or CI: libfiu is
Linux-only, and the rest of the plugin is portable (including Windows). Run this
by hand when you want a robustness signal.

## What it checks

Every run is classified by how the process terminated:

| Outcome | Meaning | Verdict |
| --- | --- | --- |
| `SIGSEGV` / `SIGBUS` / `SIGFPE` / `SIGILL` | memory unsafety or bad arithmetic | **genuine defect** |
| `HANG` (a `TIMEOUT` that still timed out on a much longer re-run) | deadlock | **genuine defect** |
| `SIGABRT` | assertion or allocation failure in a target without a CLI handler -- fail-stop | acceptable |
| `TIMEOUT_transient` (a `TIMEOUT` that completed within a longer re-run) | slow fail-stop teardown artifact | acceptable |
| non-zero exit | operation returned an error, or a CLI allocation handler terminated promptly | acceptable |
| exit `0` | fault absorbed, or not on the exercised path | acceptable |

The script exits non-zero only if it sees a genuine crash-class outcome.

Every raw crash is confirmed before it is counted, because injecting failures
into a complex runtime can produce artifacts that are not defects in the code
under test:

* A **hard signal** (`SIGSEGV`/`SIGBUS`/`SIGFPE`/`SIGILL`) is re-run at the normal
  timeout and counts only if the same class of signal recurs. A rare (~1 run in
  100) libc/runtime bootstrap crash -- e.g. an I/O fault corrupting config-file
  load *before any application code runs* (the kept log is empty) -- does not
  recur and is reported as `<sig>_transient`.
* A **`TIMEOUT`** is re-run with a much longer timeout (`max(4×, 60s)`). A genuine
  deadlock still never returns and is promoted to `HANG`; slow fail-stop teardown
  in a target without an allocation handler completes when given more time and is
  reported as `TIMEOUT_transient`.

## Requirements

* Linux with libfiu: `sudo apt-get install fiu-utils libfiu-dev`
  (provides `fiu-run`; the preload catalog lives under `/usr/lib/fiu/`).
* rASN plugins built: from the repo root, `./run.sh build --build_plugins`.
  This produces `builder/bin/{codepilot,srepilot,rasn.unit_tests}/`.

## Running

```sh
# from anywhere, after building the plugins
src/plugins/rasn/tests/fault_injection/run_fault_injection.sh
```

The harness is self-contained: it never writes into the caller's working
directory. Logs go to `RASN_FI_OUT` (default:
`src/plugins/rasn/tests/fault_injection/out/`, resolved to an absolute path),
and every child process is launched with its working directory set to
`$RASN_FI_OUT/work`. rASN's default durable state, spilled artifacts, and JSONL
trace paths are all CWD-relative, so this keeps `rasn/state`, `rasn/artifacts`,
and `rasn/traces` inside the output directory instead of scattering them across
the repository. The default output directory is covered by
`src/plugins/rasn/.gitignore`.

Useful knobs (environment variables):

```sh
RASN_FI_RUNS=40 \
RASN_FI_TIMEOUT=30 \
RASN_BIN_DIR=/path/to/builder/bin \
  src/plugins/rasn/tests/fault_injection/run_fault_injection.sh
```

Core dumps are disabled by default (`ulimit -c 0`). rDSN installs a crash
handler that writes a core to `data/coredumps` on abort; under heavy repeated
allocation-fault injection that write can take longer than the per-run timeout.
CodePilot and SREPilot install a non-allocating C++ allocation-failure handler
before their first allocation, so they exit immediately instead of entering
that teardown.
The harness also enforces the per-run timeout with **SIGKILL** rather than the
default SIGTERM: rDSN installs a SIGTERM handler that runs a full, allocating
`dsn_exit()` cleanup, which can itself stall under continuous allocation-fault
injection, so a SIGTERM-based timeout would measure rDSN's shutdown path instead
of whether the program made progress. Together with the extended-timeout
`TIMEOUT` confirmation described above, this keeps the pass/fail verdict stable.
Set `RASN_FI_KEEP_CORES=1` to keep cores if you actually want them for
post-mortem debugging.

## Fault matrix

`fiu-run -x` preloads libfiu's POSIX/libc wrappers; each `-c` enables a randomly
firing fault point. The harness exercises these cells against each target:

| Config | Injected points | Probability |
| --- | --- | --- |
| `malloc_5` / `malloc_20` | `libc/mm/*` (`malloc`,`calloc`,`realloc`) | 0.05 / 0.20 |
| `strdup_15` | `libc/str/*` (`strdup`,`strndup`) | 0.15 |
| `io_rw_5` | `posix/io/rw/*` (`read`,`write`,`pread`,`pwrite`,...) | 0.05 |
| `io_all_15` | `posix/io/*` (open/close/rw/dir/sync/net) | 0.15 |
| `net_25` | `posix/io/net/*` (`socket`,`connect`,`send`,`recv`,...) | 0.25 |
| `mixed_10` | `libc/mm/*` + `posix/io/*` | 0.10 each |

Targets, chosen for attribution:

1. **`codepilot schema {json,idl}`** -- pure rASN string/serialization work that
   runs *before* the rDSN runtime is initialized, so faults are cleanly
   attributable to rASN.
2. **`rasn.unit_tests`** (filter `rasn_*.*:codepilot_*.*`, 91 tests) -- the whole
   engine: state checkpoints/journals, workflow leases and recovery, LLM replay,
   agent runtime, and the resilience gates.
3. **`codepilot workflow validate|compile <file>`** -- filesystem read + parse.
4. **`codepilot providers|tools`, `srepilot help`** -- registry and
   runtime-initialization surfaces for both applications.

## Reference results

Campaign on Ubuntu 16.04, libfiu 0.94, plugins built with GCC 8.4. Cells show
the outcome distribution (`ok` = exit 0, `exitN` = graceful error, `SIGABRT` =
`bad_alloc` fail-stop). **No `SIGSEGV`, `SIGBUS`, or deadlock was observed on any
target under any fault mode.**

These historical CodePilot and SREPilot results predate their allocation-failure
handler. Current CLI allocation failures terminate promptly with a non-zero
exit instead of entering the `SIGABRT` teardown path.

```
codepilot.schema.json     malloc_* -> SIGABRT (100%)   io/net/str -> ok (no I/O on path)
rasn.unit_tests           io_rw_5  -> 86/90 pass; the 4 failures are exactly the
                                      I/O-dependent tests (state checkpoint/recover,
                                      workflow lease/cancel-recover, llm replay):
                                      the injected read/write error is propagated,
                                      the process exits 1, nothing crashes.
                          malloc_* -> SIGABRT (bad_alloc)   net_25 -> graceful exit1
codepilot.workflow.*      io_*     -> ok / graceful exit1   malloc_* -> SIGABRT
codepilot.providers/tools io/net   -> ok                    malloc_* -> SIGABRT
srepilot.help             io/net   -> ok                    malloc_* -> SIGABRT
```

Interpretation: rASN encodes each state-journal record before I/O and appends it
with one write. A short write is rolled back, and a later append discards only
an unterminated tail, so injected I/O failures cannot poison subsequent
recovery. CodePilot and SREPilot handle process-wide C++ allocation failure with
a fixed diagnostic and immediate non-zero exit, avoiding allocation-dependent
teardown. Per-request overload and dependency failures are contained earlier by
the circuit breaker, admission gate, rate limiter, and token/cost budget.
