# rASN generic multi-agent system design

This document describes the target architecture for Robust Agent System Nucleus
(rASN) as a generic rDSN-native multi-agent runtime. CodePilot is only one
application built on top of this runtime.

## Design goals

- Model every agent as a distributed service component with explicit lifecycle,
  identity, capability, configuration, RPC boundary, state ownership, and
  observability.
- Use rDSN public C/C++ APIs from `include/dsn/c` and `include/dsn/cpp` instead
  of ad-hoc runtime infrastructure.
- Make nondeterminism explicit: LLM calls, tool side effects, scheduling
  decisions, retries, and failure handling must be traceable and replay-aware.
- Prefer typed request/response schemas and rDSN serialization over string
  envelopes.
- Keep CodePilot-specific concepts, prompts, skills, and local tools outside the
  generic rASN core.

## rDSN API and example basis

rASN should follow these existing rDSN patterns:

| Area | rDSN API or example | rASN use |
| --- | --- | --- |
| Service lifecycle | `dsn::service_app` | Each long-running component is an app role with `start` and `stop`. |
| RPC server | `dsn::serverlet<T>` and `dsn::rpc_replier<T>` | Agent services expose typed handlers. |
| RPC client | `dsn::clientlet`, `dsn::rpc::call`, `wait_and_unwrap` | Coordinator and applications call agents through generated-style clients. |
| Task codes | `DEFINE_TASK_CODE_RPC` | Every cross-component operation has an explicit task code. |
| Serialization | `binary_writer`, `binary_reader`, `marshall`, `unmarshall` | All generic messages are typed and versioned. |
| Configuration | `dsn_config_get_value_*` | Ports, policies, budgets, timeouts, persistence paths, and provider settings are config-driven. |
| Logging/assertions | `dinfo`, `dwarn`, `derror`, `dassert` | Runtime behavior and invariant violations are visible. |
| Locks | `dsn::service::zlock`, `zauto_lock` | Shared service state is protected using rDSN locks. |
| Filesystem | `dsn::utils::filesystem::*` | Checkpoints, traces, temp payloads, and workflow files use rDSN utilities. |
| Timers/tasks | `dsn::tasking::enqueue_timer` | Heartbeats, leases, retries, and background maintenance. |
| Echo sample | `src/plugins/apps.echo` | Minimal service_app/serverlet/clientlet/task-code pattern. |
| SKV sample | `src/plugins/apps.skv` | Stateful service, locks, checkpoints, and recovery. |
| Deployment sample | `src/plugins_ext/rDSN.dist.deployment` | Multi-RPC service and richer app wrapper pattern. |

## Component graph

```text
application adapters
  CodePilot, future agents/CLIs/services
        |
        v
rasn.coordinator  <---->  rasn.registry
        |
        +---- capability route ----> rasn.model.agent
        +---- capability route ----> rasn.tool.agent
        +---- capability route ----> custom agent services
        |
        +---- state/checkpoint ----> rasn.state
        +---- policy checks -------> rasn.policy
        +---- trace/failure -------> rasn.observability
        +---- counters/latency ----> perf_counter (rasn.metrics)
        |
        +---- workflow execution --> rasn.workflow
```

`rasn_service_graph` is the shared in-process boundary for direct mode and the
client facade for service mode. rDSN app wrappers that use it retain the graph on
startup and release it on shutdown; the graph stops local agents, injected state
writers, and maintenance timers only after the last lifecycle owner releases it.
Application adapters such as CodePilot may configure providers during
construction, but command execution is responsible for retaining the graph; this
keeps construction side-effect free and preserves rDSN app startup ordering.
Service-mode command adapters probe critical dependencies before executing, and
workflow startup recovery is delayed until state RPC is ready, so concurrent app
startup does not surface as user-visible command failures.

## Core components

### 1. Agent runtime

The agent runtime is the common base for service-like agents.

Responsibilities:

- Own agent identity: `agent_id`, role name, app name, address, instance index,
  and version.
- Own lifecycle: initialize config, start providers, register RPC handlers, stop
  cleanly, and release resources.
- Expose capabilities: task types, tools, models, side-effect classes, cost
  hints, latency hints, and reliability hints.
- Apply common validation, logging, tracing, timeout defaults, and result
  normalization.

rDSN design:

- Service implementations are wrapped by `dsn::service_app` roles.
- RPC-facing agents inherit from `dsn::serverlet<T>`.
- Client wrappers inherit from `dsn::clientlet`.
- Common mutable state uses `dsn::service::zlock`.
- Agent config is read from sections such as `[rasn.agent.<role>]`.
- Service-mode smoke runs in one rDSN process by default. Static external
  descriptors can be loaded from `[rasn.agent.*]`, and service clients can be
  pointed at per-service host/port or `dsn://...` URI endpoints through
  `[rasn.service]`. Built-in agents use registry RPCs plus a rDSN timer task for
  best-effort dynamic registration and heartbeat leases. Inline mode keeps a
  same-process service graph for local prototyping; service mode can route
  through explicit RPC endpoints and generated clients so application adapters do
  not need to depend on concrete in-process stores or providers.

Correctness and robustness requirements:

- `start` is idempotent and validates required config before serving requests.
- `stop` is idempotent and unregisters RPC handlers.
- Every request gets a request id, trace id, timeout budget, and deterministic
  error if rejected before execution.
- A request timeout budget of `0` means "use `[rasn.rpc] timeout_ms`"; a
  nonzero request or workflow-node budget becomes the client-side rDSN RPC
  deadline for service-mode dispatch.
- Agents must not silently ignore malformed requests.

### 2. Agent registry

The registry records which agents exist and which capabilities they provide.

Responsibilities:

- Register/unregister agent descriptors.
- Query agents by capability, role, address, health, cost, and policy labels.
- Provide stable routing metadata to the coordinator.
- Support static config registration from `[rasn.agent.*]`, then dynamic
  registration and heartbeat-based leases.

rDSN design:

- `rasn.registry` is a `service_app` with a `serverlet`.
- Registry state is protected by `zlock`.
- Initial entries come from `dsn_config_get_all_sections` over
  `[rasn.agent.*]`.
- Static entries are loaded from `[rasn.agent.*]` sections at registry startup.
- Agent descriptors carry either host/port or `endpoint_uri`; the coordinator
  prefers URI routing and falls back to IPv4 host/port routing.
- Built-in agents register with `rasn.registry` through typed RPCs when service
  clients are enabled. `LPC_RASN_REGISTRY_HEARTBEAT_TIMER` sends periodic
  heartbeats, and lease-tracked entries are filtered from healthy queries once
  their heartbeat age exceeds `[rasn.registry] lease_ms`.
- `LPC_RASN_REGISTRY_LEASE_SWEEP_TIMER` runs inside the registry service app and
  actively removes expired lease-tracked entries. Static descriptors are not
  lease-tracked and are not removed by sweeps.
