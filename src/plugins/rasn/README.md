# Robust Agent System Nucleus (rASN)

rASN is a prototype framework for building robust AI agent systems on top of rDSN. It treats agent execution as a task-centric distributed runtime problem: every agent step is represented as an explicit task, runtime nondeterminism is captured as data, and workflow execution is exposed as an instrumented graph that can be traced, diagnosed, and replayed.

The first sample application is **rASN CodePilot** (short name: **CodePilot**), a coding CLI inspired by OpenCode, Codex CLI, and GitHub Copilot CLI. It can run entirely offline with a random simulator, or connect to OpenAI-compatible APIs such as GitHub Copilot-style endpoints, Ollama, llama.cpp, and LM Studio.

## Design

The generic multi-agent architecture is documented in `docs/DESIGN.md`, the
component-by-component implementation plan is tracked in
`docs/IMPLEMENTATION_PLAN.md`, and the ACM-style technical report lives under
`docs/report/`.

The prototype is intentionally small and is organized around four building blocks:

| Component | Files | Purpose |
| --- | --- | --- |
| rDSN service graph | `agent_services.*`, `rasn.code.definition.h` | Defines rDSN-style micro-service roles for coordinator, model agent, tool agent, state service, workflow service, observability service, and CLI gateway; service mode exposes those roles as `serverlet` RPC services with `clientlet` callers and explicit `RPC_RASN_*` task codes. |
| Runtime nucleus | `rasn_core.*`, `observability.*`, `schema_manifest.*` | Creates task IDs, trace IDs, structured JSONL runtime events, nondeterministic value capture, external-effect ledger entries, filesystem snapshots, replay lookup, failure records, schema/IDL/JSON manifests, generated C++/TypeScript/Python RPC clients, and observability query APIs. |
| LLM provider layer | `llm_provider.*` | Provides a common `llm_provider` interface and adapters for simulator, Copilot-compatible, Ollama, llama.cpp, LM Studio, and generic OpenAI-compatible HTTP APIs. |
| Tool provider layer | `agent_tools.*` | Defines the generic rASN tool-provider contract and registration factory used by the tool-agent service. |
| Workflow graph | `workflow.*` | Parses a declarative task graph, validates dependencies, topologically orders nodes, and executes them through the selected provider. |
| CodePilot tools | `codepilot/local_tools.*` | Implements and registers the CodePilot application tool provider: project inspection tools, read/search, and opt-in shell/write execution. |
| CodePilot skills | `codepilot/skills.*` | Provides reusable coding-agent skill prompts for rDSN plugin work, code review, build debugging, feature planning, and documentation. |
| CodePilot app | `codepilot/codepilot_app.*`, `main.cpp` | Exposes one-shot and interactive coding-agent commands as an adapter that builds generic `agent_request` messages and routes them through the rASN coordinator. |

### rDSN micro-service model

rASN models an agent system like a distributed service system. The current prototype registers these rDSN app roles:

```text
rasn.registry       stores agent descriptors and capability metadata
rasn.coordinator    orchestrates task graph execution and routes service calls
rasn.llm.agent      isolates nondeterministic LLM completion behind the model gateway
rasn.tool.agent     isolates local tool side effects behind explicit opt-in policies
rasn.state          stores namespaced state and checkpoints
rasn.workflow       validates, compiles, and executes declarative agent graphs
rasn.observability  queries events, failures, snapshots, and replay loading
rasn.codepilot      exposes rASN CodePilot as a gateway service
```

The CLI does not directly call an LLM provider or local tool. It sends requests through a `rasn_service_graph`, where the coordinator dispatches work to the LLM or tool agent. In rDSN service mode, the graph follows the same pattern as `apps.echo` and `apps.skv`: each agent role registers `serverlet` handlers, clients use `clientlet` wrappers, and requests flow through explicit RPC task codes:

```text
RPC_RASN_AGENT_DESCRIBE
RPC_RASN_AGENT_INVOKE
RPC_RASN_AGENT_CANCEL
RPC_RASN_AGENT_HEARTBEAT
RPC_RASN_AGENT_QUERY
RPC_RASN_REGISTRY_*
RPC_RASN_STATE_*
RPC_RASN_WORKFLOW_*
RPC_RASN_MODEL_*
RPC_RASN_OBSERVABILITY_*
```

The shared service graph is retained by each rDSN app wrapper that depends on it
and is torn down only after the last owner releases it. This prevents shutdown of
one service wrapper from unregistering local agents, state writers, or heartbeat
timers while sibling services are still running.

CodePilot configures its local tool provider during construction but does not
start the graph until a command is actually running. Direct one-shot commands and
REPL sessions hold a command-scoped graph reference; service mode additionally
holds the `rasn.codepilot` app reference and waits for required service RPCs
before running commands. Workflow service startup schedules state recovery after
the app graph has had time to register RPC handlers, avoiding brittle dependence
on rDSN's concurrent app startup order.

The default service-mode endpoints are `localhost` plus fixed ports:

