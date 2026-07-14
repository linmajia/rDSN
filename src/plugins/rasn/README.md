# Robust Agent System Nucleus (rASN)

rASN is a prototype framework for building robust AI agent systems on top of rDSN. It treats agent execution as a task-centric distributed runtime problem: every agent step is represented as an explicit task, runtime nondeterminism is captured as data, and workflow execution is exposed as an instrumented graph that can be traced, diagnosed, and replayed.

The first sample application is **rASN CodePilot** (short name: **CodePilot**), a coding CLI inspired by OpenCode, Codex CLI, and GitHub Copilot CLI. It can run entirely offline with a random simulator, or connect to OpenAI-compatible APIs such as GitHub Copilot-style endpoints, Ollama, llama.cpp, and LM Studio. The second sample application is **SREPilot**, an SRE / incident-response CLI that reuses the same model gateway, state service, observability surface, and resilience controls for production-operations workflows.

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
| Runtime metrics | `metrics.*` | Exports cumulative event counters and task/model/tool latency percentiles through rDSN `perf_counter` counters (section `rasn`) and renders them as text, Prometheus, or JSON; exposed via `observe metrics` and the rDSN `command_manager` command `rasn.metrics`. |
| Resilience | `circuit_breaker.*`, `admission_gate.*`, `rate_limiter.*`, `model_cost.*` | Guards model providers across the three classic dependency-protection dimensions: a consecutive-failure circuit breaker (closed/open/half-open), an admission gate (concurrency bulkhead plus `exp_delay`-based backpressure), and a token-bucket rate limiter. The model gateway adds a fourth governor — a token/cost budget that reuses the token bucket to weigh each request by its estimated token cost, bounding tokens-per-minute and metered spend rather than just request count. The same engines guard coordinator-to-agent RPC dispatch per remote agent, while admission/rate engines also guard tool execution per tool name, so shell/filesystem/future remote tools cannot fan out unbounded. A process-wide overload budget reuses the admission/rate engines as singletons at the coordinator `invoke` chokepoint to bound total in-flight work and throughput across all dependencies (opt-in; passthrough by default). Clocks/curves come from rDSN (`dsn_now_ms`, `exp_delay`), state is exported through `perf_counter` counters and the `rasn.resilience` command, and tuning lives under `[rasn.model]`, `[rasn.tool]`, `[rasn.remote_agent]`, and `[rasn.overload]`. |
| LLM provider layer | `llm_provider.*` | Provides a common `llm_provider` interface and adapters for simulator, Copilot-compatible, Ollama, llama.cpp, LM Studio, and generic OpenAI-compatible HTTP APIs. |
| Tool provider layer | `agent_tools.*` | Defines the generic rASN tool-provider contract and registration factory used by the tool-agent service. |
| Workflow graph | `workflow.*` | Parses a declarative task graph, validates dependencies, topologically orders nodes, and executes them through the selected provider. |
| CodePilot tools | `apps/codepilot/local_tools.*` | Implements and registers the CodePilot application tool provider: project inspection tools, read/search, and opt-in shell/write execution. |
| CodePilot skills | `apps/codepilot/skills.*` | Provides reusable coding-agent skill prompts for rDSN plugin work, code review, build debugging, feature planning, and documentation. |
| CodePilot app | `apps/codepilot/codepilot_app.*`, `apps/codepilot/main.cpp` | Exposes one-shot and interactive coding-agent commands as an adapter that builds generic `agent_request` messages and routes them through the rASN coordinator. |
| SREPilot app | `apps/srepilot/srepilot_app.*`, `apps/srepilot/main.cpp` | Exposes incident diagnosis, runbook generation, status, observability, and self-test commands as a second application adapter over the same rASN nucleus. |

### rDSN micro-service model

rASN models an agent system like a distributed service system. The current prototype registers these rDSN app roles:

```text
rasn.registry       stores agent descriptors/capabilities (local map or opt-in HA authority)
rasn.coordinator    orchestrates task graph execution and routes service calls
rasn.llm.agent      isolates nondeterministic LLM completion behind the model gateway
rasn.tool.agent     isolates local tool side effects behind explicit opt-in policies
rasn.state          stores namespaced state and checkpoints
rasn.workflow       validates, compiles, and executes declarative agent graphs
rasn.observability  queries events, failures, snapshots, and replay loading
rasn.codepilot      exposes rASN CodePilot as a gateway service
rasn.srepilot       exposes rASN SREPilot as a gateway service
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

Every app command loads the app's own `config.ini`. Runtime placement is selected
only by `[rasn.runtime] rasn_runtime_provider`: `local` uses the same module
implementations in-process, while `distributed`/`hybrid` starts a lightweight rDSN
client node and communicates with a remote runtime. The command line does not
select placement.

Configuration is split into three files, and an **app never carries runtime
service deployment**. `config.ini` is the thin app config (rDSN bootstrap, app
gateway, placement, and optional model/remote-runtime endpoint). A local app may
optionally include `config.rasn.defaults.ini`, which contains only shared module
tuning. `config.rasn.ini` is the self-contained, services-only runtime-host config;
it includes `config.rasn.defaults.ini`, never an app config. CMake binplaces the
host pair and optional `config.rasn.state.ini` quorum-state profile beside each
executable; copy the required files with the binary on a dedicated node and run
`codepilot serve`. `--dsn` remains
only as a deprecated alias of `serve`. The rASN plugin uses stock rDSN inline,
last-write-wins `@include` semantics and does not modify the parser. See
`docs/DISTRIBUTED_RUNTIME.md` §6.1.

Application CLIs share the same path-startup convention through the reusable
`cli_support` helper: a single existing directory argument switches the process
workspace, loads a bounded source-file index plus selected file excerpts as
startup context, and enters interactive mode. The directory snapshot skips
generated/build output and obvious secret-bearing paths such as `.env*`,
credential stores, keys, and `secrets.{json,yml,yaml}` /
`config.{json,yml,yaml}` files; if enumeration fails,
the CLI still opens the workspace and reports that source context is unavailable.
A single existing file argument uses the file parent as the workspace and loads
the file as startup context. In
interactive mode, slash-prefixed commands (`/help`, `/exit`, `/diagnose`,
`/ask`, etc.) are commands; `/help` renders command items with the leading slash,
and plain text without a slash is handled by the application's default prompt
action.

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

If `[rasn.runtime] trace_file` is configured or the CLI `trace <file>` command is used, events are appended as JSON lines. Simulator randomness is recorded as a `nondeterminism` event and can be reused later with `replay <trace-jsonl>` or `observe replay <trace-jsonl>`. Model and tool results are also replay-aware: when a replay trace contains recorded `llm.response` events for the provider, `rasn.llm.agent` returns those responses in provider order instead of contacting the provider; when it contains a matching `tool.ok` or `tool.error` event, `rasn.tool.agent` returns that recorded result instead of invoking the provider again. Workflow execution records `workflow.node.start`/`workflow.node.finish` transitions, and replay mode checks recorded node-start order before executing a node. If replay mode is active and a side-effecting tool has no recorded result, the runtime returns an explicit replay error rather than re-running a write or shell command. Trace identifiers and the simulator's pseudo-random choices are drawn from rDSN's pluggable environment provider (`dsn_random64`, the same primitive rDSN uses to mint RPC trace IDs), so emulator and replay tooling can seed or virtualize time and randomness instead of relying on private wall-clock-seeded generators.

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
codepilot.exe observe metrics
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

### Runtime metrics

The nucleus also exports quantitative runtime metrics through rDSN's own
`perf_counter` subsystem (section `rasn`), reusing the same counter
infrastructure that rDSN ships for its core services rather than inventing a
parallel metrics path. `nucleus_runtime::record_event` is the single choke point
that increments cumulative counters per event kind (Prometheus `_total`
convention, e.g. `rasn_tasks_begin_total`, `rasn_llm_requests_total`,
`rasn_tool_error_total`, `rasn_failures_total`), with a per-failure-class counter
created lazily for each distinct classification. Task, model, and tool wall-clock
latencies are recorded into `COUNTER_TYPE_NUMBER_PERCENTILES` counters
(`rasn_task_latency_ms`, `rasn_llm_latency_ms`, `rasn_tool_latency_ms`) whose
p50/p95/p99/p999 are computed by rDSN's counter timers.

Metrics are exposed two ways:

```bat
codepilot.exe observe metrics             :: aligned text table
codepilot.exe observe metrics prometheus  :: Prometheus text exposition format
codepilot.exe observe metrics json        :: compact JSON document
```

In service mode the same snapshot is also registered with rDSN's
`command_manager`, so a running deployment answers `rasn.metrics [text|prometheus|json]`
over the rDSN local/remote CLI alongside the built-in rDSN commands. The
Prometheus output can be scraped directly or relayed to any metrics backend.
Metric collection is controlled by `[rasn.metrics] enabled` (default `true`);
when disabled, every counter update becomes a no-op. Counter updates are
null/thread safe and degrade to no-ops when the process has no rDSN service node,
so the same code path is safe in inline, CLI, and service modes.

### Resilience: model circuit breaker

Counters make a degrading endpoint visible; a circuit breaker makes the runtime
react to it. The model gateway wraps each provider in a per-provider circuit
breaker so a failing or hanging endpoint fails fast instead of driving the
provider's retry loop on every call. After `circuit_breaker_failure_threshold`
consecutive failures the breaker *opens* and short-circuits requests for
`circuit_breaker_open_ms`; it then admits a single half-open probe, closing on
success or reopening on failure. Short-circuited calls return a normal failed
`llm_response` carrying the breaker state.

The breaker engine takes its clock from `::dsn_now_ms()` (routed through rDSN's
pluggable environment provider), so it is deterministic under replay; replayed
runs and in-process providers (the simulator, the workflow service-graph bridge,
and test fakes) bypass it entirely, leaving existing behavior unchanged. Network
providers are guarded even when their endpoint is loopback (Ollama, llama.cpp, LM
Studio), since they still issue HTTP that can hang. Breaker activity flows through
the same `record_event` choke point as every other metric via two counters
(`rasn_model_breaker_open_total`, `rasn_model_breaker_short_circuit_total`), and a
running deployment can dump live per-provider state through the rDSN command
`rasn.resilience`. Tuning lives under `[rasn.model] circuit_breaker_*` and
defaults to enabled.

### Resilience: model admission control

A circuit breaker stops calling a *broken* endpoint; admission control keeps the
runtime from overwhelming a *healthy* one. A model endpoint that is up but slow has
finite useful concurrency — beyond it, adding in-flight requests only deepens
queues and inflates tail latency. The model gateway therefore fronts each provider
with an admission gate that combines a concurrency *bulkhead* (a hard cap of
`max_concurrent_requests` simultaneous in-flight calls per provider; excess load is
rejected immediately with a failed `llm_response` instead of queueing unbounded)
with graceful *backpressure* (once in-flight load passes `soft_concurrent_requests`
the gate applies a short, growing pre-call delay, bounded by `max_backpressure_ms`,
to smooth bursts).

The backpressure curve is not hand-rolled: it reuses rDSN's own `exp_delay`
admission-control primitive — the same staged-delay mechanism rDSN uses to throttle
its task queues. The gate is evaluated *before* the breaker (so a rejected request
never perturbs breaker state) and *after* the replay check (so replayed runs stay
deterministic), and the same `in_process()` predicate that exempts the simulator
and test fakes from the breaker exempts them here too. With the generous defaults,
sequential CodePilot CLI use (in-flight ≈ 1) never delays or rejects; the gate only
engages under the concurrent fan-out of service mode. Rejections and backpressure
delays are exported as two counters
(`rasn_model_admission_rejected_total`, `rasn_model_admission_delayed_total`), and
live per-provider in-flight depth shows up in `rasn.resilience`. Tuning lives under
`[rasn.model] admission_enabled`, `max_concurrent_requests`,
`soft_concurrent_requests`, and `max_backpressure_ms`.

### Resilience: model rate limiter

The breaker reacts to a *broken* endpoint and admission control protects a
*healthy* one from concurrency overload; a rate limiter governs the third
dimension — *throughput*. Hosted model APIs publish requests-per-minute quotas,
and exceeding them returns HTTP 429s that burn the retry budget (and, on metered
endpoints, real money). A concurrency bulkhead does not help here: one caller
making rapid sequential requests stays at in-flight ≈ 1 yet can still blow through
a per-minute quota. rASN therefore fronts each provider with a per-provider
**token-bucket rate limiter**. The bucket starts full (absorbing an initial
burst) and refills at the configured sustained rate; a request that is slightly
ahead of the rate is *paced* with a short delay (reserving the next token), and
one whose projected wait would exceed `rate_limit_max_wait_ms` is rejected fast
with a normal `llm_response` instead of blocking a worker.

Like the breaker, the engine takes its clock from `::dsn_now_ms()` (routed through
rDSN's pluggable environment provider), so token refill is deterministic under
replay and unit-testable without a live node. The limiter runs beside the breaker
and admission gate on the same gateway: admission and the rate limiter reserve
first, the breaker is consulted last (so a rate-rejected request never strands a
half-open probe), the pacing and backpressure delays are coalesced into a single
wait, and the whole guard sits after the replay check so replayed runs stay
deterministic. The same `in_process()` predicate that exempts the simulator and
test fakes applies here too. It is **disabled by default** (`requests_per_min = 0`
= unlimited), so existing CLI and test behavior is unchanged; set a rate when
pointing rASN at a metered endpoint. Rejections and pacing delays are exported as
two counters (`rasn_model_rate_limited_total`, `rasn_model_rate_delayed_total`),
and live per-provider rate/burst/available-tokens show up in `rasn.resilience`.
Tuning lives under `[rasn.model] rate_limit_enabled`,
`rate_limit_requests_per_min`, `rate_limit_burst`, and `rate_limit_max_wait_ms`.

### Resilience: model cost/token budget

The rate limiter above governs request *count*, but a hosted model API also meters
*tokens*-per-minute — and on metered endpoints the token dimension is what drives
spend. A request counter cannot bound token throughput or cost, because one
large-context prompt can cost many times a small one (ten small requests can be
cheaper than a single 100K-token completion). rASN therefore adds a fourth
model-gateway governor: a **token/cost budget** that weighs each request by its
estimated token cost.

It does not introduce a second engine — it *reuses the token bucket*, which meters
whatever weight it is handed. Instead of the constant `1` a request counter spends,
the cost budget spends an estimated **token** charge:
`max(1, ceil(prompt_chars / chars_per_token) * completion_percent / 100)` over the
system prompt, user prompt, and context. The estimate depends on prompt size
**only**, never the provider response, so charging a request stays deterministic and
replay-safe. `cost_tokens_per_min` becomes the bucket's refill rate and
`cost_burst_tokens` its capacity, so a request the budget cannot fund within
`cost_max_wait_ms` is paced or fast-failed exactly like the rate limiter — size
`cost_burst_tokens` at least as large as the biggest single-request estimate you
intend to admit.

The budget is hardened for operator-supplied configuration extremes. Oversized
estimated token counts are saturated before they are written to events or error
messages, and the shared token bucket rejects unrepresentably large projected
waits before any floating-point-to-integer conversion. This keeps malformed but
valid config from turning an intended fast-fail into undefined behavior.

The budget slots into the gateway chain right after the request-count rate limiter
and before the breaker, so a cost-rejected request never strands a half-open probe;
its pacing delay joins the single coalesced wait. Refunds compose: a cost rejection
refunds the rate token already taken, and a breaker short-circuit after both were
taken refunds both, so a downstream rejection never drains the request quota *or*
the token budget. The same `in_process()` exemption applies (the simulator has no
token cost), and it is **disabled by default** (`cost_tokens_per_min = 0` =
unlimited) so CLI and test behavior is unchanged. Rejections and pacing delays are
exported as `rasn_model_cost_limited_total` and `rasn_model_cost_delayed_total`, and
live per-provider tokens/minute, burst tokens, and available budget show up in
`rasn.resilience`. Tuning lives under `[rasn.model] cost_budget_enabled`,
`cost_tokens_per_min`, `cost_burst_tokens`, `cost_max_wait_ms`,
`cost_chars_per_token`, and `cost_completion_percent`.

### Resilience: tool admission and rate controls

Tools are also outbound dependencies: a coding agent can fan out many `read`,
`search`, `shell`, or future remote-tool calls even when the model path is healthy.
rASN now applies the same reusable resilience engines to the tool gateway, keyed by
tool name. After replay lookup and policy approval, `rasn.tool.agent` reserves a
per-tool admission slot, optionally takes a per-tool rate token, coalesces any
admission backpressure and rate pacing into one delay, and then invokes the tool
provider. Replayed calls and policy-denied side effects never consume capacity, and
the `admission_slot` RAII guard releases the concurrency slot on every exit path.

The tool provider pointer is held only long enough to take a shared reference, so
the old provider-registration lock no longer serializes all tool execution.
Instead, concurrency is explicit and observable through `[rasn.tool]`
`max_concurrent_requests`, `soft_concurrent_requests`, `max_backpressure_ms`,
`rate_limit_requests_per_min`, `rate_limit_burst`, and
`rate_limit_max_wait_ms`. The rate limiter is disabled by default
(`rate_limit_requests_per_min = 0`) and the admission defaults are generous enough
for normal sequential CLI use. Rejections and delays are exported as
`rasn_tool_admission_rejected_total`, `rasn_tool_admission_delayed_total`,
`rasn_tool_rate_limited_total`, and `rasn_tool_rate_delayed_total`; live state is
shown by `rasn.resilience` and `observe resilience`.

### Resilience: remote-agent dispatch guards

In service mode, the coordinator is itself an outbound dependency client: after a
registry lookup it sends `RPC_RASN_AGENT_INVOKE` to a selected remote model, tool,
or custom agent. A slow or unhealthy remote agent can otherwise consume retry
budget, worker threads, and queue capacity even though the model and tool gateways
are individually guarded. The coordinator now applies the same three resilience
engines to each remote agent id before invoking it: a circuit breaker for
retryable dependency failures, an admission gate for concurrent fan-out, and a
token-bucket rate limiter for request quotas.

The remote-agent breaker is intentionally scoped to retryable dependency failures
only. Transport errors and retryable remote responses count against it; deterministic
application outcomes such as policy denials, validation failures, and tool errors
do not poison the remote service. Admission and rate rejections happen before the
breaker probe, and any admission/rate pacing is coalesced into a single delay just
like the model gateway. The defaults are generous (`64/32/200ms`, rate unlimited),
so normal CLI and smoke-test behavior is unchanged while service deployments get
visible guard state in `rasn.resilience` and `observe resilience`.

### Resilience: process-wide overload budget

The per-dependency gateways above each protect one outbound boundary, but a host
has finite *shared* capacity — threads, memory, sockets. When many dependencies
are individually healthy yet all busy at once, the sum of their generous
per-dependency caps can still exhaust the process. rASN adds a single process-wide
overload budget at the coordinator's `invoke` chokepoint: one global concurrency
bulkhead plus one global request-rate ceiling (the same `admission_gate` and
`rate_limiter` engines, here as singletons) that bound *total* in-flight work and
throughput across every dependency, in both inline and RPC modes.

The budget is applied outermost — after request admission/lifecycle checks (so a
malformed or cancelled request consumes no budget) and before routing (so the
process sheds load uniformly) — and rejections are structured, non-retryable
`overload` responses so a caller with alternate nodes can route away. Unlike the
per-dependency caps, the concurrency defaults are `0` (passthrough), so zero-config
behavior is unchanged and operators opt in by sizing `[rasn.overload]` to their
host. Live budget state (in-flight, caps, cached tokens) heads the
`rasn.resilience` and `observe resilience` report, and pacing/rejections export as
`rasn_overload_admission_rejected_total`, `rasn_overload_admission_delayed_total`,
`rasn_overload_rate_limited_total`, and `rasn_overload_rate_delayed_total`.

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

The prompt is free text. Words are scanned until the first recognized option
keyword (`after`, `capability`, `policy`, `cost`, `state`, ...), so a prompt that
contains one of those words is either quoted as a single argument
(`task a ask "review the cost report"`) or separated from options with an explicit
`--` delimiter (`task a ask review the cost report -- policy read_only cost 3`).
Everything before `--` is taken verbatim as the prompt; everything after it is
options.

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

The SREPilot executable is:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\srepilot\Debug\srepilot.exe
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
trace_file = C:\Users\haoxlin\source\repos\rdsn\rb-rasn\rasn\traces\rasn.trace.jsonl
```