- Dynamic updates use typed register/unregister/heartbeat RPCs.
- Direct CLI mode also loads static entries before built-in agents register, so
  `registry` and `agentctl` see the same configured agents without requiring the
  full service graph.

Correctness and robustness requirements:

- Duplicate agent ids are rejected unless the descriptor version is newer and
  the update is explicit.
- Expired or unhealthy entries are not returned for routing unless requested for
  diagnosis.
- Capability matching is exact by default; fuzzy matching is an explicit policy.

### 3. Task and message model

The task model is the generic protocol shared by all agents.

Responsibilities:

- Define `agent_request`, `agent_response`, `agent_error`,
  `agent_capability`, `agent_descriptor`, `agent_context`, and
  `agent_artifact`.
- Represent parent/child task relationships and workflow node ids.
- Carry timeout, retry, policy, budget, trace, and replay metadata.
- Preserve deterministic error information across RPC boundaries.

rDSN design:

- Messages live in generic rASN headers, not CodePilot files.
- Each struct has `marshall` and `unmarshall` functions.
- Message fields include a schema version for forward compatibility.
- RPC handlers use typed structs directly.
- `schema_manifest.*` exposes a runtime catalog of core message contracts for
  inspection, JSON export, IDL export, C++/TypeScript/Python SDK-stub
  generation, generated C++/TypeScript/Python RPC-client wrappers, and
  regression tests.
- The RPC operation manifest enumerates service, task-code, request, and
  response types for agent, registry, state, workflow, model, and observability
  calls. `codepilot schema clients-cpp`, `clients-ts`, and `clients-py` turn that
  manifest into source-level client wrappers instead of relying only on
  hand-written C++ clients.

Correctness and robustness requirements:

- All required fields are validated at service boundaries.
- Unknown enum/string values are preserved when safe and rejected when unsafe.
- Responses carry either success payload or structured error, never ambiguous
  partial success.
- The schema manifest lists the core runtime, registry, tool, policy, workflow,
  state, and observability contracts exposed by the CLI, including nested
  records referenced by top-level requests and responses. The exported JSON/IDL
  forms give external tooling a stable integration surface, while
  `codepilot schema cpp`, `codepilot schema ts`, and `codepilot schema py`
  generate self-contained C++, TypeScript, and Python contract stubs, while
  `clients-cpp`, `clients-ts`, and `clients-py` generate executable
  transport-backed clients for external agents, tools, and test harnesses.

### 4. Message/RPC layer

The message layer defines the typed RPC surface for generic agents.

Responsibilities:

- Provide common RPCs for describe, invoke, cancel, heartbeat, and query.
- Provide generated-style client wrappers for synchronous and asynchronous use.
- Hide address and timeout details from application adapters.

rDSN design:

- Task codes are defined in `rasn.code.definition.h` with
  `DEFINE_TASK_CODE_RPC`.
- Handlers are registered with `serverlet::register_async_rpc_handler`.
- Clients call through `dsn::rpc::call` and `wait_and_unwrap`.

Correctness and robustness requirements:

- Every call has a configured timeout.
- Generic `agent_request.timeout_ms` overrides the global timeout for registry,
  coordinator, and routed agent invocations. This is a client deadline; it
  bounds the caller wait time. For HTTP model providers, it is also propagated
  through the compatibility completion request into `llm_request.timeout_ms` and
  applied as curl `max-time`, capped by `[rasn.model] request_timeout_sec`.
  Arbitrary local provider work still requires cooperative cancellation.
- Built-in agents maintain bounded in-flight request tracking keyed by
  `agent_request.request_id`. `RPC_RASN_AGENT_CANCEL` records a cancellation
  tombstone for matching work, makes later duplicate starts fail as
  `request_cancelled`, and converts a cancelled in-flight result into a
  structured cancellation failure once the provider/tool returns.
  Tombstone trimming never evicts a cancellation while the corresponding request
  is still in flight, preserving terminal cancellation under cancel storms.
- RPC errors are converted into `agent_error` with rDSN error code and operation
  metadata.
- Retry is explicit and bounded. `agent_request.retry_budget` allows retryable
  model-agent dispatch failures to be attempted again through the coordinator,
  capped by `[rasn.coordinator] max_retry_budget`; retry events and retryable
  failures are written into the runtime trace. Tool capabilities are excluded
  from coordinator retries because a timed-out tool RPC may already have executed
  side effects.
- Handler registration/unregistration is paired and logged.
- A cancel for an unknown request returns `cancel_not_found` rather than being
  silently ignored.

### 4.1 Operator-facing inspection

rASN should expose enough runtime information for developers to treat agents as
service components rather than hidden prompt calls.

Current CLI surfaces:

- `registry list|get|query` inspects registered agents and capability routing
  metadata. In service mode it reads the live `rasn.registry` RPC surface.
- `agentctl describe|heartbeat|query|cancel` exercises generic agent control RPCs
  for built-in coordinator/model/tool agents.
- `workflow nodes <run-id>` reads node-level workflow state persisted by
  `rasn.workflow`.
- `observe metrics [text|prometheus|json]` dumps the perf-counter metrics
  snapshot; service deployments answer the same data over the rDSN
  `command_manager` command `rasn.metrics`.

These commands are intentionally simple text output for now. A future version
should expose the same data as stable JSON for external tooling.

### 5. Coordinator and orchestrator

The coordinator routes tasks and drives multi-agent execution.

Responsibilities:

- Accept application requests and workflow execution requests.
- Query the registry for candidate agents.
- Apply policy, choose routes, dispatch requests, and merge outputs.
- Track task state transitions and emit trace events.
- Drive dependency execution for workflows.

rDSN design:

- `rasn.coordinator` is a `service_app` with typed RPC handlers.
- It uses registry, policy, state, observer, and agent clients.
- It should not call concrete model/tool implementation classes directly after
  migration.

Correctness and robustness requirements:

- Routing is deterministic for the same registry snapshot and policy settings
  unless a configured nondeterministic strategy is recorded in the trace.
- Dependency failures are propagated with explicit failure class and blocked
  downstream nodes.
- Repeated client retries are bounded by request budget and idempotency flags.

### 6. Workflow graph

The workflow graph is the declarative execution model.

Responsibilities:

- Represent tasks, dependencies, inputs, outputs, capabilities, policies, and
  artifacts.
- Validate graph structure: no cycles, missing nodes, invalid dependencies, or
  unsupported capability references.
- Provide a stable executable order for deterministic replay.

rDSN design:

- Parser can remain a library initially.
- Validated workflow specs are submitted to `rasn.workflow`.
- File access uses `dsn::utils::filesystem`.

Correctness and robustness requirements:

- Invalid workflow specs fail before any side effect.
- Node ids are unique and stable.
- Text and JSON workflow specs normalize into the same `workflow_node` model, so
  CLI-friendly files and tool-generated structured files share validation,
  optimization, replay, and execution semantics.
- Graph validation reports obvious errors before execution, including duplicate
  node ids, unsupported actions, invalid state keys, malformed numeric hints, and
  dependency cycles.

