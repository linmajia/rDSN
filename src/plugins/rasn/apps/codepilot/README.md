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
codepilot.exe plan "Add a retry budget to an rASN workflow"
codepilot.exe agent "Inspect the rASN state service and suggest tests"
codepilot.exe tools
codepilot.exe observe resilience
codepilot.exe interactive
```

Inside interactive mode, prefix commands with `/`, for example:

```text
/provider simulator
/context src\plugins\rasn\apps\codepilot\codepilot_app.cpp
/ask Summarize this file
/exit
```

## Configuration

`apps/codepilot/CMakeLists.txt` copies the shared rASN `config.ini` beside the
built CodePilot target. Provider settings, policy settings, tool sandboxing,
trace output, and state checkpoint settings are read from that file.

CodePilot can run entirely offline with the simulator provider. It can also use
Copilot-compatible, OpenAI-compatible, Ollama, llama.cpp, or LM Studio endpoints
through the generic rASN provider layer.

## Service mode

Direct commands start an inline rASN service graph in-process. Service mode runs
the rDSN app roles and the CodePilot gateway:

```bat
codepilot.exe --dsn
codepilot.exe --dsn C:\path\to\config.ini
```

The `--dsn` path selects the CodePilot app list explicitly, so additional rASN
applications can share the repository without requiring this executable to
register their service-app types.