Replay the captured nondeterministic simulator choice and any matching recorded
model/tool results:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe replay C:\Users\haoxlin\source\repos\rdsn\rb-rasn\rasn\traces\rasn.trace.jsonl ask "Explain how to add a new rDSN plugin"
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
/context src\plugins\rasn\apps\codepilot\codepilot_app.cpp
/plan Add a new command to CodePilot
/ask Summarize the current rASN design
/exit
```

Plain text without a slash is treated as an `ask` prompt.

## Run SREPilot

SREPilot is a second application adapter that exercises rASN outside coding tasks. It uses the same simulator/provider configuration by default, persists incident artifacts under the `srepilot/` state namespace, and exposes rASN observability/resilience views for operations workflows.

```bat
set PATH=C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug;%PATH%
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\srepilot\Debug\srepilot.exe diagnose "checkout latency p95 doubled after the last deployment"
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\srepilot\Debug\srepilot.exe runbook "database connection pool exhaustion"
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\srepilot\Debug\srepilot.exe observe resilience
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\srepilot\Debug\srepilot.exe selftest
```

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

By default, local runtime files are grouped under a single top-level `rasn`
directory in the process working directory:

```text
rasn/
  state/       durable checkpoints, journals, workflow leases, incident records
  artifacts/   spilled tool-output artifacts and payload references
  traces/      JSONL runtime traces when trace output is enabled