### 7. Workflow compiler and executor

The compiler turns declarative workflows into executable multi-agent plans.

Responsibilities:

- Bind workflow nodes to required capabilities.
- Annotate nodes with budgets, retries, state keys, and trace ids.
- Parse both the compact text workflow language and structured JSON workflow
  specs with the same validation rules.
- Optimize for latency, cost, and reliability when policy allows.
- Execute ready nodes and persist progress.

rDSN design:

- `rasn.workflow` is a `service_app`.
- Execution state is stored through `rasn.state`; in service mode workflow
  persistence uses the state client/server boundary instead of directly touching
  the in-process store.
- Execution uses coordinator RPC rather than direct provider calls.
- `RPC_RASN_WORKFLOW_START` runs on `THREAD_POOL_RASN_WORKFLOW` so synchronous
  workflow execution does not occupy the default RPC pool while it calls
  coordinator/model/tool RPCs.

Correctness and robustness requirements:

- Compilation is deterministic for a given workflow, registry snapshot, and
  policy.
- Optimizations must preserve declared dependencies and side-effect ordering.
- Execution persists whole-run start/end records and latest per-node state.
- Whole-run, per-node, resume, and lease state operations route through the
  service graph state APIs. This keeps inline and service paths behaviorally
  aligned while letting rDSN RPC mode exercise `rasn.state` independently.
- Workflow execution owns `workflow-lease/<run-id>` before dispatching nodes.
  Leases are written through conditional `rasn.state` operations, reject active
  duplicate execution, and can be taken over after
  `[rasn.workflow] execution_lease_ms`.
- Active workflow owners renew their lease while nodes are running. In service
  context renewal uses an rDSN timer task (`LPC_RASN_WORKFLOW_LEASE_RENEW_TIMER`);
  inline/direct execution uses the same conditional state update loop from a
  local renewal thread so the CLI path has the same ownership semantics.
- Whole-run records are recoverable from `rasn.state` on workflow-service startup
  when state has already been recovered, and on demand when `workflow query` or
  `workflow cancel` misses the in-memory run table. Recovery distinguishes a
  missing workflow key from state-service failures; non-missing-key errors are
  returned to the caller rather than converted into `workflow run not found`.
- `workflow resume <file> <run-id>` reloads completed per-node records from
  `rasn.state`, seeds dependency outputs, marks skipped nodes as `resumed`, and
  executes only incomplete downstream nodes through the normal rDSN workflow
  service path.
- Dependency failures mark dependent nodes as `blocked`.
- Cancellation is cooperative, terminal, and durable: the `cancelled` transition
  is written through a conditional `rasn.state` update, and the cancel request
  fails explicitly if that state write fails or races with a newer terminal run
  record.
- Agent-level cancellation is also cooperative: server-side preemption of
  already-running provider or tool work remains a planned reliability feature.

### 8. State and checkpoint store

The state store owns durable runtime state.

Responsibilities:

- Store task state, workflow state, intermediate outputs, artifacts, replay
  values, and checkpoints.
- Provide snapshot, recover, query, and garbage-collection operations.
- Separate secret references from persisted task data.

rDSN design:

- `rasn.state` follows the SKV pattern: service app, serverlet, `zlock`,
  checkpoint directory, recovery on start.
- Mutations can be unconditional, create-only, or expected-sequence checked.
  The conditional path is exposed as `RPC_RASN_STATE_PUT_CONDITIONAL` and is used
  by workflow ownership leases.
- If `[rasn.state] recover_on_start` is configured, service startup recovers that
  explicit path and fails on recovery errors. If it is empty, startup
  auto-recovers the configured default checkpoint/journal when one exists and
  skips recovery on first boot.
- Optional `[rasn.state.nfs]` settings let recovery first pull checkpoint files
  from an existing `dsn.tools.nfs` source when local state is absent. This reuses
  rDSN's NFS module for remote state seeding without making the default local
  checkpoint path depend on a network service.
- Optional `[rasn.state.replica]` settings mirror checkpoint and journal files to
  a local replica directory. When enabled, state writes fail explicitly if the
  mirror cannot be updated, and recovery can seed missing primary checkpoint or
  journal files from the replica before attempting NFS import.
- Files and directories use `dsn::utils::filesystem`.
- Future replicated mode can follow `replicated_service_app_type_1`.

Correctness and robustness requirements:

- Writes are atomic at the logical operation level.
- Checkpoints include schema version and last committed event sequence.
- Replica mirrors are write-through rather than best-effort; mirror failures are
  surfaced to the caller instead of being silently ignored.
- Recovery validates checkpoint integrity and refuses corrupt state unless
  configured for best-effort diagnosis.

### 9. Model gateway

The model gateway is the generic LLM/model agent.

Responsibilities:

- Expose model invocation as an agent capability.
- Support simulator, Copilot-compatible, Ollama, llama.cpp, LM Studio, and
  OpenAI-compatible providers.
- Record nondeterminism, prompts, sanitized metadata, token source, and errors.
- Expose a provider-neutral streaming callback surface and record emitted chunks.
- Keep credentials outside source files and traces.

rDSN design:

- Current `rasn.llm.agent` becomes a `model_agent` implementation of the generic
  runtime.
- Provider settings come from `[rasn.model.*]` or `[rasn.llm]`.
- Credentials are referenced by `token_ref` handles (`env:`, `file:`, or
  `cmd:`) or by legacy environment-variable names, not stored as literal config
  values. Provider descriptors expose only safe handles such as
  `cmd:<configured>`, never token values or command output.
- `llm_provider::complete_streaming` is the generic streaming API. Providers that
  do not implement native streaming inherit a default implementation that chunks
  the completed response; providers with native streaming can emit chunks through
  the same callback. Every chunk is redacted and recorded as
  `llm.response.chunk`.

Correctness and robustness requirements:

- Token material is never logged, traced, or placed in persisted source files.
- Explicit credential handles that cannot produce a token fail before an HTTP
  request is issued.
- Provider failures include HTTP/process exit status when available.
- Simulator output is replayable through recorded nondeterministic choices.
- Streaming callbacks must not bypass redaction or trace capture.

### 10. Tool gateway

The tool gateway is the generic side-effect boundary.

Responsibilities:

- Expose local or remote tools as agent capabilities.
- Validate arguments and policy before execution.
- Expose structured tool descriptors with side-effect classes and argument
  schemas for orchestration and UI surfaces.
- Classify tools by side-effect level: read-only, workspace write, shell,
  network, secret access.
- Return structured output, error, and artifact references.

rDSN design:

- Current `agent_tool_provider` remains the provider interface but moves behind a
  generic `tool_agent`.
- CodePilot local tools remain under `codepilot`.
- `describe_tool_schemas()` exposes descriptors; text descriptions are derived
  from the same structured source.
- Tool policy comes from `rasn.policy` and rDSN config.

Correctness and robustness requirements:

- Side-effect tools are denied by default.
- The CodePilot provider re-checks policy locally as defense in depth; direct
  provider calls should not bypass default-deny write/shell gates.
