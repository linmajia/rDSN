# CodePilot

CodePilot is the first rASN application adapter. It is a coding CLI that routes
model requests, local tools, workflows, state, and observability through the
generic rASN service graph instead of calling providers or tools directly.

## Build

The app is built by the `codepilot` CMake target under the rASN plugin:

```bat
C:\Users\haoxlin\source\repos\rdsn\copilot-worktrees\rDSN\ideal-system\ext\cmake-3.22.6\bin\cmake.exe --build C:\Users\haoxlin\source\repos\rdsn\rb-rasn --target codepilot --config Debug -- /m
```

The executable is usually:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe
```

Before running direct commands, put the rDSN runtime DLL directory on `PATH`:

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
```

## Common commands

```bat
codepilot.exe selftest
codepilot.exe ask "Explain how to add a new rDSN plugin"
codepilot.exe -p "Explain the current workspace"
codepilot.exe --provider ollama --model llama3.1 --print "Summarize src/plugins/rasn"
codepilot.exe --cwd C:\path\to\repo --prompt "Find likely build issues"
codepilot.exe plan "Add a retry budget to an rASN workflow"
codepilot.exe agent "Inspect the rASN state service and suggest tests"
codepilot.exe tools
codepilot.exe observe resilience
codepilot.exe interactive
codepilot.exe C:\path\to\repo
codepilot.exe C:\path\to\file.cpp
```

```sh
./codepilot ~/src/repo
./codepilot ~/src/repo/file.cpp
```

A single existing directory or file argument starts interactive mode instead of
being sent as a prompt. A directory becomes the process workspace and loads a
bounded source-file index plus selected file excerpts as initial context. A file
makes its parent the workspace and loads the file as initial context.
Directory snapshots skip generated/build output and obvious secret-bearing files
such as `.env*`, credentials, private keys, and
`secrets.{json,yml,yaml}` / `config.{json,yml,yaml}`. If source
enumeration fails, CodePilot still enters interactive mode with the workspace set
and reports that source context is unavailable.

CodePilot also accepts common coding-CLI aliases inspired by OpenCode, Codex CLI,
GitHub Copilot CLI, and Claude Code CLI: `-p` / `--print`, `--prompt`, `-m` /
`--model`, `--provider`, `--cwd` / `--workspace` / `--dir`, `--resume`,
`--continue`, `--approval`, `--sandbox`, `--yes`, `--dry-run`, `--help`, and
`--version`. Compatibility aliases are normalized onto rASN commands and service
graph state rather than bypassing the rASN runtime.

Inside interactive mode, prefix commands with `/`; text without `/` is sent as an
ask prompt. For example:

```text
/provider simulator
/context src/plugins/rasn/apps/codepilot/codepilot_app.cpp
/ask Summarize this file
/exit
```

## Configuration

`apps/codepilot/CMakeLists.txt` copies the shared rASN `config.ini` beside the
built CodePilot target. Provider settings, policy settings, tool sandboxing,
trace output, and state checkpoint settings are read from that file.

By default, local runtime files are grouped under a single `rasn` directory in
the process working directory:

```text
rasn/state      durable state checkpoints and journals
rasn/artifacts  spilled tool-output artifacts
rasn/traces     runtime JSONL traces
```

CodePilot can run entirely offline with the simulator provider. It can also use
Copilot-compatible, OpenAI-compatible, Ollama, llama.cpp, or LM Studio endpoints
through the generic rASN provider layer.

## Service mode

Direct commands start an inline rASN service graph in-process. Service mode runs
the rDSN app roles and the CodePilot gateway. You never name a config file on the
command line — CodePilot loads its own thin `config.ini` by default, and in
`--dsn` mode auto-loads the shared runtime `config.rasn.ini` when it is deployed
next to the binary:

```bat
codepilot.exe --dsn
```

To host the full service fleet in this one process, drop the shared
`config.rasn.ini` (from `src/plugins/rasn/config.rasn.ini`) next to the executable;
`--dsn` auto-loads it, and it `@include`s CodePilot's own `config.ini` to co-host
the gateway. Without that overlay, `--dsn` falls back to the thin `config.ini` and
still starts on built-in default configuration values. (Run the binary from the
directory holding these files so the relative include resolves.) The `--dsn` path
selects the CodePilot app list explicitly, so additional rASN applications can
share the repository without requiring this executable to register their
service-app types.