```

| Section/key | Meaning |
| --- | --- |
| `[rasn.model] provider` | `simulator`, `copilot`, `ollama`, `llamacpp`, `lmstudio`, or any custom OpenAI-compatible provider name. `[rasn.llm]` remains a compatibility alias. |
| `[rasn.model] endpoint/model` | Shared HTTP endpoint and model defaults for the selected provider. |
| `[rasn.model] copilot_endpoint`, `ollama_model`, `llama_cpp_endpoint`, `lmstudio_model` | Provider-specific overrides. |
| `[rasn.model] token_ref` | Preferred credential handle. Supports `env:NAME[,NAME2]`, `file:C:\path\token`, and `cmd:<command>` without exposing token values in descriptors or traces. |
| `[rasn.model] token_env` | One environment variable or comma-separated fallback list containing bearer tokens. |
| `[rasn.model] token_command` | Optional command that prints a token at runtime. Used only if token env vars are unset. Treat this as trusted local configuration. |
| `[rasn.model] connect_timeout_sec/request_timeout_sec` | Curl connect and request bounds for network providers. |
| `[rasn.model] circuit_breaker_enabled` | Enables the per-provider model circuit breaker (default `true`). When `false`, every model request is admitted regardless of recent failures. |
| `[rasn.model] circuit_breaker_failure_threshold` | Consecutive model-call failures that open the breaker (default `5`). |
| `[rasn.model] circuit_breaker_open_ms` | Cooldown in milliseconds before an open breaker admits a single half-open probe (default `30000`). Inspect live state with the `rasn.resilience` command. |
| `[rasn.model] admission_enabled` | Enables the per-provider model admission gate (default `true`). When `false`, requests are never rejected or delayed by concurrency limits. |
| `[rasn.model] max_concurrent_requests` | Hard cap on concurrent in-flight model requests per provider (default `32`; `0` = unlimited). Excess requests fast-fail. |
| `[rasn.model] soft_concurrent_requests` | In-flight level at which graceful backpressure begins (default `16`; `0` = no backpressure). |
| `[rasn.model] max_backpressure_ms` | Upper bound in milliseconds on the graceful admission backpressure delay (default `200`). Inspect live in-flight depth with the `rasn.resilience` command. |
| `[rasn.model] rate_limit_enabled` | Enables the per-provider model rate limiter (default `true`). Has no effect unless a rate is set, since `rate_limit_requests_per_min = 0` already means unlimited. |
| `[rasn.model] rate_limit_requests_per_min` | Sustained model request rate per provider in requests/minute (default `0` = unlimited / disabled). The token-bucket refill rate. |
| `[rasn.model] rate_limit_burst` | Rate-limiter burst capacity in tokens (default `0` = about one second of the sustained rate, minimum 1). |
| `[rasn.model] rate_limit_max_wait_ms` | Max milliseconds a request may be paced waiting for a token before it is rejected instead (default `1000`; `0` = reject immediately when the bucket is empty). Inspect live rate/burst/tokens with the `rasn.resilience` command. |
| `[rasn.model] cost_budget_enabled` | Enables the per-provider model token/cost budget (default `true`). Has no effect unless `cost_tokens_per_min` is non-zero. Weighs each request by its estimated token cost so it bounds tokens-per-minute and metered spend, not just request count. |
| `[rasn.model] cost_tokens_per_min` | Sustained token budget per provider in estimated tokens/minute (default `0` = unlimited / disabled). The token bucket's refill rate, denominated in tokens. |
| `[rasn.model] cost_burst_tokens` | Cost-budget burst capacity in estimated tokens (default `0` = about one second of the sustained budget, minimum 1). Size this at least as large as the biggest single-request estimate you intend to admit. |
| `[rasn.model] cost_max_wait_ms` | Max milliseconds a request may be paced waiting for token budget before it is rejected instead (default `1000`; `0` = reject immediately when the budget is exhausted). Inspect live tokens/min, burst, and available budget with `rasn.resilience`. |
| `[rasn.model] cost_chars_per_token` | Prompt characters per estimated token (default `0` = 4, the common ~4-chars/token heuristic for byte-pair tokenizers). |
| `[rasn.model] cost_completion_percent` | Completion allowance as a percent of estimated input tokens added to the charge (default `0` = 150 = +50% for the completion; `100` = charge input tokens only). Oversized derived estimates are saturated in diagnostics/events. |
| `[rasn.tool] admission_enabled` | Enables the per-tool admission gate (default `true`). Replayed tool results and policy-denied calls bypass the gate. |
| `[rasn.tool] max_concurrent_requests` | Hard cap on concurrent in-flight tool invocations per tool name (default `16`; `0` = unlimited). Excess invocations fast-fail. |
| `[rasn.tool] soft_concurrent_requests` | Per-tool in-flight level at which graceful backpressure begins (default `8`; `0` = no backpressure). |
| `[rasn.tool] max_backpressure_ms` | Upper bound in milliseconds on graceful tool backpressure (default `100`). Inspect live in-flight depth with `rasn.resilience`. |
| `[rasn.tool] rate_limit_enabled` | Enables the per-tool rate limiter (default `true`). Has no effect unless `rate_limit_requests_per_min` is non-zero. |
| `[rasn.tool] rate_limit_requests_per_min` | Sustained invocation rate per tool name in requests/minute (default `0` = unlimited / disabled). |
| `[rasn.tool] rate_limit_burst` | Per-tool rate-limiter burst capacity in tokens (default `0` = about one second of the sustained rate, minimum 1). |
| `[rasn.tool] rate_limit_max_wait_ms` | Max milliseconds an invocation may be paced waiting for a token before it is rejected (default `1000`; `0` = reject immediately when the bucket is empty). |
| `[rasn.remote_agent] circuit_breaker_enabled` | Enables the per-agent coordinator RPC circuit breaker (default `true`). Only retryable dependency failures count against it. |
| `[rasn.remote_agent] circuit_breaker_failure_threshold` | Consecutive retryable remote-agent failures that open the breaker (default `5`). |
| `[rasn.remote_agent] circuit_breaker_open_ms` | Cooldown in milliseconds before an open remote-agent breaker admits a single half-open probe (default `30000`). |
| `[rasn.remote_agent] admission_enabled` | Enables the per-agent coordinator RPC admission gate (default `true`). |
| `[rasn.remote_agent] max_concurrent_requests` | Hard cap on concurrent in-flight coordinator RPC dispatches per remote agent (default `64`; `0` = unlimited). |
| `[rasn.remote_agent] soft_concurrent_requests` | Per-agent in-flight level at which graceful backpressure begins (default `32`; `0` = no backpressure). |
| `[rasn.remote_agent] max_backpressure_ms` | Upper bound in milliseconds on graceful remote-agent dispatch backpressure (default `200`). |
| `[rasn.remote_agent] rate_limit_enabled` | Enables the per-agent coordinator RPC rate limiter (default `true`). Has no effect unless `rate_limit_requests_per_min` is non-zero. |
| `[rasn.remote_agent] rate_limit_requests_per_min` | Sustained coordinator RPC dispatch rate per remote agent in requests/minute (default `0` = unlimited / disabled). |
| `[rasn.remote_agent] rate_limit_burst` | Per-agent rate-limiter burst capacity in tokens (default `0` = about one second of the sustained rate, minimum 1). |
| `[rasn.remote_agent] rate_limit_max_wait_ms` | Max milliseconds a dispatch may be paced waiting for a token before it is rejected (default `1000`; `0` = reject immediately when the bucket is empty). |
| `[rasn.overload] admission_enabled` | Enables the process-wide overload admission gate — a single global concurrency bulkhead across all dependencies (default `true`). |
| `[rasn.overload] max_concurrent_operations` | Hard cap on concurrent in-flight coordinator operations across all dependencies (default `0` = unlimited/passthrough). Each top-level `invoke` counts once; excess operations fast-fail with a non-retryable `overload` response. |
| `[rasn.overload] soft_concurrent_operations` | Process-wide in-flight level at which graceful backpressure begins (default `0` = no backpressure). |
| `[rasn.overload] max_backpressure_ms` | Upper bound in milliseconds on graceful process-wide admission backpressure (default `200`). Inspect live in-flight depth with `rasn.resilience`. |
| `[rasn.overload] rate_limit_enabled` | Enables the process-wide overload rate limiter (default `true`). Has no effect unless `rate_limit_requests_per_min` is non-zero. |
| `[rasn.overload] rate_limit_requests_per_min` | Sustained process-wide operation rate in requests/minute (default `0` = unlimited / disabled). |
| `[rasn.overload] rate_limit_burst` | Process-wide rate-limiter burst capacity in tokens (default `0` = about one second of the sustained rate, minimum 1). |
| `[rasn.overload] rate_limit_max_wait_ms` | Max milliseconds an operation may be paced waiting for a process-wide token before it is rejected (default `1000`; `0` = reject immediately when the bucket is empty). |
| `[rasn.service] host`, `<name>_host`, `<name>_port`, `<name>_uri` | rDSN service graph endpoints. `<name>_uri` takes precedence and supports resolver-backed values such as `dsn://cluster/rasn.coordinator`; otherwise rASN uses host/port. |
| `[rasn.coordinator] max_retry_budget` | Caps per-request `retry_budget` for retryable model-agent dispatch. Tool capabilities are never retried by the coordinator. |
| `[rasn.workflow] execution_lease_ms` | Time-to-live for durable workflow execution owner leases. Active duplicate starts are rejected until the owner finishes or the lease becomes stale. |
| `[rasn.workflow] execution_lease_renew_ms` | Lease renewal interval for active workflow runs. `0` derives a safe interval from the lease TTL. |
| `[rasn.registry] dynamic_registration/heartbeat_ms/lease_ms/sweep_interval_ms/registration_timeout_ms` | Enables best-effort RPC registration of built-in agents plus rDSN timer-driven heartbeats and lease cleanup. `lease_ms = 0` disables TTL filtering; `sweep_interval_ms = 0` disables active cleanup. |
| `[rasn.state] checkpoint_dir/checkpoint_file/journal_file/recover_on_start` | Durable state checkpoint and append-only journal paths. Defaults place checkpoints and journals under `rasn/state`. `recover_on_start` names an explicit recovery checkpoint; if set, recovery failures are surfaced instead of falling back to an empty store. State writes also support create-only and expected-sequence conditions for leases and compare-and-swap style ownership. |
| `[rasn.state.nfs] enabled/remote_host/remote_port/remote_checkpoint_dir/timeout_ms` | Optional rDSN NFS import source used before state recovery when no local checkpoint or journal exists. Enable `[core] start_nfs` on the importing process and run `dsn.tools.nfs` on the source process. `timeout_ms` defaults to `20000` (20 seconds); use about `5000` for local/LAN interactive CLI fail-fast behavior, and keep `20000` or higher when remote recovery success is more important than command latency. |
| `[rasn.state.replica] enabled/directory/recover` | Optional local mirror for state checkpoints and journals. When enabled, writes fail explicitly if the mirror cannot be updated, and recovery can seed missing primary state from the replica directory. The default mirror path is `rasn/state/replica`. Keep `directory` non-empty; an empty directory is a configuration error and should block persistence rather than silently disabling recovery protection. |
| `[rasn.runtime] trace_file` | JSONL file for runtime traces. Defaults use `rasn/traces/<app>.trace.jsonl`; the trace writer creates parent directories. |
| `[rasn.runtime] temp_dir` | Optional temporary directory for request bodies and curl config files. Empty or `.` uses the OS temp directory under `rasn-provider`. |
| `[rasn.metrics] enabled` | Enables rDSN `perf_counter`-backed runtime metrics (event counters and task/model/tool latency percentiles in section `rasn`). Default `true`; `false` makes every counter update a no-op. Surface them with `observe metrics [text|prometheus|json]` or the `rasn.metrics` rDSN command. |
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
| `[rasn.policy] artifact_dir` | Directory used for spilled tool-output artifacts. Defaults to `rasn/artifacts`. |
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