- `[rasn.policy] workspace_root` can scope local tool targets to a repository or
  workspace after rDSN filesystem absolute/normalized path resolution.
- `[rasn.policy] require_write_approval` and `require_shell_approval` require a
  `human_approved:<side-effect>` policy label before enabled write or shell
  operations can run. CodePilot obtains that label through an interactive prompt
  or the explicit `tool --yes` direct-command flag.
- `[rasn.policy] shell_allowed_commands` optionally restricts shell execution to
  named executables and rejects shell metacharacters while the allowlist is
  active.
- `[rasn.policy] shell_working_directory` or `workspace_root` can force shell
  commands to start from a known working directory, reducing accidental
  execution outside the target repository.
- `[rasn.policy] shell_timeout_ms` bounds shell command runtime; on Windows the
  shell process is launched with explicit stdout/stderr pipes and terminated when
  the deadline expires.
- `[rasn.policy] shell_executor=container` routes an approved shell command
  through `shell_container_template` with `{command}`, `{raw_command}`,
  `{workspace}`, and `{raw_workspace}` placeholders. The default `local`
  executor preserves existing behavior.
- Tool execution is guarded after replay and policy checks by per-tool admission
  and rate controls. Replayed results and policy-denied side effects do not
  consume tool capacity; admitted calls hold an RAII `admission_slot` while the
  provider runs.
- The tool provider registration lock is held only while taking a shared provider
  reference. Actual execution concurrency is governed by `[rasn.tool]`, not by an
  implicit mutex around all tools.
- Every admitted, denied, replayed, or replay-missing side-effect tool call
  records an `external.effect` event with effect class, operation fingerprint,
  replay policy, and status.
- File mutation tools write temporary files and replace targets with rDSN
  filesystem rename helpers instead of truncating targets in place.
- Tool results are size-bounded and can spill to artifact files; artifact
  metadata is indexed through the state service boundary.
- Tool result previews and spilled artifact payloads are redacted before storage
  so a read/search/shell response does not persist obvious credential material.
- Shell execution must be explicit, logged, policy-checked, and timeout-bound.

### 11. Policy and safety manager

The policy manager decides what is allowed.

Responsibilities:

- Enforce capability permissions, side-effect gates, budgets, retry limits,
  secret-source rules, and network/provider allow lists.
- Provide policy decisions to coordinator, model gateway, and tool gateway.
- Explain denials in structured form.

rDSN design:

- `rasn.policy` is a service app when dynamic policy is needed; initially it can
  be a config-backed library.
- Reads policy from `[rasn.policy]`, `[rasn.tool.policy.*]`, and
  `[rasn.model.policy.*]`.
- Uses a service-graph-backed state writer for artifact metadata so RPC mode
  routes oversized tool-output references through `rasn.state`.
- Uses rDSN logging for deny/audit events.

Correctness and robustness requirements:

- Default policy is deny for side effects and secrets.
- Policy decisions are deterministic for a request and config snapshot.
- Secrets are represented by references only, never by values.
- `[rasn.policy] redaction_enabled`, `redact_env_names`,
  `redact_literal_values`, and `redact_min_secret_length` define a deterministic
  redaction configuration used by runtime tracing, tool artifact spilling, and
  model-provider boundaries.

### 12. Observability, replay, and failure manager

This component makes runtime behavior diagnosable.

Responsibilities:

- Emit structured events for task lifecycle, routing, RPC calls, provider calls,
  tool calls, retries, failures, nondeterminism, checkpoints, and replay.
- Classify failures: validation, policy, timeout, provider, tool, RPC, state,
  dependency, and internal invariant failure.
- Provide replay hooks and trace queries.

rDSN design:

- `rasn.observability` is a service app over `nucleus_runtime` with
  `serverlet`/`clientlet` query APIs for events, failures, snapshots, and replay
  loading.
- Uses rDSN logging for live diagnosis, JSONL for append-only trace events, and
  the state service for durable snapshot index records.
- Records `filesystem.snapshot` fingerprints for CodePilot read/list/search
  tools and checks them during replay to expose workspace drift before stale
  file context is reused.
- Uses rDSN locks around event buffers.

Correctness and robustness requirements:

- Trace events are append-only and sequence-numbered.
- Snapshot index metadata is written through the service graph so RPC mode uses
  `rasn.state` rather than a hidden local state-store dependency.
- Nondeterministic decisions record enough input metadata to explain replay.
- In-process timing and randomness are drawn from rDSN's pluggable environment
  provider (`dsn_now_ms`/`dsn_random64`), so replay and emulator tooling can seed
  or virtualize them; private wall-clock-seeded generators are avoided.
- Model responses can be replayed from prior `llm.response` events in provider
  order before the model provider is called.
- Tool calls record their arguments and result, and replay mode can return a
  matching recorded `tool.ok`/`tool.error` result without re-invoking the tool.
- Side-effecting tool intents also record `external.effect` ledger events, so
  non-file effects have a stable audit/replay surface even when they are denied
  or replay fails closed.
- Workflow execution records `workflow.node.start` and `workflow.node.finish`
  events; replay mode validates recorded node-start order before executing a
  node so scheduler drift is surfaced before model/tool side effects run.
- Missing side-effect tool replay values fail closed; write and shell commands
  are not re-run while replay mode is active unless a recorded result matches.
- Internal invariant violations use `dassert`; recoverable failures return
  structured errors.

### 12.1 Runtime metrics

Structured trace events make a single run explainable; aggregate metrics make a
fleet operable. rASN therefore reuses rDSN's existing `perf_counter` subsystem
rather than introducing a separate metrics stack.

Responsibilities:

- Maintain cumulative counters per runtime event kind (Prometheus `_total`
  convention) and lazily created per-failure-class counters.
- Maintain task, model, and tool wall-clock latency distributions.
- Expose a point-in-time snapshot renderable as text, Prometheus exposition
  format, or JSON.

rDSN design:

- `metrics_registry` is a process-global facade over rDSN perf counters created
  in the `rasn` section. Cumulative series use `COUNTER_TYPE_NUMBER`; latency
  series use `COUNTER_TYPE_NUMBER_PERCENTILES`, so p50/p95/p99/p999 are computed
  by rDSN's own counter timers instead of bespoke histogram code.
- `nucleus_runtime::record_event` is the single choke point that increments
  counters, so every existing and future event kind is covered without touching
  call sites. Task and model latencies are paired inside the runtime; tool
  latency is timed in `rasn_tool_agent_service::run_tool`, the point where the
  CLI, workflow, RPC-server, and direct-call tool paths converge, with an
  RPC-client transport failure that never reaches the tool agent timed at the
  `rasn_service_graph::run_tool` facade instead.
- The service graph registers a `rasn.metrics` command with rDSN's
  `command_manager`, so a running deployment answers
  `rasn.metrics [text|prometheus|json]` over the same local/remote CLI as
  built-in rDSN commands. CodePilot exposes the same snapshot through
  `observe metrics`.