```text
rasn.registry       27100
rasn.coordinator    27101
rasn.llm.agent      27102
rasn.tool.agent     27103
rasn.state          27104
rasn.workflow       27105
rasn.observability  27106
```

For multi-process or resolver-backed deployments, `[rasn.service]` also accepts a
shared `host`, per-service `<name>_host`, and per-service `<name>_uri` values.
When `<name>_uri` is present, rASN constructs the client address with rDSN's
`url_host_address`, so values such as `dsn://cluster/rasn.llm.agent` can be
resolved by `dsn.dist.uri.resolver`; otherwise it falls back to `<name>_host` and
`<name>_port`.

Standalone one-shot CLI mode still initializes the rDSN runtime with the plugin `config.ini`, but starts no rDSN service apps. It lazily starts the shared graph for the duration of the command and uses the same service implementations inline for fast local commands, while service mode starts the full rDSN app graph and routes through RPC.

This keeps the prototype close to rDSN principles: explicit service roles, lifecycle-managed apps, config-driven ports, typed task-code RPC dispatch, rDSN config, rDSN logging, rDSN locks, traceable runtime events, and replaceable providers/tools.

`rasn.registry` also scans static `[rasn.agent.*]` sections at startup, so external
or experimental agents can be described without changing CodePilot code. For
example:

```ini
[rasn.agent.remote-reviewer]
agent_id = remote-reviewer
role = model
app_name = rasn.remote.reviewer
host = 127.0.0.1
port = 27200
version = dev
health = healthy
capabilities = model.complete
side_effect_class = nondeterministic
```

Static descriptors are loaded in both direct CLI mode and rDSN service mode.
In service mode, `registry list|get|query` talks to the live `rasn.registry`
service through typed RPC instead of reading a local snapshot.

### Runtime model

Each agent operation is represented as an `agent_task`:

```text
task id + task name + input
```

The runtime records events such as:

```text
task.begin
llm.request
llm.response
tool.ok
tool.error
nondeterminism
workflow.node.start
workflow.node.finish
external.effect
replay
task.finish
failure
replay.load
replay.miss
```

If `[rasn.runtime] trace_file` is configured or the CLI `trace <file>` command is used, events are appended as JSON lines. Simulator randomness is recorded as a `nondeterminism` event and can be reused later with `replay <trace-jsonl>` or `observe replay <trace-jsonl>`. Model and tool results are also replay-aware: when a replay trace contains recorded `llm.response` events for the provider, `rasn.llm.agent` returns those responses in provider order instead of contacting the provider; when it contains a matching `tool.ok` or `tool.error` event, `rasn.tool.agent` returns that recorded result instead of invoking the provider again. Workflow execution records `workflow.node.start`/`workflow.node.finish` transitions, and replay mode checks recorded node-start order before executing a node. If replay mode is active and a side-effecting tool has no recorded result, the runtime returns an explicit replay error rather than re-running a write or shell command.

CodePilot read/list/search tools also record `filesystem.snapshot` events with
stable path fingerprints. During replay, the tool boundary checks those
fingerprints before returning recorded file-tool results or reading the current
workspace, so filesystem drift is surfaced as a replay miss rather than hidden
inside stale context.

Use the observability service to inspect runtime behavior in direct or rDSN service mode:

```bat
codepilot.exe observe events
codepilot.exe observe events nondeterminism
codepilot.exe observe failures
codepilot.exe observe timeline
codepilot.exe observe diagnose
codepilot.exe observe snapshot
```

Use `codepilot.exe schema` to inspect the runtime message contract manifest for
core structs such as `agent_request`, `agent_response`, `policy_request`,
`runtime_event`, `state_record`, and `workflow_node`. The same descriptor table
also exports JSON/IDL, C++/TypeScript/Python contract stubs, and generated
C++/TypeScript/Python RPC client wrappers.

`observe snapshot` also writes a compact `observability-snapshot/<trace>/<sequence>`
index record through the rASN state service boundary. In service mode this flows
through `rasn.state` RPC, giving operators a durable pointer to the trace file,
event count, failure count, and last observed sequence without coupling
observability to an in-process state store.

### Tool calling model

The CLI supports direct local tools:

```bat
codepilot.exe tool list C:\path\to\project
codepilot.exe tool read C:\path\to\project\README.md
codepilot.exe tool search C:\path\to\project "class service_app"
```

The `agent <prompt>` command lets the model request one tool call at a time using this protocol:

```text
RASN_TOOL <tool> <args>
```

CodePilot exposes structured descriptors for the built-in tools through the
generic tool provider. Each descriptor includes a name, side-effect class,
description, and argument list; the human-readable `tools` command is rendered
from those descriptors instead of a separate free-form list.

The CLI executes the tool through the generic rASN policy manager, records the bounded tool result in the runtime trace, appends the result as context, and asks the provider to continue. The CodePilot tool provider also re-checks the same policy at its own boundary, so future direct provider uses keep the safe default even if they bypass `rasn.tool.agent`. Oversized tool outputs spill to an artifact file and are indexed through the rASN state service boundary, so service mode stores the artifact metadata through `rasn.state` RPC instead of a hidden in-process write. Shell execution is disabled by default and requires this in `config.ini`:

```ini
[rasn.policy]
allow_shell = true
```

This makes local side effects explicit and opt-in. Every admitted, denied, replayed, or replay-missing side-effecting tool call also records an `external.effect` event with an effect class, operation, stable fingerprint, replay policy, and status. When `require_shell_approval = true`, CodePilot asks for an interactive confirmation before adding the `human_approved:shell` policy label to the generic tool request. Direct scripted invocations can use `tool --yes shell ...` to provide that explicit approval non-interactively. When `shell_allowed_commands` is non-empty, only those executable names can run and shell metacharacters such as `&&`, pipes, redirection, and semicolons are rejected before execution. `shell_working_directory` can pin shell commands to a configured working directory; if unset, `workspace_root` is used as the shell working directory when present. `shell_executor = container` can route an approved shell command through `shell_container_template` with `{command}`, `{raw_command}`, `{workspace}`, and `{raw_workspace}` placeholders; the default `local` executor preserves existing behavior.

File editing tools are also opt-in:

```ini
[rasn.policy]
allow_write = true
```

Then run:

```bat
codepilot.exe tool --yes write C:\temp\example.txt "hello"
codepilot.exe tool --yes replace C:\temp\example.txt "hello" "hello from rASN"
```

When write tools are enabled, `require_write_approval = true` requires either an interactive confirmation or `tool --yes` before the policy label reaches `rasn.tool.agent` and the provider-side defense-in-depth check. CodePilot writes through a temporary file and `dsn::utils::filesystem::rename_path` rather than truncating the target in place. This keeps failed writes from leaving a partially written target file.

### CodePilot skill model

CodePilot skills are named reusable prompts owned by the CodePilot adapter, not
generic rASN runtime concepts. The built-in skills are:

```text
rdsn-plugin
code-review
debug-build
feature-plan
docs
```

Use `skills` to list them, `skill <name>` to inspect one, and `skill <name> <task>` to apply one to a prompt.

### Provider model

All providers implement:

```cpp
class llm_provider
{
public:
    virtual std::string name() const = 0;
    virtual std::string model() const = 0;
    virtual llm_response complete(const llm_request &request, nucleus_runtime &runtime) = 0;
};
```

Network providers currently call `curl` so the prototype can stay dependency-light and build inside the existing rDSN C++ plugin system.

### Workflow model

A workflow file describes an executable agent graph:

```text
task inspect plan Inspect the target files
task patch ask Suggest the patch after inspect
task verify ask Suggest validation after patch
```

Each line has this form:

```text
task <id> <ask|plan> <prompt> [after dependency1,dependency2]
```

Nodes can also declare generic runtime metadata:

```text
capability <model.complete|tool.run> policy <label> budget_ms <ms> retry_budget <n> state <scope/key> artifact <path>
```

Optimizer hints are optional:

```text
cost_hint <units> latency_ms <ms> reliability <0-100>
```

The compiler keeps dependency order stable, orders ready nodes by optimizer score,
and reports stages, maximum parallelism, critical-path latency, cost units, and
minimum reliability.

`budget_ms` is also an execution budget. In rDSN service mode, a nonzero node
budget is propagated into `agent_request.timeout_ms` and used as the client-side
RPC deadline for registry/coordinator/agent calls. HTTP model providers also use
the smaller of this budget and `[rasn.model] request_timeout_sec` as the curl
`max-time`. A budget of `0` falls back to the global `[rasn.rpc] timeout_ms`
setting and provider request timeout. Agents also keep a bounded in-flight
request table, so `agentctl cancel ... <request-id>` marks matching work as
cancelled and prevents a completed result from being surfaced after cancellation.
Cancellation tombstones for still-running requests are retained even under later
cancel traffic; only completed request tombstones are evicted to bound memory.
The current deadline and cancel path remain cooperative: arbitrary local provider
work that already started may run until its provider/tool returns.
`retry_budget` is an opt-in count of additional attempts for retryable model
agent failures. The coordinator records each retry in the runtime trace, caps the
value with `[rasn.coordinator] max_retry_budget`, and does not retry `tool.*`
capabilities to avoid duplicating side effects.