Direct CLI commands omit the slash. Inside interactive mode, `/help` shows the
same command items with the required leading `/`.

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
state <cmd> [args]       use rASN state/checkpoint service (`compact` verifies runtime watermarks)
observe events [kind]    query structured runtime events
observe timeline [trace] show ordered trace events
schema [text|json|idl|cpp|clients-cpp|ts|clients-ts|py|clients-py] print/export schemas and generated RPC clients
observe diagnose [trace] summarize failures and replay issues
observe failures         query classified failure records
observe replay <file>    load replay choices through rasn.observability
observe metrics [format] dump runtime metrics (text|prometheus|json)
observe resilience       dump overload/model/tool/remote-agent resilience state
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

## Standalone rDSN runtime host

The executable can launch the services-only runtime host using
`config.rasn.ini` (which includes sibling `config.rasn.defaults.ini`):

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe serve
```

This registers and runs the runtime service types independently of the CodePilot
gateway:

```text
rasn.registry
rasn.coordinator
rasn.llm.agent
rasn.tool.agent
rasn.state
rasn.workflow
rasn.observability
rasn.runtime
```

`rasn.runtime` hosts the shared rASN runtime modules (agent
control plane, message bus, task kernel, determinism ledger, capability
directory, resource budget, recovery supervisor, blackboard, contract verifier,
human interaction, and sandbox runtime) behind one service. See
[Distributed rASN runtime modules](#distributed-rasn-runtime-modules) for how
to split these modules across processes or nodes.

For a quorum-backed state authority, build with `--build_plugins` and launch
`config.rasn.state.ini` in a separate service process. It starts a one-partition,
three-replica `rasn.state.replicated` development cluster. Point runtime hosts at
`state_uri = dsn://rasn-cluster/rasn-state`; applications and runtime modules keep
the same state API. The single-machine profile validates quorum behavior but must
be split into independently supervised replica nodes for host-failure tolerance.