Correctness and robustness requirements:

- Counter creation asserts an rDSN service node; creation is guarded by a
  node-context check and all updates are null/thread safe, so metric code is a
  no-op (never a crash) in inline, CLI, and node-less contexts.
- `[rasn.metrics] enabled = false` disables all updates without removing call
  sites.
- The command is registered once per process via `std::call_once`, avoiding the
  duplicate-registration assertion in `command_manager`.
- Metric formatting is dependency-light and pure (it lives alongside the
  perf-counter backend in `metrics.cpp` but uses no rDSN headers), so it is unit
  tested independent of the perf-counter backend.

### 12.2 Model circuit breaker

Counters and latency percentiles make a degrading model endpoint *visible*; a
circuit breaker makes the runtime *react* to it. Without one, every request keeps
driving the provider's internal retry loop, so a dead or hanging endpoint turns
into piled-up latency and wasted budget. rASN therefore guards the model gateway
with a per-provider circuit breaker.

Responsibilities:

- Track consecutive failures per model provider and, once a threshold is crossed,
  *open* the breaker so subsequent requests fail fast instead of calling the
  endpoint.
- After a cooldown, admit exactly one *half-open* probe; a successful probe closes
  the breaker, a failed probe reopens it with a fresh cooldown.
- Short-circuited requests return a normal `llm_response{ok=false}` carrying the
  breaker state, so callers and the coordinator handle it like any other failure.

rDSN design:

- The breaker engine (`circuit_breaker` / `circuit_breaker_registry`) is
  intentionally dependency-light, like `metrics.h`: it pulls in no rDSN headers
  and the caller injects the clock as `::dsn_now_ms()`. Because that clock is
  routed through rDSN's pluggable environment provider, breaker timing is
  deterministic under replay and the engine is unit-testable without a live node.
- It is wired into `rasn_llm_agent_service`, where both `complete()` and
  `complete_streaming()` converge on the provider call. The gate sits *after* the
  replay check and *before* the provider, so replayed runs bypass it entirely and
  the provider's own retry loop is skipped when the breaker is open.
- In-process providers that perform no network I/O (the deterministic simulator,
  the workflow service-graph bridge, and test fakes) report `in_process() == true`
  and bypass the breaker, so those runs and tests keep their existing behavior.
  This is deliberately distinct from `describe().local`: loopback HTTP providers
  such as Ollama, llama.cpp, and LM Studio are "local" but still issue curl/HTTP
  requests to an endpoint that can hang or fail, so they are breaker-guarded.
- Two `perf_counter` series — `rasn_model_breaker_open_total` and
  `rasn_model_breaker_short_circuit_total` — flow through the same
  `record_event` choke point as every other metric, so breaker activity shows up
  in `rasn.metrics` and `observe metrics` with no extra plumbing.
- `[rasn.model] circuit_breaker_enabled`, `circuit_breaker_failure_threshold`,
  and `circuit_breaker_open_ms` are read once through `dsn_config` (null-safe
  defaults), and a `rasn.resilience` command registered with `command_manager`
  dumps each provider's live breaker state next to the existing `rasn.metrics`
  command.

### 12.3 Model admission control

The circuit breaker stops calling a *broken* dependency; admission control keeps
the runtime from overwhelming a *healthy* one. A model endpoint that is up but
slow has a finite amount of useful concurrency: past that point, piling on more
in-flight requests only deepens queues, inflates tail latency, and can tip a
healthy provider into failure (at which point the breaker takes over). rASN
therefore fronts each model provider with an admission gate — a concurrency
bulkhead plus graceful backpressure — that sits beside the breaker on the same
gateway.

Responsibilities:

- Cap the number of simultaneous in-flight requests per provider (a *bulkhead*).
  Requests beyond the hard cap are rejected immediately with a normal
  `llm_response{ok=false}`, so excess load fails fast instead of queueing without
  bound.
- As in-flight load climbs past a soft threshold (but below the hard cap), apply a
  short, growing *backpressure* delay before the call, smoothing bursts so the
  provider sees a steadier arrival rate rather than a thundering herd.
- Account every admitted request with an RAII *slot* so capacity is released on
  every exit path — success, provider failure, exception, or an early breaker
  short-circuit.

rDSN design:

- The admission engine (`admission_gate` / `admission_gate_registry`) is
  dependency-light like the breaker and `metrics.h`. The backpressure curve is not
  hand-rolled: it reuses rDSN's own `exp_delay` admission-control primitive
  (`include/dsn/utility/exp_delay.h`) — the same staged-delay mechanism rDSN uses
  to throttle its task queues — seeded so the delay ramps from zero at the soft
  threshold up to a configured ceiling. This keeps the overload policy consistent
  with the host runtime instead of inventing a parallel one. Because `exp_delay`
  works entirely in signed `int`, the configured ceiling is clamped to an int-safe
  bound before it scales the curve, so an out-of-range `max_backpressure_ms`
  degrades to a large-but-valid delay instead of overflowing to a negative value.
- It is wired into `rasn_llm_agent_service` alongside the breaker, and the gate is
  evaluated *before* the breaker's authoritative (probe-consuming) check — only the
  non-mutating open-breaker precheck runs ahead of it. Ordering matters: admitting
  first means a rejected request never perturbs breaker state, and a request the
  breaker later short-circuits still releases its admission slot through the RAII
  guard. The bulkhead *reservation* runs first, but the graceful backpressure
  *delay* is applied only after the breaker also admits — so a request the breaker
  short-circuits neither sleeps nor holds its slot through a sleep. Both gates sit
  *after* the replay check, so replayed runs bypass admission entirely and stay
  deterministic.
- The same `in_process()` predicate that exempts the simulator and other
  no-network providers from the breaker also exempts them from admission control,
  so single-agent CLI runs and deterministic tests are unaffected. With the default
  generous cap, sequential CodePilot usage (in-flight ≈ 1) never delays or rejects;
  the gate only engages under the concurrent fan-out of service mode.
- Two `perf_counter` series — `rasn_model_admission_rejected_total` and
  `rasn_model_admission_delayed_total` — flow through the same `record_event`
  choke point as every other metric. Live per-provider in-flight depth and limits
  are surfaced in the `rasn.resilience` command and the model provider summary.
- `[rasn.model] admission_enabled`, `max_concurrent_requests`,
  `soft_concurrent_requests`, and `max_backpressure_ms` are read once through
  `dsn_config` with null-safe defaults.

### 12.4 Model rate limiter

The breaker reacts to a *broken* dependency and admission control protects a
*healthy* one from concurrency overload; the rate limiter governs the third
classic dimension — *throughput*. Hosted model APIs publish requests-per-minute
and tokens-per-minute quotas, and exceeding them does not slow the provider down
gracefully: it returns HTTP 429s that burn the retry budget, and on metered
endpoints it costs money. A concurrency bulkhead does not prevent this — a single
caller making rapid sequential requests stays at in-flight ≈ 1 yet can still blow
through a per-minute quota. rASN therefore fronts each model provider with a
client-side **token-bucket rate limiter**, the throughput counterpart to the
breaker (failure) and admission gate (concurrency) on the same gateway.