When a workflow runs through `rasn.workflow`, each node publishes state updates
under `workflow-node/<run-id>/<node-id>`. The latest record contains the node
action, status (`running`, `completed`, `resumed`, `failed`, `blocked`, or
`cancelled`), output, and error text. The append-only state journal preserves the
intermediate transitions. Use `workflow nodes <run-id>` to inspect the latest
per-node records. `workflow resume <file> <run-id>` reloads completed node
outputs from `rasn.state`, skips those nodes, and resumes downstream execution
through the same rDSN workflow service path. Before execution starts, the service
acquires a durable state-backed owner lease under `workflow-lease/<run-id>` using
conditional `rasn.state` writes; duplicate active starts/resumes are rejected, and
stale leases can be taken over after `[rasn.workflow] execution_lease_ms`. Active
runs renew their lease periodically using the smaller configured-safe interval
from `[rasn.workflow] execution_lease_renew_ms` (or one third of the lease TTL
when set to `0`), so long-running nodes do not accidentally become stealable
while they still own execution. In rDSN service mode, whole-run records,
per-node progress, resume queries, and lease CAS operations are routed through
the `rasn.state` client/server boundary; inline CLI mode uses the same APIs with
the local state store behind the service graph.
`workflow cancel <run-id>` is cooperative and state-backed: if the run is not in
the current process, the workflow service first recovers `workflow/<run-id>` from
`rasn.state`, then writes the `cancelled` transition through a conditional state
update. If that durable transition fails, cancellation returns an explicit error
instead of claiming success. The running node is allowed to finish, then later
nodes observe the cancellation request, persist `cancelled`, release the
execution lease, and the run remains terminally cancelled instead of being
overwritten as completed. `workflow query <run-id>` can recover the persisted
whole-run record from the state checkpoint/journal when the current process has
not seen the run in memory; if `rasn.state` is unreachable or returns a
non-missing-key error, that failure is surfaced instead of being treated as a
missing workflow. In service mode, `rasn.state` automatically loads the
configured checkpoint or journal at startup when one exists, so `rasn.workflow`
can hydrate persisted runs during a normal service restart without requiring an
operator to set `recover_on_start` for the default paths.
CodePilot local tools normalize path separators at execution time so workflow
examples authored on Windows can be smoke-tested in Linux WSL.

## Repeatable examples

Example workflows live under `src\plugins\rasn\examples` and run with the local
simulator provider.

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow validate src\plugins\rasn\examples\generic-multi-agent.workflow
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow run src\plugins\rasn\examples\generic-multi-agent.workflow example-run
```

- `generic-multi-agent.workflow` runs a read-only `tool.run` node and feeds the
  result to a `model.complete` node.
- `state-checkpoint.workflow` records workflow progress in namespaced state keys
  and is useful when validating checkpoint/recovery.
- `optimized-coding.workflow` demonstrates latency/cost/reliability hints and
  optimizer plan output.
- `service-rpc-smoke.ini` starts all rDSN service apps and runs `selftest`
  through the `rasn.codepilot` gateway app.

## Build

From the rDSN repository root:

```bat
cd /d C:\Users\haoxlin\source\repos\rdsn\copilot-worktrees\rDSN\ideal-system
set DSN_ROOT=C:\Users\haoxlin\source\repos\rdsn\copilot-worktrees\rDSN\ideal-system\install
set DSN_AUTO_TEST=1
call .\run.cmd build --build_plugins -d C:\Users\haoxlin\source\repos\rdsn\rb-rasn -b "C:\Users\haoxlin\source\repos\rdsn\rDSN\ext\boost_1_84_0"
```

The short build directory avoids Windows path-length issues in external dependencies.

After building, the CodePilot executable is:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe
```

Before running it directly, put the rDSN runtime DLL directory on `PATH`:

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
```

## Run CodePilot

### 1. Run with the built-in simulator

The simulator requires no network or LLM server.

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe ask "Explain how to add a new rDSN plugin"
```

To capture a trace by default, set this in the built `config.ini` next to the CodePilot target, for example `C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\config.ini`:

```ini
[rasn.runtime]
trace_file = C:\Users\haoxlin\source\repos\rdsn\rb-rasn\rasn-trace.jsonl
```

Replay the captured nondeterministic simulator choice and any matching recorded
model/tool results:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe replay C:\Users\haoxlin\source\repos\rdsn\rb-rasn\rasn-trace.jsonl ask "Explain how to add a new rDSN plugin"
```

### 2. Start interactive mode

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe interactive
```

Inside the REPL:

```text
/help
/provider simulator
/trace C:\temp\rasn.jsonl
/context src\plugins\rasn\codepilot\codepilot_app.cpp
/plan Add a new command to CodePilot
/ask Summarize the current rASN design
/exit
```

Plain text without a slash is treated as an `ask` prompt.

### 3. Run a workflow

Create a workflow file, for example `C:\temp\rasn.workflow`:

```text
task inspect plan Inspect the files relevant to CodePilot
task design plan Design the CLI change after inspect
task answer ask Explain the implementation plan after design
```

Workflows can also be written as structured JSON when richer tooling or editor
support is useful:

```json
{
  "nodes": [
    {
      "id": "inspect",
      "action": "tool",
      "prompt": "list .",
      "capability": "tool.run",
      "policy_labels": ["read_only"],
      "budget_ms": 5000,
      "latency_hint_ms": 50,
      "cost_hint": 1,
      "reliability_hint": 98,
      "state_key": "unit/inspect"
    },
    {
      "id": "summarize",
      "action": "ask",
      "prompt": "Summarize the inspection result",
      "depends_on": ["inspect"],
      "retry_budget": 1,
      "state_key": "unit/summary"
    }
  ]
}
```

Run it:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow C:\temp\rasn.workflow
```

Inspect node state for a known run id:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow nodes <run-id>
```

Cancel a running workflow by run id:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe workflow cancel <run-id>
```

## Configure LLM serving

CodePilot reads non-secret runtime settings from the rDSN plugin config file. After a build, the copied file is usually:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\config.ini
```