`rasn.llm.agent`, `rasn.tool.agent`, `rasn.coordinator`, `rasn.workflow`, and
`rasn.observability` all retain the shared
`rasn_service_graph` while active; service shutdown releases those references in
any order and only the final release stops the graph.

Use `topology` (or `/topology` interactively) from a local or distributed app to inspect the
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
rasn_runtime_port = 27107
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

For a repeatable service RPC smoke, point the built executable at
`src\plugins\rasn\examples\service-rpc-smoke.ini`. The `rasn.codepilot` app runs
`selftest` through the service graph, then the rDSN process remains alive until
you stop it. Because the normal host is services-only, explicitly include the
test gateway in this custom app list:

```bat
C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\codepilot\Debug\codepilot.exe serve src\plugins\rasn\examples\service-rpc-smoke.ini "rasn.registry;rasn.llm.agent;rasn.tool.agent;rasn.state;rasn.coordinator;rasn.workflow;rasn.observability;rasn.codepilot"
```

The direct one-shot CLI mode is recommended for local prototyping and provider testing.

### Distributed rASN runtime modules

Apps do not own the rASN runtime modules directly. They call a `rasn_runtime`
facade, and a provider decides how each call is executed:

- **local** (default): calls resolve in-process, or over an intra-node LPC when a
  rDSN node is present. State lives in the process-global module store.
- **distributed**: every call is a typed rDSN RPC to a module service, so module
  state is owned by the service node and can run on a remote host. Select it with
  `[rasn.runtime] rasn_runtime_provider = distributed` (and
  `rasn_runtime_strict = true` to label module RPC failures as strict provider
  failures).
- **hybrid**: each module is routed independently, so hot or latency-sensitive
  modules stay in-process while shared, stateful modules (for example `blackboard`
  or `resource_budget`) are pushed onto their own service nodes. Select it with
  `[rasn.runtime] rasn_runtime_provider = hybrid`, then set
  `[rasn.service] rasn_runtime_default_mode` and per-module `<module>_mode`
  (`local` or `remote`).

The provider is chosen like an rDSN environment/aspect provider: apps depend only
on the facade API, and swapping `local`/`distributed`/`hybrid` changes where the
modules run without any app code change.

A standalone host's aggregate `rasn.runtime` app hosts all eleven modules
behind one endpoint (default port `27107`). Each module also has a standalone
role so it can be deployed on its own process or node:

| Module | Standalone role | Default port |
| --- | --- | --- |
| agent_control_plane | `rasn.runtime.agent_control` | 27110 |
| agent_message_bus | `rasn.runtime.message_bus` | 27111 |
| task_orchestration_kernel | `rasn.runtime.task_kernel` | 27112 |
| determinism_ledger | `rasn.runtime.determinism` | 27113 |
| capability_directory | `rasn.runtime.capability` | 27114 |
| resource_budget | `rasn.runtime.budget` | 27115 |
| recovery_supervisor | `rasn.runtime.recovery` | 27116 |
| blackboard | `rasn.runtime.blackboard` | 27117 |
| contract_verifier | `rasn.runtime.contract` | 27118 |
| human_interaction | `rasn.runtime.human_interaction` | 27119 |
| sandbox_runtime | `rasn.runtime.sandbox_runtime` | 27120 |