Responsibilities:

- Pace outbound calls to a configured sustained rate (requests/minute). The bucket
  starts full, so an idle dependency can still absorb a burst up to its capacity.
- When a caller is slightly ahead of the rate, apply a short *pacing* delay
  (reserving the next token) instead of failing — smoothing traffic to the quota.
- When the projected wait would exceed a bound (`rate_limit_max_wait_ms`), reject
  the request fast with a normal `llm_response{ok=false}` rather than blocking a
  worker for a long time. Setting the bound to zero turns the limiter into a pure
  reject-when-over-rate gate.

rDSN design:

- The engine (`rate_limiter` / `rate_limiter_registry`) is dependency-light like
  the breaker, the admission gate, and `metrics.h`: the header pulls in no rDSN or
  thrift types, and the current time is *injected* as a millisecond value rather
  than read from a clock. The caller passes `::dsn_now_ms()`, so token refill flows
  through rDSN's pluggable environment provider — making it deterministic under
  replay and unit-testable without a live service node, exactly like the breaker's
  cooldown clock. Token refill is hardened against a non-monotonic injected clock: a
  reading at or before the last refill timestamp never removes tokens *and* never
  moves the refill baseline backward. Holding the baseline at its high-water mark
  matters because moving it back would let a later-but-still-stale reading (one below
  the original mark) measure a positive elapsed interval and refill prematurely;
  refill resumes only once the clock advances past the mark.
- It is wired into `rasn_llm_agent_service` beside the breaker and admission gate.
  Ordering: a non-mutating **open-breaker precheck** runs *first*, so a request to a
  dependency whose breaker is already open (and still cooling down) fast-fails ahead
  of any admission or rate rejection — a broken dependency wins over an overload or
  quota signal, and no admission slot or rate token is wasted on it. The precheck
  only *reads* breaker state; it does not consume the one-shot half-open probe.
  Otherwise admission *reserves* a slot, the rate limiter *acquires* a token, and the
  authoritative (mutating) breaker check is consulted **last**, because that check
  consumes a one-shot half-open probe that must be paired with a result report.
  Putting both reject-capable gates (admission, rate) ahead of that authoritative
  breaker check guarantees a request they reject never strands a half-open probe
  (the precheck never admits a probe, so it is safe ahead of them). The pacing delay,
  like admission backpressure, is applied only *after* the breaker also admits, and
  the two delays are coalesced into a single wait (the larger of the two) so they
  never stack into additive over-delay. All gates sit *after* the replay check, so
  replayed runs bypass rate limiting and stay deterministic.
- If the authoritative breaker check *does* short-circuit a request that has already
  acquired a rate token — the half-open-probe-busy case, or a race where the breaker
  opened between the precheck and the authoritative check — that token is **refunded**
  before the fast-fail returns. A breaker short-circuit therefore never silently
  drains the provider's quota or delays its eventual recovery probe. The admission
  slot is returned the same way, by its RAII guard. The refund is a no-op when the
  limiter is disabled or unlimited (no token was taken) and is clamped to burst
  capacity so it can never inflate the bucket past its configured size.
- The same `in_process()` predicate that exempts the simulator and other
  no-network providers from the breaker and admission control also exempts them
  here. The limiter is **disabled by default** (`requests_per_min = 0` = unlimited),
  so existing single-agent CLI runs and deterministic tests are byte-for-byte
  unchanged; an operator opts in by setting a rate when pointing rASN at a metered
  endpoint.
- Two `perf_counter` series — `rasn_model_rate_limited_total` (rejected) and
  `rasn_model_rate_delayed_total` (paced) — flow through the same `record_event`
  choke point as every other metric. Per-provider rate, burst, and available
  tokens are surfaced in the `rasn.resilience` command and the model provider
  summary.
- `[rasn.model] rate_limit_enabled`, `rate_limit_requests_per_min`,
  `rate_limit_burst`, and `rate_limit_max_wait_ms` are read once through
  `dsn_config` with null-safe defaults.

### 12.5 Tool gateway admission and rate controls

The same overload dimensions apply to tools. A single model response can request a
burst of `read`/`search` calls, or an application adapter can expose shell/network
tools backed by local processes or remote services. Without an explicit governor,
the tool gateway either serializes too much behind an implementation mutex or lets
concurrent calls fan out without a product-level budget. rASN therefore reuses the
existing admission and rate engines for tool execution, keyed by tool name.

Responsibilities:

- Bound simultaneous in-flight invocations per tool name, fast-failing excess load
  with a normal failed `tool_result`.
- Smooth bursts with the same `exp_delay`-backed admission curve used for model
  providers and rDSN task queues.
- Optionally pace a tool under an operator-defined requests-per-minute quota using
  the shared token-bucket limiter.
- Preserve replay and policy semantics: replay hits, replay-missing side-effect
  failures, and policy denials occur before the gates and never consume tool
  capacity.

rDSN design:

- `rasn_tool_agent_service` takes a shared pointer to the registered
  `agent_tool_provider` under `zlock`, then releases the lock before validation,
  policy, gates, and provider execution. The provider remains alive for the call,
  but unrelated tools are no longer serialized by registration state.
- `admission_gate_registry` and `rate_limiter_registry` are reused directly. Tool
  admission reads `[rasn.tool] admission_enabled`, `max_concurrent_requests`,
  `soft_concurrent_requests`, and `max_backpressure_ms`; tool rate limiting reads
  `[rasn.tool] rate_limit_*`.
- Backpressure and rate pacing are coalesced into a single sleep (the larger of
  the two delays), matching the model gateway and avoiding additive over-delay.
- `rasn_tool_admission_rejected_total`,
  `rasn_tool_admission_delayed_total`, `rasn_tool_rate_limited_total`, and
  `rasn_tool_rate_delayed_total` are emitted through `record_event` and rDSN
  `perf_counter` counters. Live per-tool in-flight/rate state is shown by
  `rasn.resilience` and CodePilot `observe resilience`.
- The rate limiter is unlimited by default (`rate_limit_requests_per_min = 0`) and
  the admission defaults are generous, so normal sequential CLI usage remains
  unchanged while service-mode fan-out becomes explicit and observable.

### 12.6 Remote-agent dispatch resilience

Service mode introduces another dependency boundary: the coordinator resolves a
capability through the registry and then calls the selected agent over
`RPC_RASN_AGENT_INVOKE`. That remote agent may be a built-in service role or a
future custom agent registered dynamically. Without a gateway governor, a slow
or unhealthy agent can consume coordinator threads, request budgets, and retry
budget even though model providers and tools are individually guarded. rASN now
guards the coordinator-to-agent RPC gateway per remote `agent_id`.

Responsibilities:

- Fast-fail repeated retryable remote-agent failures with a per-agent circuit
  breaker, including the same open/half-open/closed state machine used for model
  providers.
- Cap concurrent coordinator dispatches to each remote agent with an admission
  bulkhead and `exp_delay`-based backpressure.
- Optionally pace requests to each remote agent with the shared token-bucket rate
  limiter.
- Treat only retryable dependency outcomes as breaker failures: RPC transport
  errors and retryable remote responses count, while deterministic application
  outcomes such as policy denials, validation errors, and tool failures do not
  poison the remote service.

rDSN design:

- `rasn_coordinator_service` reuses `circuit_breaker_registry`,
  `admission_gate_registry`, and `rate_limiter_registry`, keyed by the registry
  descriptor's `agent_id` (falling back to endpoint text only for malformed
  descriptors). The clock again comes from `::dsn_now_ms()`, preserving
  replay-friendly behavior and unit-testability of the engines.
- Guard ordering mirrors the model gateway: a non-mutating open-breaker precheck
  wins first; admission and rate rejection happen before the authoritative
  breaker probe; the rate token is refunded if that final breaker check
  short-circuits; admission and rate delays are coalesced into one sleep.
- The guards are applied only on the RPC-client dispatch path. Inline standalone
  execution still calls the in-process model/tool services directly, so the
  single-process CLI keeps its existing behavior.
- Six `perf_counter` series flow through `record_event`:
  `rasn_remote_agent_breaker_open_total`,
  `rasn_remote_agent_breaker_short_circuit_total`,
  `rasn_remote_agent_admission_rejected_total`,
  `rasn_remote_agent_admission_delayed_total`,
  `rasn_remote_agent_rate_limited_total`, and
  `rasn_remote_agent_rate_delayed_total`. Live per-agent state is included in
  `rasn.resilience` and CodePilot `observe resilience`.
- `[rasn.remote_agent] circuit_breaker_*`, `admission_*`,
  `max_concurrent_requests`, `soft_concurrent_requests`, `max_backpressure_ms`,
  and `rate_limit_*` are read once through `dsn_config` with null-safe defaults.
  The default admission cap is generous (`64` hard, `32` soft, `200ms`
  backpressure) and the rate limiter is unlimited (`requests_per_min = 0`).

### 13. Service-mode integration self-test

The service-mode integration layer turns rASN correctness assumptions into a
repeatable runtime check.

Responsibilities:

- Exercise model, tool, state, workflow, registry, and observability calls through
  the same `rasn_service_graph` used by applications.
- Run inline in direct CLI mode and through typed rDSN RPC when the full app graph
  is active.
- Provide a small service config that starts the rDSN apps and runs the self-test
  as the `rasn.codepilot` gateway app.

rDSN design:

- `codepilot selftest` remains an application command, but it uses only generic
  rASN service APIs.
- In service mode, `rasn.coordinator` enables RPC clients before `rasn.codepilot`
  executes, so self-test calls cross `RPC_RASN_*` task codes.
- Shared graph lifecycle uses reference-counted app ownership, so stopping one
  service wrapper cannot prematurely tear down sibling services.
- CodePilot command execution uses scoped lifecycle ownership rather than eager
  constructor startup, so service-mode app construction does not start the graph
  before configured RPC clients and app owners are ready.
- The service-mode gateway performs non-mutating readiness probes across state,
  registry, coordinator, model health, tool, workflow validation, and
  observability before dispatching commands, and returns per-boundary errors
  when startup is incomplete.
- The smoke config lives under `examples/` and should not replace production
  config.

Correctness and robustness requirements:

- Every failed step reports the component boundary that failed.
- State/checkpoint checks must use namespaced keys.
- The self-test must not require a real network model provider; simulator is the
  default.
- Lifecycle checks must prove the graph remains started until the final owner
  releases it.
- Adapter construction must be side-effect light: installing a tool provider
  must not start the graph.

### 14. Provider adapter profiles

Provider adapters are profile-driven wrappers around a common HTTP transport.

Responsibilities:

- Provide first-class profiles for simulator, GitHub Copilot-compatible,
  Ollama, llama.cpp, LM Studio, and generic OpenAI-compatible APIs.
- Keep provider-specific endpoints, payload formats, headers, token environment
  fallbacks, and timeout settings in config.
- Avoid leaking tokens through process command lines, logs, traces, or source
  files.

rDSN design:

- Provider metadata is exposed through `rasn.llm.agent` and
  `RPC_RASN_MODEL_*`.
- `[rasn.model]` is the canonical config section; `[rasn.llm]` remains a
  compatibility alias.
- Provider-specific keys such as `copilot_endpoint`, `ollama_model`,
  `llama_cpp_endpoint`, and `lmstudio_model` override shared defaults.
- Temporary provider request and curl config files are created under an OS temp
  `rasn-provider` directory by default. A non-empty `[rasn.runtime] temp_dir`
  can override this for controlled test environments.

Correctness and robustness requirements:

- Network providers use bounded connect/request timeouts.
- Token lookup supports environment-variable lists and token commands.
- Local providers are marked local; remote providers are explicit.
- Prompts, context entries, provider responses, runtime events, and tool outputs
  pass through the shared redaction boundary before they are persisted or handed
  to providers. The boundary removes configured exact secret values, bearer
  tokens, and common key/value fields such as `password` and `api_key`.
- Bearer tokens are not written under the project tree by default; future
  hardening should remove token materialization entirely, add restrictive file
  permissions where portable, or integrate an OS/secret-vault credential handle.

### 15. Durable state journal

The state subsystem is a memory store plus append-only durability layer.

Responsibilities:

- Append every committed state mutation to a journal before making it visible in
  memory.
- Write compact checkpoints atomically and remove covered journal entries.
- Recover from checkpoint plus journal, or from journal alone if a checkpoint has
  not yet been created.

rDSN design:

- State records remain guarded by `dsn::service::zlock`.
- Checkpoint paths are config-driven under `[rasn.state]`; journal writes use a
  single configured/default journal so explicit checkpoints and default
  checkpoints share one recovery stream.
- Remote checkpoint import uses `dsn::file::copy_remote_files` from
  `dsn.tools.nfs`, waits with a bounded timeout, stages files locally, then moves
  them into the configured recovery paths before normal validation and replay.
- Filesystem operations use `dsn::utils::filesystem`.

Correctness and robustness requirements:

- A state write fails explicitly if the journal cannot be appended.
- Conditional state writes compare against the currently visible sequence while
  holding the store lock, then append the committed record to the journal before
  publishing it.
- Recovery validates headers, schema versions, namespaces, and encoding before
  swapping in recovered state.
- Checkpoint compaction is serialized with writes.

### 16. Workflow optimizer

The workflow compiler estimates and explains execution tradeoffs.

Responsibilities:

- Parse optional `cost_hint`, `latency_ms`, and `reliability` node metadata.
- Order ready nodes deterministically by latency, cost, and reliability penalty.
- Report stages, maximum parallelism, critical-path latency, cost units, and
  minimum reliability in compiled plans.