The most important sections are:

| Section/key | Meaning |
| --- | --- |
| `[rasn.model] provider` | `simulator`, `copilot`, `ollama`, `llamacpp`, `lmstudio`, or any custom OpenAI-compatible provider name. `[rasn.llm]` remains a compatibility alias. |
| `[rasn.model] endpoint/model` | Shared HTTP endpoint and model defaults for the selected provider. |
| `[rasn.model] copilot_endpoint`, `ollama_model`, `llama_cpp_endpoint`, `lmstudio_model` | Provider-specific overrides. |
| `[rasn.model] token_ref` | Preferred credential handle. Supports `env:NAME[,NAME2]`, `file:C:\path\token`, and `cmd:<command>` without exposing token values in descriptors or traces. |
| `[rasn.model] token_env` | One environment variable or comma-separated fallback list containing bearer tokens. |
| `[rasn.model] token_command` | Optional command that prints a token at runtime. Used only if token env vars are unset. Treat this as trusted local configuration. |
| `[rasn.model] connect_timeout_sec/request_timeout_sec` | Curl connect and request bounds for network providers. |
| `[rasn.service] host`, `<name>_host`, `<name>_port`, `<name>_uri` | rDSN service graph endpoints. `<name>_uri` takes precedence and supports resolver-backed values such as `dsn://cluster/rasn.coordinator`; otherwise rASN uses host/port. |
| `[rasn.coordinator] max_retry_budget` | Caps per-request `retry_budget` for retryable model-agent dispatch. Tool capabilities are never retried by the coordinator. |
| `[rasn.workflow] execution_lease_ms` | Time-to-live for durable workflow execution owner leases. Active duplicate starts are rejected until the owner finishes or the lease becomes stale. |
| `[rasn.workflow] execution_lease_renew_ms` | Lease renewal interval for active workflow runs. `0` derives a safe interval from the lease TTL. |
| `[rasn.registry] dynamic_registration/heartbeat_ms/lease_ms/sweep_interval_ms/registration_timeout_ms` | Enables best-effort RPC registration of built-in agents plus rDSN timer-driven heartbeats and lease cleanup. `lease_ms = 0` disables TTL filtering; `sweep_interval_ms = 0` disables active cleanup. |
| `[rasn.state] checkpoint_dir/checkpoint_file/journal_file` | Durable state checkpoint and append-only journal paths. State writes also support create-only and expected-sequence conditions for leases and compare-and-swap style ownership. |
| `[rasn.state.nfs] enabled/remote_host/remote_port/remote_checkpoint_dir` | Optional rDSN NFS import source used before state recovery when no local checkpoint or journal exists. Enable `[core] start_nfs` on the importing process and run `dsn.tools.nfs` on the source process. |
| `[rasn.state.replica] enabled/directory/recover` | Optional local mirror for state checkpoints and journals. When enabled, writes fail explicitly if the mirror cannot be updated, and recovery can seed missing primary state from the replica directory. |
| `[rasn.runtime] trace_file` | JSONL file for runtime traces. |
| `[rasn.runtime] temp_dir` | Optional temporary directory for request bodies and curl config files. Empty or `.` uses the OS temp directory under `rasn-provider`. |
| `[rasn.rpc] timeout_ms` | Default timeout for rASN RPC client calls. |
| `[rasn.agent.<id>] agent_id/role/app_name/host/port/endpoint_uri/version/health/capabilities/side_effect_class` | Optional static registry descriptors loaded by `rasn.registry` on startup. `uri` is accepted as an alias for `endpoint_uri`. |
| `[rasn.policy] allow_shell` | Set to `true` to enable the `shell` local tool. |
| `[rasn.policy] allow_write` | Set to `true` to enable `write` and `replace` local tools. |
| `[rasn.policy] require_shell_approval` | Require a `human_approved:shell` policy label before an enabled shell tool can run. CodePilot prompts interactively or accepts `tool --yes`. |
| `[rasn.policy] require_write_approval` | Require a `human_approved:write` policy label before enabled write/replace tools can run. CodePilot prompts interactively or accepts `tool --yes`. |
| `[rasn.policy] shell_allowed_commands` | Optional comma-separated executable allowlist for the `shell` tool. When set, shell metacharacters are denied and the first command token must match the allowlist. |
| `[rasn.policy] shell_working_directory` | Optional working directory wrapper for shell commands. Empty falls back to `workspace_root` when it is set. |
| `[rasn.policy] shell_executor` | `local` by default. Set to `container` to run shell commands through `shell_container_template` after normal policy and approval checks. |
| `[rasn.policy] shell_container_template` | Host command template for `shell_executor = container`. Supports `{command}` for quoted command, `{raw_command}`, `{workspace}` for quoted workspace path, and `{raw_workspace}`. |
| `[rasn.policy] shell_timeout_ms` | Maximum shell command runtime before the shell process is terminated. On Windows, the process is assigned to a job object so timeout termination also covers child processes. `0` disables the timeout for trusted local debugging. |
| `[rasn.policy] workspace_root` | Optional absolute or relative root for local tool targets. When set, read/write/search/list/replace targets must stay inside this root after rDSN path normalization. Empty preserves unrestricted path compatibility. |
| `[rasn.policy] max_tool_output_bytes` | Maximum tool output kept inline before spilling to an artifact file and `rasn.state` reference. |
| `[rasn.policy] artifact_dir` | Directory used for spilled tool-output artifacts. |
| `[rasn.policy] redaction_enabled` | Enables default secret redaction for runtime traces, model-provider prompts/context, model responses, tool previews, and spilled artifacts. |
| `[rasn.policy] redact_env_names` | Comma-separated environment-variable names whose current values should be redacted as exact secrets. |
| `[rasn.policy] redact_literal_values` | Optional comma-separated literal values to redact for local testing or deployment-specific secrets. |
| `[rasn.policy] redact_min_secret_length` | Minimum length for exact env/literal secret matching. Pattern-based redaction still handles common keys such as `password`, `api_key`, and bearer tokens. |
| `[rasn.codepilot.tools] allow_shell/allow_write/require_shell_approval/require_write_approval/shell_allowed_commands/shell_working_directory/shell_executor/shell_container_template/shell_timeout_ms/workspace_root/redaction_*` | Compatibility aliases for older CodePilot configs. |