Launch a single module service with a state authority available for hydration by
passing the state service plus module name (module name or role) after the config;
the app-list is normalized to the matching standalone role. Deploy
`config.rasn.ini` and `config.rasn.defaults.ini` beside the binary:

```bat
codepilot.exe serve config.rasn.ini "rasn.state;resource_budget"
```

When `rasn_runtime_registry_registration_enabled` is true, each runtime module
service publishes a lease-tracked capability such as
`rasn.runtime.resource_budget` to `rasn.registry`. Sharded services can also
publish explicit partition capabilities such as
`rasn.runtime.blackboard.shard.0` by setting `<module>_hosted_shards` or
`<module>_shard_index` on the service process. Set
`rasn_runtime_advertise_host` (or `<module>_advertise_host`) on the host to the
address clients can dial; otherwise rDSN's primary address is published.
Distributed and hybrid clients use an explicitly configured module/runtime
address authoritatively. Only modules left unconfigured use registry discovery,
with the static localhost endpoint as the final fallback.

Point clients at each module independently with `[rasn.service]` overrides. The
key prefix is the module name, and any of `uri`, `host`, or `port` may be set:

```ini
resource_budget_uri = dsn://meta-server:34601/rasn-resource-budget
blackboard_host = remote-host
blackboard_port = 27117
```

Sharded modules (`agent_message_bus`, `resource_budget`, and `blackboard`) also
support deterministic key-based partition routing. Set `<module>_shard_count`,
then optionally override each shard with
`<module>_shard_<n>_uri` or `<module>_shard_<n>_{host,port}`. Writes and keyed
reads route by the module's natural key (`message_id`, budget `scope`, or
blackboard `key`); snapshot-style reads fan out to every configured partition and
merge typed results.

```ini
blackboard_shard_count = 2
blackboard_shard_0_host = blackboard-a
blackboard_shard_0_port = 27117
blackboard_shard_1_host = blackboard-b
blackboard_shard_1_port = 27127
```

For a module-level `dsn://` URI, rASN passes the stable key hash as the rDSN RPC
partition hash. The core `dist::partition_resolver` then owns
partition-to-replica-group resolution, cache invalidation, and retry after access
failure. Set `<module>_shard_count` to the meta-server table's partition count;
explicit per-shard endpoints remain authoritative overrides. Existing FNV-1a key
hashing is preserved so enabling resolver-backed routing does not silently remap
already-sharded state.

```ini
[rasn.service]
blackboard_shard_count = 4
blackboard_uri = dsn://rasn-cluster/rasn-blackboard

[uri-resolver.dsn://rasn-cluster]
factory = partition_resolver_simple
arguments = meta-1:27601,meta-2:27601
```

The client path is resolver-aware, but current runtime module app roles are still
single-writer in-memory services rather than rDSN replicated tables. Meta-managed
module replica groups require the direct module-replication work described in
`docs/DISTRIBUTED_RUNTIME.md`.

For registry-routed sharded deployments, set the same shard count on clients and
configure each standalone module service with the shard labels it owns:

```ini
blackboard_shard_count = 2
blackboard_hosted_shards = 0
```

The service advertises both `rasn.runtime.blackboard` and
`rasn.runtime.blackboard.shard.0`; clients query the shard-specific capability
first and use the module-level descriptor only as a fallback.

Distributed RPCs are resilient to cross-node latency and transient transport
errors. These `[rasn.service]` knobs apply to the shared endpoint and accept the
same per-module prefix (for example `resource_budget_timeout_ms`):

```ini
; 0 falls back to [rasn.rpc] timeout_ms (default 5000).
rasn_runtime_timeout_ms = 0
; extra attempts on timeout/network/busy/capacity/try-again errors.
rasn_runtime_retries = 2
rasn_runtime_retry_backoff_ms = 50
rasn_runtime_ping_timeout_ms = 1000
```

The runtime also owns two higher-level resilience policies for remote calls, so a
single unhealthy module node cannot stall or corrupt the system:

- **Per-endpoint circuit breaker.** After `rasn_runtime_breaker_failures`
  consecutive transport failures to a resolved module/shard endpoint, its breaker opens and calls
  short-circuit for `rasn_runtime_breaker_open_ms` before a half-open probe is
  admitted. This bounds the blast radius of a dead node instead of retrying into it.
- **Idempotent retries.** When `rasn_runtime_idempotency_enabled` is set, the
  client stamps each remote call with a unique id that is stable across its own
  retries. Module services dedup mutating operations by full request signature
  plus id: a concurrent duplicate waits on the in-flight placeholder and receives
  the first response instead of applying twice. Read-only operations bypass the
  cache, and the completed-response window is bounded by
  `rasn_runtime_dedup_capacity` plus `rasn_runtime_dedup_ttl_ms`. Hit/miss/wait/
  eviction/expiry counters are exposed as rASN metrics. This suppresses
  lost-reply double-apply within one service process; it is not cross-process
  exactly-once.
- **Optional service-to-service auth.** When `rasn_runtime_auth_enabled` is true,
  distributed/hybrid clients stamp a shared token onto runtime module RPC
  envelopes and runtime services reject missing or invalid tokens before
  dispatching to module handlers. Rejections increment
  `rasn_runtime_auth_rejected_total`. Keep it disabled for trusted local
  experiments; for multi-node deployments, provide the same
  `rasn_runtime_auth_token` to every client and module service through
  deployment-specific config, not the checked-in sample config.

```ini
rasn_runtime_breaker_enabled = true
rasn_runtime_breaker_failures = 5
rasn_runtime_breaker_open_ms = 30000
rasn_runtime_idempotency_enabled = true
rasn_runtime_dedup_capacity = 8192
rasn_runtime_dedup_ttl_ms = 300000
rasn_runtime_auth_enabled = false
rasn_runtime_auth_token =
```

Readiness is probed by pinging every module through the facade, so a module
whose service is down (locally or on a remote node) is reported by name instead
of being masked by a static summary. In distributed mode the ping uses the
dedicated `*_ping_timeout_ms` budget and a single attempt so an unreachable
endpoint surfaces quickly; sharded modules probe every configured shard.
`rasn_runtime::describe_topology()` renders where each module is routed (local
vs. remote), whether the endpoint came from `registry:`, `static:`, or
resolver-backed `resolver:` config, per-shard endpoint labels when applicable,
its standalone role, and both its
intended consistency model and its `actual=single_writer_in_memory` runtime
backing, which is useful for verifying a hybrid or multi-node layout.

