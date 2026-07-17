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
codepilot.exe state put codepilot/example "value"
codepilot.exe state query rasn/runtime
codepilot.exe state migrate rasn/state/export.chkpt --prefix rasn/runtime
codepilot.exe interactive
codepilot.exe C:\path\to\repo
codepilot.exe C:\path\to\file.cpp
```

State keys are stored as `<scope>/<id>`. For CLI compatibility, a bare CodePilot
`state put|get` key is resolved as `codepilot/<key>`; explicit namespaced keys stay
unchanged. `state put` derives record `scope` metadata from the resulting key, so
`state put other/foo value` stores `key=other/foo` and `scope=other`.

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

## Runtime placement and standalone host

CodePilot always loads its own thin `config.ini` for app commands. Runtime
placement comes only from `[rasn.runtime] rasn_runtime_provider`: `local` runs the
module graph in-process, while `distributed`/`hybrid` starts a lightweight rDSN
client node and calls the configured remote runtime. No mode flag is required.

To deploy the runtime independently, put `config.rasn.ini` and
`config.rasn.defaults.ini` beside the executable and start the standalone,
services-only host:

```bat
codepilot.exe serve
```

The runtime host never includes CodePilot's app config or co-hosts its gateway;
`config.rasn.ini` includes only the shared module tuning defaults. An explicit
`codepilot.exe serve <config> [app_list]` selects another host config or a subset
of runtime roles. `--dsn` remains only as a deprecated alias of `serve`.