Tokens are not written to source files or runtime traces. For curl-based providers, rASN uses a temporary curl config file so the bearer token is not placed on the curl command line; the temporary file is deleted immediately after the request. Runtime redaction also removes configured secret values and common secret-shaped fields before traces, tool artifacts, or provider prompts persist or cross rASN boundaries.
By default those temporary files are created under the OS temp directory rather
than the project tree.

### Ollama

Start Ollama and pull a model:

```bat
ollama pull llama3.1
ollama serve
```

In another terminal:

```ini
[rasn.model]
provider = ollama
model = llama3.1
endpoint = http://localhost:11434/api/chat
```

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe ask "Write a plan for adding tests"
```

### llama.cpp server

Start an OpenAI-compatible llama.cpp server:

```bat
llama-server -m C:\models\model.gguf --host 127.0.0.1 --port 8080
```

Run the CLI:

```ini
[rasn.model]
provider = llamacpp
model = local-model
endpoint = http://localhost:8080/v1/chat/completions
```

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe ask "Review this design"
```

### LM Studio

In LM Studio, enable the local server. The default endpoint is usually:

```text
http://localhost:1234/v1/chat/completions
```

Run the CLI:

```ini
[rasn.model]
provider = lmstudio
model = local-model
endpoint = http://localhost:1234/v1/chat/completions
```

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe plan "Build a reliable coding-agent workflow"
```

### GitHub Copilot-compatible endpoint

Configure a Copilot-compatible chat-completions endpoint and token:

```ini
[rasn.model]
provider = copilot
endpoint = https://api.githubcopilot.com/chat/completions
model = gpt-4o-copilot
token_ref = env:RASN_COPILOT_TOKEN,GITHUB_COPILOT_TOKEN,COPILOT_TOKEN,GH_TOKEN
```

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
set RASN_COPILOT_TOKEN=<token>
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe ask "Summarize the rASN runtime"
```

If the token is stored in a different variable:

```bat
set MY_TOKEN=<token>
```

```ini
[rasn.model]
token_env = MY_TOKEN
```

`token_env` is kept as a compatibility alias. New integrations should prefer
`token_ref`, which can reference an environment variable list, a token file, or
a command without storing the token value in config, traces, or provider
descriptors:

```bat
rem Keep the token out of config files and source files.
```

```ini
[rasn.model]
token_ref = file:C:\Users\me\.config\rasn\copilot.token
# or:
token_ref = cmd:gh auth token
```

Whether that token is accepted depends on the endpoint you configure. Some Copilot endpoints require a Copilot-specific token rather than a generic GitHub token.

### Generic OpenAI-compatible API

Use any provider name other than the built-ins with an OpenAI-compatible `/v1/chat/completions` endpoint:

```ini
[rasn.model]
provider = openai-compatible
endpoint = http://localhost:8000/v1/chat/completions
model = my-model
token_ref = env:RASN_API_KEY
```

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
set RASN_API_KEY=<token-if-required>
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe ask "Generate a coding task breakdown"
```

## CLI command reference

```text
ask <prompt>             send a coding prompt
stream <prompt>          stream model-response chunks with trace events
agent <prompt>           run an agent loop that can request local tools
plan <goal>              request an implementation plan
eval [suite]             run CodePilot eval tasks
eval external <template> [suite] compare an external CLI command template
workflow <file>          execute a declarative task graph
context <file>           attach a source file to future prompts
tools                    list local tools
tool <name> <args>       run a local tool directly
selftest [checkpoint]    run model/tool/state/workflow/observability checks
state <cmd> [args]       use rASN state/checkpoint service
observe events [kind]    query structured runtime events
observe timeline [trace] show ordered trace events
schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py] print/export schemas and generated RPC clients
observe diagnose [trace] summarize failures and replay issues
observe failures         query classified failure records
observe replay <file>    load replay choices through rasn.observability
observe snapshot         summarize observability state
skills                   list CodePilot skills
skill <name> [task]      show or apply a skill prompt
topology                 show the rDSN service graph
provider [name]          show or switch provider
trace [file]             show or set JSONL runtime trace file
replay <trace-jsonl>     replay captured nondeterministic choices
simulate <prompt>        force the random local simulator
interactive              start REPL mode
```

Provider, trace, and replay can be chained with another command:

```bat
codepilot.exe provider ollama ask "Explain the code"
codepilot.exe trace C:\temp\trace.jsonl plan "Add a feature"
codepilot.exe replay C:\temp\trace.jsonl ask "Repeat this deterministically"
codepilot.exe selftest
```

`stream <prompt>` uses the same model route as `ask`, but emits response chunks
as they are produced by the provider surface. Each chunk is redacted and recorded
as an `llm.response.chunk` event before it is printed. Providers without native
streaming still use the same callback API by chunking the final response, so
applications can build streaming UIs without depending on provider-specific
protocols.

The `eval` command provides a runnable evaluation harness instead of leaving
comparison as a report-only plan. With no suite file it runs a small built-in
simulator-friendly task set and reports per-task success, latency, and output
size. A suite file is a UTF-8 text file with one prompt per non-empty,
non-comment line. To compare another coding CLI, pass a quoted command template
containing `{prompt}`:

```bat
codepilot.exe eval
codepilot.exe eval C:\benchmarks\coding-tasks.txt
codepilot.exe eval external "opencode run --prompt {prompt}" C:\benchmarks\coding-tasks.txt
```

The `schema` command exports the runtime contract for platform integrators.
`schema text` is readable documentation, `schema json` is a machine-readable
manifest, `schema idl` is a compact language-neutral contract, `schema cpp`
emits a self-contained C++ header under `dsn::rasn::generated`,
`schema clients-cpp` emits executable C++ rDSN `clientlet` wrappers for the
agent, registry, state, workflow, model, and observability RPC surfaces,
`schema ts` emits TypeScript interfaces, `schema clients-ts` emits TypeScript
transport-backed client classes, `schema py` emits Python dataclasses, and
`schema clients-py` emits Python transport-backed client classes. Generated
contract stubs cover core agent, registry, tool, policy, observability, state,
and workflow messages.

```bat
codepilot.exe schema cpp > rasn_generated_schema.h
codepilot.exe schema clients-cpp > rasn_generated_rpc_clients.h
codepilot.exe schema ts > rasn-generated-schema.ts
codepilot.exe schema clients-ts > rasn-generated-rpc-clients.ts
codepilot.exe schema py > rasn_generated_schema.py
codepilot.exe schema clients-py > rasn_generated_rpc_clients.py
```

## Unit tests

The rDSN build also produces a standalone `rasn.unit_tests` binary for focused
rASN and CodePilot unit coverage. The target initializes a minimal rDSN runtime
before invoking Google Test, then runs through the existing per-binary test
scripts used by `run.cmd test` and `run.sh test`. Coverage includes request
validation, policy classification, bounded artifact indexing, observability
snapshot indexing, parser helpers, workflow cancellation/budget propagation,
state checkpoint/recovery, external-effect ledger events, shell wrapper
formatting, and CodePilot local read/search tools.

Windows:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\rasn.unit_tests\test.cmd C:\Users\haoxlin\source\repos\rdsn\rDSN Debug C:\Users\haoxlin\source\repos\rdsn\rb-rasn
```

Linux:

```bash
cd /home/lin/src/rdsn/rDSN/builder/bin/rasn.unit_tests
./test.sh
```

## Project testing examples

Boost source tree, which is not a git repository:

```bat
codepilot.exe tool list C:\Users\haoxlin\source\repos\rdsn\boost_1_84_0
codepilot.exe tool search C:\Users\haoxlin\source\repos\rdsn\boost_1_84_0 "boost/version.hpp"
codepilot.exe agent "Inspect C:\Users\haoxlin\source\repos\rdsn\boost_1_84_0 and explain how to find Boost version information"
```

llama.cpp:

```bat
codepilot.exe tool list C:\Users\haoxlin\source\repos\rdsn\llama.cpp
codepilot.exe tool search C:\Users\haoxlin\source\repos\rdsn\llama.cpp "llama_server"
codepilot.exe skill feature-plan "Plan a small llama.cpp documentation change"
```

rDSN.dist.service:

```bat
codepilot.exe tool list C:\Users\haoxlin\source\repos\rdsn\rDSN.dist.service
codepilot.exe tool search C:\Users\haoxlin\source\repos\rdsn\rDSN.dist.service "replica"
codepilot.exe skill debug-build "Diagnose a build failure in rDSN.dist.service"
```

## rDSN service mode