rDSN design:

- Optimization remains deterministic and local to `workflow_graph`.
- Execution still uses coordinator/model/tool services; optimization never
  bypasses policy or dependency checks.
- Future provider selection can reuse the same node annotations.

Correctness and robustness requirements:

- Dependencies and side-effect order are preserved.
- Hints are validated before execution; invalid hints cause no side effects.
- Compiled plans explain optimizer decisions in stable text form.

### 17. Observability diagnosis tooling

Observability includes human-facing diagnosis helpers in addition to raw queries.

Responsibilities:

- Render trace timelines from ordered runtime events.
- Summarize event kinds, failures, retries, replay misses, and nondeterministic
  points.
- Provide next-step recommendations for replay gaps and failure triage.

rDSN design:

- `rasn.observability` continues to expose typed event/failure/snapshot RPCs.
- Formatting helpers live with the generic observability component.
- CodePilot only exposes those helpers through CLI commands.

Correctness and robustness requirements:

- Diagnosis is derived from structured event fields, not log text scraping.
- Trace filtering is explicit by trace id.
- Missing replay values are surfaced as failures, never silently ignored.
- Recorded model responses are reused before contacting providers when replay is
  enabled.
- Missing side-effect tool replay values fail closed so replay does not
  accidentally repeat writes or shell commands.
- Workflow scheduler replay mismatches are reported as `replay.miss` failures
  before dispatching the mismatched node.
- Filesystem snapshot mismatches are reported as `replay.miss` failures before
  read/list/search tools consume changed local files.

### 18. CLI UX, packaging, and tutorials

The application layer should make robust-agent concepts easy to exercise without
requiring users to understand every internal service.

Responsibilities:

- Keep command help aligned with implemented features.
- Provide tutorial workflows for generic, state/checkpoint, optimizer, and
  service-mode RPC scenarios.
- Document build, PATH, provider, state, trace, replay, and troubleshooting
  steps in one place.

rDSN design:

- Packaging remains source-tree local: `config.ini` is copied beside the built
  executable by CMake, and examples are stored under `examples/`.
- rASN builds as a reusable `rasn` static library containing the engine, while
  CodePilot (`codepilot/`, including `codepilot/main.cpp`) and the
  `rasn.unit_tests` binary are thin executables that link it. This keeps the
  platform reusable by other applications and avoids recompiling the engine per
  consumer.
- The CLI is an adapter over generic rASN APIs and should not introduce hidden
  direct provider/tool paths.
- `codepilot eval` runs built-in or file-backed task suites and can invoke an
  external CLI command template with `{prompt}` for comparison experiments.

Correctness and robustness requirements:

- Tutorial examples must run with the simulator by default.
- Provider examples must reference token variables or commands, never literal
  secrets.
- Service-mode examples should be explicit that the rDSN runtime remains running
  after the smoke command completes.

## Application adapter rule

Application adapters such as CodePilot may provide:

- User interface and CLI commands.
- Application-specific skills and prompts.
- Application-specific local or remote tool providers.
- Application-specific config defaults.

Application adapters must not own:

- Generic task schema.
- Generic agent lifecycle.
- Generic registry.
- Generic orchestration.
- Generic policy/failure semantics.
- Generic state/checkpoint format.

## Current prototype mapping

| Existing file | Target direction |
| --- | --- |
| `agent_messages.h` | Replace narrow LLM/tool messages with generic task/message schema. |
| `agent_services.*` | Split into generic runtime, registry, coordinator, model agent, tool agent, and clients. |
| `rasn.code.definition.h` | Add generic RPC task codes and keep specialized compatibility codes during migration. |
| `rasn_core.*` | Become observability/replay foundation, backed by state service later. |
| `metrics.*` | Aggregate runtime metrics over rDSN `perf_counter`; surfaced via `observe metrics` and the `rasn.metrics` command. |
| `workflow.*` | Promote to workflow graph plus compiler/executor service. |
| `llm_provider.*` | Remain provider adapters behind generic model gateway. |
| `agent_tools.h` | Become generic tool provider interface; CodePilot tools stay in `codepilot/`. |
| `codepilot/*` | Application adapter using generic rASN APIs. |

## Current product limitations

rASN is now usable as a prototype nucleus for building and testing robust agent
systems, but it should not be described as a production-complete platform. The
remaining limitations are:

- **State availability:** the state service has checkpoints, journals,
  conditional writes, workflow leases, optional local replica mirroring, and
  optional rDSN NFS import, but it is not quorum-replicated and does not yet use
  an external HA database or replicated SKV backend.
- **Tool isolation:** local tools are default-deny, policy-gated,
  approval-gated, allowlist-aware, workspace-rooted, timeout-bound, and can be
  routed through a configured container command wrapper. rASN still lacks a
  hardened container orchestrator with image, mount, network, and lifecycle
  policy.
- **Replay fidelity:** replay covers model responses, matching tool results,
  workflow scheduling, filesystem snapshots, and side-effect intent records via
  `external.effect`, and in-process timing and randomness flow through rDSN's
  pluggable environment provider (`dsn_now_ms`/`dsn_random64`) so tooling can
  virtualize or seed them; it does not yet virtualize arbitrary external
  services, OS-level clocks, network state, process environments, or
  provider-side nondeterminism.
- **Deployment validation:** service-mode RPC, URI/host endpoint configuration,
  registry heartbeats, and active lease cleanup are implemented, but
  multi-process and cluster deployment tests remain limited.
- **Credential storage:** model credentials can be referenced with `env:`,
  `file:`, and `cmd:` handles and are protected by redaction, but vault-backed
  or OS-backed secret providers are not integrated.
- **SDK packaging:** C++/TypeScript/Python contracts and RPC-client source can be
  generated, but packaged SDKs and concrete TypeScript/Python transports remain
  product work.
- **Evaluation evidence:** unit tests, self-tests, service smokes, schema
  smokes, report builds, and a small evaluation harness exist, but large
  benchmark suites and user studies for debugging effectiveness are still
  future work.
- **Observability metrics:** runtime counters and task/model/tool latency
  percentiles are exported through rDSN `perf_counter` and rendered as
  text/Prometheus/JSON, but rASN does not bundle a scrape gateway, a retention
  store, or prebuilt dashboards, and latency percentiles depend on rDSN's
  periodic counter timers.
- **Overload and dependency isolation:** the model gateway and remote-agent RPC
  gateway have the full per-dependency failure/concurrency/throughput trio:
  circuit breaker, admission gate (concurrency bulkhead plus `exp_delay`-based
  backpressure), and token-bucket rate limiter. The tool gateway reuses the
  admission and rate engines per tool name, after replay and policy checks, so
  tool fan-out is bounded and observable. Remaining gaps: the limits are
  per-dependency rather than a single process-wide budget; rate limiting governs
  request count but not token/cost budgets; and in RPC-client mode model/tool
  resilience state lives on the serving node while remote-agent state lives on the
  coordinator.