Each module declares an intended distributed consistency model
(`rasn_runtime_module_descriptors()`): `sharded` modules (`blackboard` by key,
`resource_budget` by scope, `agent_message_bus` by message id) now have
deterministic partition routing, ownership/ledger modules are `replicated` (for
example `agent_control_plane`, `determinism_ledger`), and control-surface modules
are `singleton` (for example `human_interaction`, `sandbox_runtime`). These
document the target rDSN-native replication strategy; the current in-memory
service store realizes each shard/replica as a single-writer service store.

> **Operational warning — one active writer per shard.** Key-based sharding is a
> placement/routing layer, not replication. Do **not** run multiple active
> instances for the same shard expecting high availability: you would get split
> state. Replicated modules still need one active writer until real rDSN
> replication fronts them. The state-service mirror written in `distributed` mode
> is replayed by module services on startup. Each mirrored mutation also writes a
> per-module watermark by default; hydration verifies those watermarks before
> replay so a torn, incomplete, or pre-watermark mirror fails closed instead of
> serving partial state. In strict runtime mode, a mutation whose state mirror or
> watermark write fails is returned as a failed facade call rather than success
> with only a warning. `codepilot state compact [--prefix <state-prefix>]
> [checkpoint-path]`
> lets operators verify existing watermarks and fold the shared state service into
> a compact checkpoint/journal baseline. When hydration is enabled, a module
> service refuses to open its RPC API if the configured state service cannot be
> queried; disable hydration only for intentionally cold local experiments. In `hybrid` mode,
> flipping a module from `local` to `remote` is still a cold migration:
> local-routed modules are not mirrored, so the remote service starts without the
> previously local state unless the operator explicitly migrates it.

For the full architecture, provider model, resilience contracts, consistency
models, and the multi-node roadmap, see
[docs/DISTRIBUTED_RUNTIME.md](docs/DISTRIBUTED_RUNTIME.md).

### Current product limitations

The current implementation is a usable prototype platform, but these product
hardening gaps remain:

| Area | Current capability | Remaining limitation |
| --- | --- | --- |
| State availability | Standalone checkpoints/journal/NFS/local mirror, plus optional one-partition `rasn.state.replicated` quorum writes and checkpoint learning over rDSN type-1 replication. | Runtime modules still execute as elected single-writer in-memory stores; multi-partition query fan-out, safe online checkpoint GC, and an HA meta-server deployment remain. |
| Discovery availability | Local registry by default; optional ZooKeeper-backed shared descriptors/leases, committed-epoch active-writer failover, read-capable standby frontends, bounded epoch/tombstone retention, and rDSN group-address client failover across `registry_addresses`. | Automated multi-process registry-writer failover evidence remains. |
| Tool isolation | Default-deny side effects, workspace scoping, approvals, command allowlists, timeout/job containment, and a configurable container command wrapper. | No hardened container orchestrator with image, mount, network, and lifecycle policy. |
| Replay fidelity | Replay for model responses, tool results, workflow scheduling, filesystem snapshots, and an `external.effect` ledger for side-effect intents. | No full virtualization of arbitrary external services, clocks, network state, or process environments. |
| Deployment validation | Inline mode, typed service-mode RPC, URI/host endpoint configuration, registry heartbeats, active lease cleanup, distributed runtime modules, state-mirror hydration/watermarks, and a deployable rDSN type-1 replicated-state profile. | Full replicated-state cluster automation, direct quorum replication/sharding of module state, explicit watermark pruning, and local-to-remote migration tooling are not yet implemented. |
| Credentials | `token_ref` handles for environment variables, files, and commands, plus deterministic redaction. | No vault-backed or OS-backed credential provider integration. |
| SDK packaging | Generated C++/TypeScript/Python contracts and RPC-client source. | Packaged SDKs and concrete TypeScript/Python transports are not shipped. |
| Evaluation evidence | Unit tests, self-tests, service smokes, schema smokes, report build, and a small eval harness. | Large benchmarks and user studies for debugging effectiveness remain future work. |
| Observability metrics | Cumulative event counters and task/model/tool latency percentiles via rDSN `perf_counter`, exported as text/Prometheus/JSON through `observe metrics` and the `rasn.metrics` rDSN command. | No bundled scrape gateway, retention store, or prebuilt dashboards; percentiles rely on rDSN's periodic counter timers. |
| Overload / dependency isolation | Model providers and remote-agent RPC dispatch now have the full failure/concurrency/throughput trio: per-dependency circuit breaker, admission gate, and token-bucket rate limiter. The model gateway adds a token/cost budget that weighs each request by its estimated token cost, bounding tokens-per-minute and metered spend rather than just request count. Tool execution has per-tool admission and rate controls using the same engines, with replay/policy bypasses and live state in `observe resilience` / `rasn.resilience`. A process-wide overload budget (`[rasn.overload]`) reuses the admission/rate engines as singletons at the coordinator `invoke` chokepoint to bound total in-flight work and aggregate throughput across all dependencies (opt-in; passthrough by default). | In RPC-client mode model/tool guard state lives on the serving node while remote-agent and process-wide overload guard state live on the coordinator. |

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `dsn.core.dll` or other runtime DLL cannot be found | Ensure `C:\Users\haoxlin\source\repos\rdsn\rb-rasn\bin\Debug` is first on `PATH` before running `codepilot.exe`. |
| Runtime-host RPC calls fail | Confirm no other process owns ports `27100`-`27107`, then rerun `codepilot.exe serve`. |
| Provider request fails | Run `codepilot.exe providers` and verify `[rasn.model]` or compatibility `[rasn.llm]` has the expected `provider`, `endpoint`, and `model`. |
| Copilot/OpenAI-compatible authentication fails | Store credentials in `token_env` or trusted `token_command`; do not place token values in `config.ini` or trace files. |
| Write or shell tools are denied | This is the safe default. Set `[rasn.policy] allow_write = true` or `allow_shell = true` only for trusted local tests. If approval is required, confirm the prompt or use `tool --yes ...` for direct scripted invocations. |
| Replay output does not match expectations | Use `observe events nondeterminism`, `observe timeline`, and `observe diagnose` to inspect replay loads and replay misses. |
| Runtime metrics read as zero | Counters are cumulative and created lazily on first matching event; latency percentiles are computed by rDSN counter timers, so they populate after the first timer interval. Confirm `[rasn.metrics] enabled = true`. |
| State recovery misses recent writes | Check `[rasn.state] journal_file`; recovery replays the journal after loading the compact checkpoint. |