The executable can also run as an rDSN `service_app` using `config.ini`:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe --dsn
```

This registers and runs the app type:

```text
rasn.registry
rasn.coordinator
rasn.llm.agent
rasn.tool.agent
rasn.state
rasn.workflow
rasn.observability
rasn.codepilot
```

`rasn.llm.agent`, `rasn.tool.agent`, `rasn.coordinator`, `rasn.workflow`,
`rasn.observability`, and `rasn.codepilot` all retain the shared
`rasn_service_graph` while active; service shutdown releases those references in
any order and only the final release stops the graph.

Use `topology` in direct mode or `/topology` in service mode to inspect the
registered graph, registry snapshot, state summary, observability counters, and
the latest state-indexed observability snapshot:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe topology
```

For focused operator inspection, use:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe registry list
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe registry query model.complete
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe agentctl describe model
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe agentctl cancel model <request-id>
```

The default service ports are configured in `[rasn.service]`:

```ini
host = localhost
registry_port = 27100
coordinator_port = 27101
llm_agent_port = 27102
tool_agent_port = 27103
state_port = 27104
workflow_port = 27105
observability_port = 27106
```

Each service can also use a specific host or URI:

```ini
coordinator_host = 10.0.0.20
llm_agent_uri = dsn://rasn/llm.agent
tool_agent_uri = dsn://rasn/tool.agent
```

When RPC clients are enabled, built-in model, tool, and coordinator agents also
register themselves with `rasn.registry` through typed registry RPCs. A rDSN
timer task sends periodic heartbeats so `healthy_only` registry queries and
coordinator routing can exclude lease-tracked agents whose heartbeat is older
than `[rasn.registry] lease_ms`. The registry service also owns a separate rDSN
timer that periodically removes expired lease-tracked agents from the registry,
while static `[rasn.agent.*]` descriptors are retained.

`RPC_RASN_WORKFLOW_START` uses a dedicated `THREAD_POOL_RASN_WORKFLOW` pool so
service-mode workflow execution does not block the default RPC worker pool while
it performs nested coordinator/model/tool calls.

Before executing service-mode CodePilot commands, the gateway now waits for the
full service surface to answer non-mutating readiness probes: state, registry,
coordinator, model agent health, tool agent, workflow validation, and
observability. Startup failures name the specific service boundary that did not
become ready.

For a repeatable service-mode RPC smoke, copy or point the built executable at
`src\plugins\rasn\examples\service-rpc-smoke.ini`. The `rasn.codepilot` app runs
`selftest` through the service graph, then the rDSN process remains alive until
you stop it:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe --dsn src\plugins\rasn\examples\service-rpc-smoke.ini
```

The direct one-shot CLI mode is recommended for local prototyping and provider testing.

### Current product limitations

The current implementation is a usable prototype platform, but these product
hardening gaps remain:

| Area | Current capability | Remaining limitation |
| --- | --- | --- |
| State availability | Checkpoints, append-only journals, conditional writes, workflow leases, optional local replica mirroring, and optional rDSN NFS import. | No quorum-replicated or externally managed HA state backend yet. |
| Tool isolation | Default-deny side effects, workspace scoping, approvals, command allowlists, timeout/job containment, and a configurable container command wrapper. | No hardened container orchestrator with image, mount, network, and lifecycle policy. |
| Replay fidelity | Replay for model responses, tool results, workflow scheduling, filesystem snapshots, and an `external.effect` ledger for side-effect intents. | No full virtualization of arbitrary external services, clocks, network state, or process environments. |
| Deployment validation | Inline mode, typed service-mode RPC, URI/host endpoint configuration, registry heartbeats, and active lease cleanup. | Multi-process and cluster deployment tests are still limited. |
| Credentials | `token_ref` handles for environment variables, files, and commands, plus deterministic redaction. | No vault-backed or OS-backed credential provider integration. |
| SDK packaging | Generated C++/TypeScript/Python contracts and RPC-client source. | Packaged SDKs and concrete TypeScript/Python transports are not shipped. |
| Evaluation evidence | Unit tests, self-tests, service smokes, schema smokes, report build, and a small eval harness. | Large benchmarks and user studies for debugging effectiveness remain future work. |

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `dsn.core.dll` or other runtime DLL cannot be found | Ensure `C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug` is first on `PATH` before running `codepilot.exe`. |
| Service-mode RPC calls fail | Confirm no other process owns ports `27100`-`27106`, then rerun `codepilot.exe --dsn`. |
| Provider request fails | Run `codepilot.exe providers` and verify `[rasn.model]` or compatibility `[rasn.llm]` has the expected `provider`, `endpoint`, and `model`. |
| Copilot/OpenAI-compatible authentication fails | Store credentials in `token_env` or trusted `token_command`; do not place token values in `config.ini` or trace files. |
| Write or shell tools are denied | This is the safe default. Set `[rasn.policy] allow_write = true` or `allow_shell = true` only for trusted local tests. If approval is required, confirm the prompt or use `tool --yes ...` for direct scripted invocations. |
| Replay output does not match expectations | Use `observe events nondeterminism`, `observe timeline`, and `observe diagnose` to inspect replay loads and replay misses. |
| State recovery misses recent writes | Check `[rasn.state] journal_file`; recovery replays the journal after loading the compact checkpoint. |
