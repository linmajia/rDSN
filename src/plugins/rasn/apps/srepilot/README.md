# SREPilot

SREPilot is the second rASN application adapter. It is an SRE / incident-response
CLI that uses the same rASN model gateway, state service, observability queries,
metrics, and resilience controls as CodePilot, but targets production-operations
workflows.

## Build

The app is built by the `srepilot` CMake target under the rASN plugin:

```bat
C:\Users\haoxlin\source\repos\rdsn\copilot-worktrees\rDSN\ideal-system\ext\cmake-3.22.6\bin\cmake.exe --build C:\Users\haoxlin\source\repos\rdsn\rb-rasn --target srepilot --config Debug -- /m
```

The executable is usually:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\srepilot\Debug\srepilot.exe
```

Before running direct commands, put the rDSN runtime DLL directory on `PATH`:

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
```

## Common commands

```bat
srepilot.exe selftest
srepilot.exe status
srepilot.exe diagnose "checkout latency p95 doubled after the last deployment"
srepilot.exe runbook "database connection pool exhaustion"
srepilot.exe observe snapshot
srepilot.exe observe events
srepilot.exe observe failures
srepilot.exe observe metrics text
srepilot.exe observe resilience
srepilot.exe provider simulator
srepilot.exe interactive
```

Inside interactive mode, prefix commands with `/`. Plain text without a slash is
treated as incident input for `diagnose`.

## Incident records

`diagnose` and `runbook` persist generated artifacts through the rASN state
service under the `srepilot/` key namespace. They also checkpoint state so a later
`status` command can recover and summarize prior incident records.

Before the first `diagnose` or `runbook` persist in each process, SREPilot
recovers configured state once. This is important for direct one-shot CLI usage:
each command starts a fresh process with an empty in-memory state store, so
recovering before checkpointing prevents a new incident from overwriting prior
records.

Recovery sources are treated as authoritative and fail-safe:

- `[rasn.state] recover_on_start` points to an explicit checkpoint path. If it is
  set, SREPilot attempts that recovery path before writing new incident records;
  recovery errors fail the command instead of checkpointing a partial store.
- The default local checkpoint/journal from `[rasn.state] checkpoint_dir`,
  `checkpoint_file`, and `journal_file` is auto-recovered when present. The
  app-local defaults store these files under `rasn/state`.
- `[rasn.state.replica] enabled = true` with `recover = true` lets recovery seed
  missing primary checkpoint/journal files from a local replica directory before
  trying NFS. Keep `directory` non-empty when enabling this; an empty replica
  directory is a configuration error and should block persistence rather than
  silently disabling the safeguard.
- `[rasn.state.nfs] enabled = true` lets recovery import checkpoint/journal files
  from an rDSN NFS source when no local or replica state is available. If the NFS
  source is unreachable, direct `diagnose` and `runbook` commands wait up to
  `[rasn.state.nfs] timeout_ms` and then fail rather than risk clobbering older
  incident records.

`[rasn.state.nfs] timeout_ms` is in milliseconds and defaults to `20000` (20
seconds). That default is intentionally conservative for service startup or
remote state seeding, where successful recovery is usually more important than a
fast failure. For human-driven direct CLI use with a local or same-datacenter NFS
source, `5000` is a good starting value; use `3000`-`5000` for fail-fast
operations, and keep or raise `20000` when checkpoints are large, the source is
remote, or avoiding false recovery failures matters more than CLI latency.

## Configuration

SREPilot has an app-local `config.ini` in this folder. Its CMake target copies
that config beside the built `srepilot` executable, keeping SREPilot service-app
configuration separate from CodePilot's shared config.

By default, local runtime files are grouped under a single `rasn` directory in
the process working directory:

```text
rasn/state      durable state checkpoints, journals, and incident records
rasn/artifacts  spilled tool-output artifacts
rasn/traces     runtime JSONL traces
```

The default provider is the offline simulator. Hosted or local model providers
use the same `[rasn.llm]`, `[rasn.model]`, `[rasn.state]`, `[rasn.runtime]`, and
resilience sections documented in the top-level rASN README.

## Service mode

Direct commands start an inline rASN service graph in-process. Service mode runs
the rDSN app roles and the SREPilot gateway:

```bat
srepilot.exe --dsn
srepilot.exe --dsn C:\path\to\config.ini
```

The `--dsn` path selects the SREPilot app list explicitly, so this executable
does not need to register CodePilot's service-app type.
