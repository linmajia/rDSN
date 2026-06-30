# rASN examples

These examples are intentionally small and deterministic enough to run with the
local simulator provider. They exercise the generic rASN service graph rather
than CodePilot-specific shortcuts.

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow validate src\plugins\rasn\examples\generic-multi-agent.workflow
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow run src\plugins\rasn\examples\generic-multi-agent.workflow example-run
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow compile src\plugins\rasn\examples\optimized-coding.workflow
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe selftest
```

`generic-multi-agent.workflow` uses a read-only `tool.run` node followed by a
`model.complete` node. `state-checkpoint.workflow` is useful when validating
state checkpoint/recovery after a workflow run.

`optimized-coding.workflow` adds `latency_ms`, `cost_hint`, and `reliability`
metadata. The compiled plan reports stages, maximum parallelism, critical-path
latency, cost units, and minimum reliability.

For rDSN service-mode RPC validation, launch the example config:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe --dsn src\plugins\rasn\examples\service-rpc-smoke.ini
```

The `rasn.codepilot` service app runs `selftest` through the rDSN service graph.
The rDSN process remains alive after the smoke output; stop it from the terminal
when validation is complete.
