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

## Configuration

SREPilot has an app-local `config.ini` in this folder. Its CMake target copies
that config beside the built `srepilot` executable, keeping SREPilot service-app
configuration separate from CodePilot's shared config.

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
