# rASN generic multi-agent implementation plan

This plan tracks the work required to refactor the current rASN prototype into a
generic rDSN-native multi-agent system. The status markers are intentionally
simple so they can be updated in source control:

- `[ ]` not started
- `[~]` in progress
- `[x]` complete

## Ground rules

- Use public rDSN C/C++ APIs from `include/dsn/c` and `include/dsn/cpp`.
- Follow patterns from `src/plugins/apps.echo`, `src/plugins/apps.skv`, and
  `src/plugins_ext`.
- Prefer `service_app`, `serverlet`, `clientlet`, typed task codes, rDSN config,
  rDSN logging, rDSN locks, and rDSN filesystem utilities.
- Keep CodePilot-specific logic under `apps/codepilot/`.
- Every component must define correctness invariants and validation checks before
  it is considered complete.
- Every behavior-changing phase must pass build and direct/service-mode smoke
  checks.

## Target source layout

```text
src/plugins/rasn/
  README.md
  docs/
    DESIGN.md
    IMPLEMENTATION_PLAN.md
    report/
  rasn.code.definition.h
  agent_types.h/.cpp
  agent_messages.h
  agent_runtime.h/.cpp
  agent_registry.h/.cpp
  agent_clients.h/.cpp
  coordinator_service.h/.cpp
  state_service.h/.cpp
  workflow.h/.cpp
  workflow_service.h/.cpp
  policy_manager.h/.cpp
  observability.h/.cpp
  model_agent.h/.cpp
  tool_agent.h/.cpp
  llm_provider.h/.cpp
  agent_tools.h/.cpp
  apps/
    codepilot/
```

The exact file split can be adjusted during implementation, but each generic
component should remain independently understandable and testable.

## Phase 0: design and planning

Status: `[x]`

- [x] Study relevant public rDSN APIs:
  - `service_app`
  - `serverlet`
  - `clientlet`
  - `serialization`
  - `api_utilities`
  - `zlocks`
  - `utils::filesystem`
- [x] Study relevant examples:
  - `apps.echo`
  - `apps.skv`
  - `rDSN.dist.deployment`
- [x] Write generic architecture design in `docs/DESIGN.md`.
- [x] Write this implementation plan.
- [ ] Review design names and component boundaries before large code movement.

Validation:

- [ ] Confirm docs are internally consistent with current source layout.
- [ ] Confirm every target component has design, files, invariants, and tests.

## Phase 1: generic task and message model

Status: `[x]`

Goal: Replace narrow LLM/tool request types with generic rASN messages.

Files:

- `agent_types.h`
- `agent_messages.h`
- `agent_messages.cpp` if non-inline helpers are needed
- `rasn.code.definition.h`

Work items:

- [x] Define `agent_error_code` string constants or enum-like safe strings.
- [x] Define `agent_error` with class, code, message, retryable flag, and
  rDSN error metadata.
- [x] Define `agent_artifact` with kind, uri/path, mime/type, size, and digest.
- [x] Define `agent_context_entry` for prompt, file, trace, tool result, state
  reference, and artifact reference.
- [x] Define `agent_capability` with name, input type, output type, side-effect
  class, cost hint, latency hint, and reliability hint.
- [x] Define `agent_descriptor` with id, role, address, health, version, and
  capabilities.
- [x] Define `agent_request` with request id, parent id, trace id, capability,
  input, context, timeout, retry budget, policy labels, and replay mode.
- [x] Define `agent_response` with request id, success flag, output, artifacts,
  error, trace summary, and state references.
- [x] Implement `marshall` and `unmarshall` for every type using
  `binary_writer` and `binary_reader`.
- [x] Keep compatibility adapters from current `agent_completion_request` and
  `agent_tool_request` until CodePilot migration is complete.

Correctness checks:

- [x] Reject missing request id, trace id, or capability at service boundaries.
- [x] Ensure response cannot contain both success output and fatal error.
- [x] Include schema version in every top-level message.

Validation:

- [x] Build plugin.
- [x] Add direct smoke for simulator ask through compatibility adapter.
- [ ] Add service-mode smoke for typed generic invoke once RPC layer exists.

## Phase 2: generic agent runtime

Status: `[x]`

Goal: Create a common runtime abstraction for all rASN agents.

Files:

- `agent_runtime.h`
- `agent_runtime.cpp`
- `model_agent.h/.cpp`
- `tool_agent.h/.cpp`

Work items:

- [x] Define `agent_runtime` base with identity, lifecycle, capabilities,
  request validation, tracing hooks, and config prefix.
- [ ] Define `agent_service_app<TAgent>` helper if it reduces repeated app
  wrappers without hiding rDSN lifecycle behavior.
- [x] Move shared start/stop/idempotency logic out of current LLM/tool services.
- [ ] Add config helpers for `[rasn.agent.<role>]`.
- [x] Add common request validation and structured rejection.
- [x] Add common logging in start, stop, reject, and failure paths.

Correctness checks:

- [ ] Start validates config before opening RPC handlers.
- [x] Stop unregisters handlers and is safe if called more than once.
- [x] Mutable runtime state uses `zlock` or is immutable after start.

Validation:

- [x] Build plugin.
- [x] Direct-mode CodePilot still starts with no app services.
- [x] Service-mode apps start and stop cleanly.

## Phase 3: generic RPC and clients

Status: `[x]`

Goal: Unify agent RPC around generic describe/invoke operations.

Files:

- `rasn.code.definition.h`
- `agent_clients.h`
- `agent_clients.cpp`
- `agent_runtime.h/.cpp`

New task codes:

- [x] `RPC_RASN_AGENT_DESCRIBE`
- [x] `RPC_RASN_AGENT_INVOKE`
- [x] `RPC_RASN_AGENT_CANCEL`
- [x] `RPC_RASN_AGENT_HEARTBEAT`
- [x] `RPC_RASN_AGENT_QUERY`

Work items:

- [x] Implement generic describe/invoke handlers on current serverlets.
- [x] Implement `agent_client` with sync describe/invoke/cancel/heartbeat/query
  calls.
- [x] Convert RPC failures to `agent_error`.
- [ ] Add configured default timeouts from `[rasn.rpc]`.
- [x] Keep old LLM/tool task codes during migration only.

Correctness checks:

- [ ] Every RPC has a timeout.
- [x] Every handler catches invalid input before side effects.
- [x] Handler registration and unregistration are paired.

Validation:

- [x] Service-mode generic describe returns model/tool descriptors.
- [x] Service-mode generic invoke can call simulator model agent.
- [x] Service-mode generic invoke can call read-only tool agent.

## Phase 4: agent registry

Status: `[x]`

Goal: Add a registry service for capabilities and routing metadata.

Files:

- `agent_registry.h`
- `agent_registry.cpp`
- `agent_clients.h/.cpp`
- `config.ini`

Task codes:

- [x] `RPC_RASN_REGISTRY_REGISTER`
- [x] `RPC_RASN_REGISTRY_UNREGISTER`
- [x] `RPC_RASN_REGISTRY_QUERY`
- [x] `RPC_RASN_REGISTRY_LIST`
- [x] `RPC_RASN_REGISTRY_HEARTBEAT`

Work items:

- [x] Implement `rasn.registry` service app.
- [x] Load static agents from `[rasn.agent.*]` config sections.
- [x] Store descriptors by agent id and by capability.
- [x] Add duplicate handling and version checks.
- [x] Add health heartbeat operation.
- [x] Add registry client wrapper.

Correctness checks:

- [x] Duplicate ids are rejected unless explicitly updated.
- [x] Query returns only healthy entries by default.
- [x] Registry state is protected by `zlock`.

Validation:

- [x] Static model/tool agents appear in registry.
- [x] Coordinator can query by capability.
- [x] Coordinator can query registry through service RPC in service mode.
- [x] Build and service-mode startup pass.

## Phase 5: coordinator/orchestrator upgrade

Status: `[x]`

Goal: Make the coordinator route generic tasks by capability instead of calling
hardwired LLM/tool services.

Files:

- `coordinator_service.h`
- `coordinator_service.cpp`
- `agent_services.*` migration or removal

Work items:

- [x] Move coordinator routing helpers out of monolithic `agent_services.*`.
- [x] Add coordinator request validation.
- [x] Query registry for route candidates.
- [ ] Call policy manager before dispatch. Deferred until Phase 9 introduces policy manager.
- [x] Dispatch through `agent_client`.
- [x] Record routing decision as trace event.
- [x] Merge or pass through responses.
- [x] Preserve compatibility methods used by CodePilot while migrating.

Correctness checks:

- [x] Missing capability returns structured error.
- [x] Multiple candidates are selected deterministically unless policy enables
  nondeterministic routing and trace records the choice.
- [x] Downstream RPC errors preserve rDSN error code and route metadata.

Validation:

- [x] CodePilot ask runs through generic coordinator to model agent.
- [x] CodePilot tool command runs through generic coordinator to tool agent.
- [x] Service-mode smoke covers coordinator, registry, and agent RPCs.

## Phase 6: state and checkpoint service

Status: `[x]`

Goal: Own durable task, workflow, trace, and replay state.

Files:

- `state_service.h`
- `state_service.cpp`
- `observability.h/.cpp`
- `config.ini`

Task codes:

- [x] `RPC_RASN_STATE_PUT`
- [x] `RPC_RASN_STATE_GET`
- [x] `RPC_RASN_STATE_QUERY`
- [x] `RPC_RASN_STATE_CHECKPOINT`
- [x] `RPC_RASN_STATE_RECOVER`

Work items:

- [x] Implement in-memory map guarded by `zlock`.
- [x] Add checkpoint directory config.
- [x] Implement atomic logical state update.
- [x] Implement checkpoint write and recovery using rDSN filesystem utilities.
- [x] Add schema version and last event sequence.
- [x] Keep secret references separate from values.

Correctness checks:

- [x] Recovery validates schema version.
- [x] Corrupt checkpoint fails explicitly.
- [x] State keys are namespaced by trace/workflow/request id.

Validation:

- [x] State put/get/query smoke.
- [x] Checkpoint and recover smoke.
- [x] Existing trace/replay behavior still works.

## Phase 7: workflow service and compiler

Status: `[x]`

Goal: Promote workflow parsing and execution into a generic rDSN service.

Files:

- `workflow.h`
- `workflow.cpp`
- `workflow_service.h`
- `workflow_service.cpp`

Task codes:

- [x] `RPC_RASN_WORKFLOW_VALIDATE`
- [x] `RPC_RASN_WORKFLOW_COMPILE`
- [x] `RPC_RASN_WORKFLOW_START`
- [x] `RPC_RASN_WORKFLOW_QUERY`
- [x] `RPC_RASN_WORKFLOW_CANCEL`

Work items:

- [x] Extend workflow spec with capability, policy, budget, state, and artifact
  fields.
- [x] Validate graph and report all obvious errors.
- [x] Compile workflow to executable plan using registry snapshot.
- [x] Execute ready nodes through coordinator.
- [x] Persist progress to state service.
- [ ] Support resume from checkpointed workflow state.

Correctness checks:

- [x] Invalid graph causes no side effects.
- [x] Execution order is stable for the same graph and registry snapshot.
- [x] Failed dependencies block downstream nodes with explicit state.

Validation:

- [x] Workflow validate smoke.
- [x] Workflow compile smoke.
- [x] Workflow run smoke with simulator and read-only tool.

## Phase 8: model gateway migration

Status: `[x]`

Goal: Recast current LLM provider service as a generic model agent.

Files:

- `model_agent.h`
- `model_agent.cpp`
- `llm_provider.h`
- `llm_provider.cpp`

Work items:

- [x] Implement model agent capability descriptor.
- [x] Convert `agent_request` to `llm_request`.
- [x] Convert `llm_response` to `agent_response`.
- [x] Preserve simulator, Ollama, llama.cpp, LM Studio, Copilot-compatible, and
  OpenAI-compatible providers.
- [x] Move non-secret settings under `[rasn.model]` or keep `[rasn.llm]` as a
  compatibility alias.
- [x] Keep token handling as references or commands only.

Correctness checks:

- [x] Token values are not logged, traced, or stored in config docs.
- [x] Provider process/HTTP failures return structured error.
- [x] Simulator nondeterminism is replayable.

Validation:

- [x] Simulator direct and service-mode ask.
- [x] Provider summary command.
- [x] Curl temp file cleanup still uses rDSN filesystem utilities.

## Phase 9: tool gateway and policy manager

Status: `[x]`

Goal: Enforce side-effect policy through generic tool and policy components.

Files:

- `tool_agent.h`
- `tool_agent.cpp`
- `policy_manager.h`
- `policy_manager.cpp`
- `agent_tools.h/.cpp`
- `apps/codepilot/local_tools.*`

Work items:

- [x] Define side-effect classes.
- [x] Add policy request/decision structs.
- [x] Implement config-backed policy manager first.
- [x] Add optional `rasn.policy` service app later if dynamic policy is needed. Not needed yet; config-backed policy is the current implementation.
- [x] Make tool agent call policy before invoking provider.
- [x] Move CodePilot write/shell gates to policy decisions.
- [x] Bound tool output size and spill large results to state/artifact files.

Correctness checks:

- [x] Read-only tools are allowed only when target paths are valid.
- [x] Write/shell/network tools are denied by default.
- [x] Denials are structured and logged.

Validation:

- [x] Read-only list/read/search tools pass.
- [x] Write and shell are denied by default.
- [x] Write and shell work only after explicit config enablement.

## Phase 10: observability, replay, and failure manager

Status: `[x]`

Goal: Make traces, failures, retries, and replay generic.

Files:

- `observability.h`
- `observability.cpp`
- `rasn_core.h`
- `rasn_core.cpp`

Work items:

- [x] Define structured event schema with sequence number and schema version.
- [x] Define failure classes as structured runtime event metadata.
- [x] Record lifecycle, routing, tool, retry, policy, replay, and
  nondeterminism events; provider/state/workflow paths continue to use the
  common runtime trace hooks where they cross the service graph.
- [x] Add replay lookup by nondeterminism key from JSONL traces.
- [x] Add retry metadata while keeping retry execution in coordinator/workflow.
- [x] Add trace query through `rasn.observability` service.

Correctness checks:

- [x] Trace append is protected by rDSN lock.
- [x] Event sequence is monotonic per trace.
- [x] Replay mismatch is explicit, not silently ignored.

Validation:

- [x] Trace file smoke.
- [x] Replay smoke.
- [x] Failure classification smoke for denied tool.

## Phase 11: CodePilot migration

Status: `[x]`

Goal: Make CodePilot an application adapter over generic rASN.

Files:

- `apps/codepilot/codepilot_app.*`
- `apps/codepilot/local_tools.*`
- `apps/codepilot/skills.*`

Work items:

- [x] Replace direct service-graph calls with coordinator client calls.
- [x] Register CodePilot local tools through generic tool provider interface.
- [x] Register CodePilot skills as application metadata, not core agent
  concepts.
- [x] Keep direct CLI mode by initializing rDSN runtime without starting service
  apps.
- [x] Update README commands and config docs.

Correctness checks:

- [x] CodePilot owns no generic task, registry, policy, or state schema.
- [x] CLI commands continue to return clear user-facing errors.
- [x] Credential safety remains unchanged.

Validation:

- [x] Direct `ask`, `plan`, `tools`, `tool list`, `tool search`.
- [x] Service-mode `/ask`, `/tools`, `/tool list`, `/exit`.
- [x] Real project smoke on Boost, llama.cpp, and rDSN.dist.service paths.

## Phase 12: examples and validation

Status: `[x]`

Goal: Provide repeatable examples for the generic runtime.

Files:

- `README.md`
- `config.ini`
- optional sample workflow files under a source-tree examples directory if
  desired later

Work items:

- [x] Add minimal generic multi-agent example.
- [x] Add workflow example using model and read-only tool capabilities.
- [x] Add service-mode topology documentation.
- [x] Add troubleshooting section for config, ports, provider endpoints, and
  credentials.

Validation:

- [x] Full plugin build with `run.cmd build --build_plugins`.
- [x] Direct-mode smoke.
- [x] Service-mode smoke.
- [x] State checkpoint/recovery smoke once implemented.

## Phase 13: RPC hardening and compatibility cleanup

Status: `[x]`

Goal: Remove migration-only RPC surfaces and ensure generic RPC calls have
configured timeouts.

Files:

- `rasn.code.definition.h`
- `agent_clients.*`
- `agent_services.*`
- `coordinator_service.*`
- `config.ini`
- `README.md`

Work items:

- [x] Add `[rasn.rpc] timeout_ms` config and `default_rpc_timeout()`.
- [x] Pass configured timeouts through service-graph and router RPC calls.
- [x] Remove migration-only LLM/tool/coordinator compatibility RPC task codes,
  handlers, and clients.
- [x] Route tool summary through generic `tool.describe` agent invoke in service
  mode.

Correctness checks:

- [x] Generic `RPC_RASN_AGENT_DESCRIBE` and `RPC_RASN_AGENT_INVOKE` remain the
  only model/tool/coordinator invoke surface.
- [x] Tool summary continues to work in direct and service modes.

Validation:

- [x] Full plugin build with `run.cmd build --build_plugins`.
- [x] Direct-mode ask/tools/workflow smoke.
- [x] Service-mode ask/tools/topology/observability smoke.

## Phase 14: service-mode RPC integration self-test

Status: `[x]`

Goal: Make service-mode rDSN RPC flows repeatably testable.

Files:

- `apps/codepilot/codepilot_app.*`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `examples/README.md`

Work items:

- [x] Add `codepilot selftest [checkpoint-path]`.
- [x] Exercise model invoke, tool describe, state put/get/checkpoint, workflow
  compile/run, and observability snapshot through `rasn_service_graph`.
- [x] Ensure the same command uses typed RPC clients in rDSN service mode.
- [x] Add service-mode smoke config with `rasn.codepilot` arguments.

Validation:

- [x] Direct-mode `selftest` with simulator.
- [x] Service-mode `selftest` through `examples/service-rpc-smoke.ini`.

## Phase 15: provider adapter hardening

Status: `[x]`

Goal: Strengthen real-provider adapters while keeping the prototype
dependency-light.

Files:

- `llm_provider.*`
- `config.ini`
- `README.md`

Work items:

- [x] Add provider profiles for Copilot-compatible, Ollama, llama.cpp, LM Studio,
  and generic OpenAI-compatible endpoints.
- [x] Add provider-specific config keys such as `copilot_endpoint`,
  `ollama_model`, `llama_cpp_endpoint`, and `lmstudio_model`.
- [x] Add token environment fallback lists and token-command fallback.
- [x] Add HTTP headers for Copilot-compatible profiles and bounded curl timeouts.
- [x] Preserve the simulator as the default offline provider.

Validation:

- [x] Provider summary reports profile metadata.
- [x] Simulator ask remains replayable.

## Phase 16: durable state journal

Status: `[x]`

Goal: Add an append-only journal behind the in-memory state store.

Files:

- `state_service.*`
- `config.ini`
- `README.md`

Work items:

- [x] Append every accepted state write before making it visible in memory.
- [x] Add `[rasn.state] journal_file`.
- [x] Recover from checkpoint plus journal, or from journal alone.
- [x] Compact the journal after a successful checkpoint.
- [x] Validate journal headers, record schemas, key namespaces, and encodings.

Validation:

- [x] State put/get/query still pass.
- [x] Checkpoint/recover still pass.
- [x] Journal-only recovery is covered by self-test paths.

## Phase 17: workflow optimizer improvements

Status: `[x]`

Goal: Compile workflow plans with latency, cost, and reliability annotations.

Files:

- `workflow.*`
- `examples/optimized-coding.workflow`
- `README.md`

Work items:

- [x] Parse `cost_hint`, `latency_ms`, and `reliability` node options.
- [x] Validate optimizer hints before execution.
- [x] Order ready nodes deterministically by optimizer score.
- [x] Add compiled-plan estimates for stages, max parallelism, critical path,
  total cost units, and minimum reliability.
- [x] Pass optimizer annotations as model context.

Validation:

- [x] Workflow validate/compile/run still pass.
- [x] Optimized workflow example compiles with the new plan text.

## Phase 18: observability diagnosis tooling

Status: `[x]`

Goal: Add richer trace inspection and failure diagnosis.

Files:

- `observability.*`
- `apps/codepilot/codepilot_app.*`
- `README.md`

Work items:

- [x] Add generic timeline formatter.
- [x] Add diagnosis summary with event-kind, failure, retry, replay-miss, and
  nondeterminism counts.
- [x] Expose `observe timeline [trace]`.
- [x] Expose `observe diagnose [trace]`.

Validation:

- [x] Observability snapshot remains compatible.
- [x] Timeline/diagnosis commands work after a self-test run.

## Phase 19: CLI UX, packaging, and tutorials

Status: `[x]`

Goal: Make the hardened prototype easy to run and refine.

Files:

- `README.md`
- `examples/README.md`
- `examples/*.workflow`
- `examples/service-rpc-smoke.ini`

Work items:

- [x] Update CLI help with self-test and observability commands.
- [x] Document provider profiles and token safety.
- [x] Document durable journal/checkpoint behavior.
- [x] Add optimizer and service-mode examples.
- [x] Keep all examples simulator-friendly by default.

Validation:

- [x] Example workflow validation/run.
- [x] Full plugin build.

## Phase 20: review-driven hardening

Status: `[x]`

Goal: Address the highest-risk findings from the design/implementation review.

Files:

- `policy_manager.cpp`
- `llm_provider.cpp`
- `state_service.cpp`
- `rasn.code.definition.h`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `docs/DESIGN.md`
- `README.md`

Work items:

- [x] Make `[rasn.policy]` override legacy `[rasn.codepilot.tools]`.
- [x] Move provider temp files to an OS temp `rasn-provider` directory when
  `[rasn.runtime] temp_dir` is empty or `.`.
- [x] Use unique provider temp filenames to reduce collision risk.
- [x] Make journal path semantics consistent for explicit and default
  checkpoints by using one configured/default journal stream.
- [x] Put `RPC_RASN_WORKFLOW_START` on `THREAD_POOL_RASN_WORKFLOW` to avoid
  default RPC pool starvation during nested synchronous coordinator calls.
- [x] Document remaining prototype boundaries instead of overstating guarantees.

Validation:

- [x] Full plugin build.
- [x] Direct self-test after hardening.
- [x] Service-mode self-test after hardening.

## Phase 21: platform hardening

Status: `[x]`

Goal: Close the next set of platform gaps needed for rASN to act as a reusable
agent-system nucleus rather than only a CodePilot demo.

Files:

- `agent_clients.*`
- `agent_services.*`
- `agent_registry.*`
- `workflow.*`
- `workflow_service.*`
- `apps/codepilot/codepilot_app.cpp`
- `docs/DESIGN.md`
- `README.md`

Work items:

- [x] Wire generic agent control RPCs for cancel, heartbeat, and query across
  model, tool, and coordinator serverlets.
- [x] Add client wrappers for the control RPCs.
- [x] Return explicit unsupported cancel responses until request tracking is
  implemented in a later phase.
- [x] Load static registry descriptors from `[rasn.agent.*]` config sections.
- [x] Emit workflow node status events during execution.
- [x] Persist latest per-node workflow state under
  `workflow-node/<run-id>/<node-id>` and preserve transitions in the state
  journal.
- [x] Extend self-test coverage for service-mode control RPCs and workflow node
  state.

Validation:

- [x] Full plugin build.
- [x] Direct self-test.
- [x] Service-mode self-test.

## Phase 22: cross-platform operator usability

Status: `[x]`

Goal: Make rASN easier to inspect and validate as a platform across Windows and
Linux.

Files:

- `apps/codepilot/codepilot_app.*`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Validate the current rASN plugin build in Ubuntu WSL with the Linux rDSN
  checkout and Boost tree.
- [x] Add `registry list|get|query` CLI commands for agent descriptor inspection.
- [x] Add `agentctl describe|heartbeat|query|cancel` for built-in agent control
  RPCs.
- [x] Add `workflow nodes <run-id>` to inspect per-node workflow state.
- [x] Normalize local-tool path separators for Windows-authored workflows running
  under Linux WSL.
- [x] Document the operator-facing inspection commands.

Validation:

- [x] Linux WSL plugin build.
- [x] Windows plugin build.
- [x] Direct self-test.
- [x] Service-mode self-test.

## Phase 23: registry usability hardening

Status: `[x]`

Goal: Make the registry inspection surfaces consistent between direct CLI mode
and rDSN service mode.

Files:

- `agent_registry.*`
- `agent_services.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Extract static `[rasn.agent.*]` loading into an idempotent shared helper.
- [x] Load static agent descriptors during direct service-graph startup, not only
  from `rasn.registry` app startup.
- [x] Make `registry list|get|query` use `rasn.registry` RPC when service clients
  are enabled.
- [x] Keep direct CLI registry inspection backed by the local in-process registry.

Validation:

- [x] Windows plugin build.
- [x] Direct static-registry smoke.
- [x] Service-mode static-registry smoke.
- [x] WSL Linux build and direct static-registry smoke.

## Phase 24: workflow cancellation semantics

Status: `[x]`

Goal: Make workflow cancellation observable, terminal, and safe from being
overwritten by later synchronous execution completion.

Files:

- `workflow.*`
- `workflow_service.*`
- `apps/codepilot/codepilot_app.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a workflow execution cancellation predicate checked before each node.
- [x] Persist `cancelled` node statuses for skipped nodes when a cancellation is
  observed.
- [x] Preserve an existing cancelled run record in `workflow_store::start`
  instead of overwriting it with completed or failed status.
- [x] Extend direct self-test coverage with an in-process running workflow
  cancellation probe.
- [x] Document cooperative cancellation semantics and the remaining synchronous
  execution boundary.

Validation:

- [x] Windows plugin build.
- [x] Direct self-test.
- [x] Service-mode self-test.
- [x] WSL Linux plugin build and direct smoke.

## Phase 25: workflow run recovery

Status: `[x]`

Goal: Make persisted workflow run metadata usable after process restarts, so
operators can query completed, failed, or cancelled runs from the state
checkpoint/journal instead of relying only on in-memory workflow-service state.

Files:

- `workflow_service.*`
- `apps/codepilot/codepilot_app.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Decode persisted `workflow/<run-id>` records written by `rasn.workflow`.
- [x] Hydrate the workflow store from current `rasn.state` records during
  workflow-service startup.
- [x] Recover a single run from state when `workflow_store::query` misses the
  in-memory run table.
- [x] Make `workflow query <run-id>` trigger state recovery and retry before
  reporting a missing run in direct CLI mode.
- [x] Document recovery behavior and remaining per-node resume boundaries.

Validation:

- [x] Windows plugin build.
- [x] Direct self-test.
- [x] Direct restart-style workflow run/query recovery smoke.
- [x] Service-mode self-test.
- [x] WSL Linux plugin build and direct recovery smoke.

## Phase 26: service startup state auto-recovery

Status: `[x]`

Goal: Make service-mode restarts load existing local state by default so workflow
hydration and operator queries work after a normal restart without requiring an
explicit `recover_on_start` value for the configured default checkpoint/journal.

Files:

- `state_service.*`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a state-store check for whether the configured checkpoint or journal
  exists.
- [x] Preserve explicit `[rasn.state] recover_on_start` behavior and fail startup
  on explicit recovery errors.
- [x] Auto-recover default state at `rasn.state` startup only when a checkpoint or
  journal exists.
- [x] Keep first boot safe by starting normally when no persisted state exists.
- [x] Document service startup auto-recovery semantics.

Validation:

- [x] Windows plugin build.
- [x] Direct self-test.
- [x] Service-mode workflow run, restart, and query recovery smoke.
- [x] Service-mode self-test.
- [x] WSL Linux plugin build and direct smoke.

## Phase 27: rASN and CodePilot unit tests

Status: `[x]`

Goal: Add focused unit tests for the generic rASN runtime surfaces and the
CodePilot local tool adapter without requiring a live LLM provider.

Files:

- `tests/CMakeLists.txt`
- `tests/rasn_unit_tests.cpp`
- `tests/standalone/rasn_unit_test_main.cpp`
- `tests/standalone/test.cmd`
- `tests/standalone/test.sh`
- `README.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a `rasn.unit_tests` executable target under the rASN plugin build.
- [x] Initialize a minimal rDSN runtime before running Google Test so rDSN locks,
  filesystem helpers, and config APIs are safe in standalone unit tests.
- [x] Cover agent request/response validation, policy side-effect
  classification, path/word helpers, workflow parsing and cancellation, and
  CodePilot read/search tools.
- [x] Add Windows and Linux per-binary test scripts so the target fits the
  existing rDSN test harness model.
- [x] Generate LF-normalized Linux test scripts during CMake post-build.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 6 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 6 tests passed.

## Phase 28: request deadlines and workflow budgets

Status: `[x]`

Goal: Make existing rASN request timeout fields and workflow `budget_ms`
metadata affect service-mode dispatch instead of remaining descriptive-only.

Files:

- `agent_clients.*`
- `coordinator_service.cpp`
- `agent_services.*`
- `llm_provider.h`
- `workflow.*`
- `workflow_service.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a request-derived RPC timeout helper that falls back to
  `[rasn.rpc] timeout_ms` only when `agent_request.timeout_ms == 0`.
- [x] Use request-derived timeouts for registry lookup, coordinator invoke, and
  routed agent invoke RPCs.
- [x] Propagate workflow node `budget_ms` into model and tool dispatch paths.
- [x] Preserve workflow policy labels on model-node generic requests.
- [x] Add unit coverage for workflow budget propagation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 7 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 7 tests passed.
- [x] WSL Linux direct `codepilot selftest`.

## Phase 29: rDSN module reuse for state recovery

Status: `[x]`

Goal: Leverage existing rDSN modules conservatively before adding new runtime
infrastructure, starting with `dsn.tools.nfs` for remote state seeding.

Files:

- `rasn.code.definition.h`
- `state_service.*`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Review reusable rDSN modules: `dsn.tools.nfs`, `apps.skv`,
  `rDSN.dist.service` replication examples, and `dist.uri.resolver`.
- [x] Select NFS checkpoint import as the lowest-risk first integration because
  it fits the current file-backed state store and remains disabled by default.
- [x] Add `[rasn.state.nfs]` configuration for remote NFS host, source
  directory, checkpoint/journal file names, local staging directory, overwrite,
  and timeout.
- [x] Add an AIO task code and bounded `dsn::file::copy_remote_files` path that
  imports remote checkpoint artifacts before normal state recovery when local
  state is absent.
- [x] Keep all checkpoint validation and journal replay in the existing recovery
  logic after import.
- [x] Extend unit tests to cover local checkpoint/recovery.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 8 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 8 tests passed.
- [x] WSL Linux direct `codepilot selftest`.

## Phase 30: configurable service endpoints

Status: `[x]`

Goal: Remove the remaining hardcoded `localhost` service graph assumptions so
rASN can be deployed as separate rDSN services or via `dsn.dist.uri.resolver`
without changing code.

Files:

- `agent_types.h`
- `agent_runtime.*`
- `agent_registry.cpp`
- `agent_services.cpp`
- `coordinator_service.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `tests/*`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `endpoint_uri` to agent descriptors while preserving host/port fields.
- [x] Let built-in agent runtimes advertise URI endpoints when configured.
- [x] Load static agent `endpoint_uri` values from `[rasn.agent.*]`.
- [x] Make coordinator routing prefer URI endpoints and fall back to host/port.
- [x] Add `[rasn.service]` shared host, per-service host, and per-service URI
  configuration with the existing localhost ports as defaults.
- [x] Show URI endpoints in CodePilot registry/agent inspection output.
- [x] Add unit coverage for runtime endpoint metadata.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 9 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 9 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 31: registry heartbeats and dynamic leases

Status: `[x]`

Goal: Leverage rDSN RPC, tasking timers, and locks to make registry membership
usable for multi-service agent deployments instead of relying only on local
singleton registration.

Files:

- `rasn.code.definition.h`
- `agent_registry.*`
- `agent_services.*`
- `tests/*`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `LPC_RASN_REGISTRY_HEARTBEAT_TIMER` for periodic registry heartbeat
  work.
- [x] Make registry registration idempotent so repeated RPC registration updates
  descriptors instead of failing as duplicates.
- [x] Track lease metadata for RPC-registered or heartbeat-updated agents.
- [x] Filter expired lease-tracked agents from healthy list/query results.
- [x] Register built-in agents through `rasn.registry` RPC when service clients
  are enabled.
- [x] Send periodic heartbeats through a rDSN timer and re-register if the
  registry reports an unknown agent.
- [x] Unregister built-in agents through registry RPC during service graph stop.
- [x] Add unit coverage for heartbeat endpoint refresh and lease expiration.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 10 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 10 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 32: active registry lease cleanup

Status: `[x]`

Goal: Reuse rDSN service-app timers to actively remove stale dynamic registry
entries instead of only filtering them at query time.

Files:

- `rasn.code.definition.h`
- `agent_registry.*`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `LPC_RASN_REGISTRY_LEASE_SWEEP_TIMER` for registry-owned cleanup work.
- [x] Add `[rasn.registry] sweep_interval_ms` with `0` as the opt-out value.
- [x] Start the sweep timer from `rasn_registry_app::start` and cancel it during
  stop.
- [x] Reuse `agent_registry::expire_leases` from the timer callback.
- [x] Preserve static `[rasn.agent.*]` descriptors by keeping them
  non-lease-tracked.
- [x] Extend unit coverage to verify static agents survive lease expiration.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 10 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 10 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 33: resumable workflow execution

Status: `[x]`

Goal: Reuse persisted rDSN state records so workflow retries and service
restarts can resume from completed nodes instead of re-running the whole graph.

Files:

- `workflow.*`
- `workflow_service.*`
- `apps/codepilot/codepilot_app.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add resume state input to `workflow_graph::execute`.
- [x] Decode persisted `workflow-node/<run-id>/<node-id>` records from
  `rasn.state`.
- [x] Add `workflow_start_request.resume` and source identity checks.
- [x] Skip completed nodes, seed dependency outputs, and persist `resumed`
  status transitions.
- [x] Add `codepilot workflow resume <file> <run-id>`.
- [x] Extend unit/self-test coverage for resumed nodes.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 11 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 11 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 34: bounded coordinator retries

Status: `[x]`

Goal: Turn existing retry metadata and runtime trace hooks into real
coordinator behavior for transient model-agent failures, while preserving
side-effect safety for tools.

Files:

- `agent_messages.h`
- `llm_provider.h`
- `coordinator_service.*`
- `agent_services.cpp`
- `workflow.*`
- `workflow_service.cpp`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Propagate model `retry_budget` through compatibility request types and
  workflow model nodes.
- [x] Add workflow syntax aliases `retry_budget <n>` and `retry <n>`.
- [x] Add coordinator retry helper that records retry/failure trace events.
- [x] Cap retries with `[rasn.coordinator] max_retry_budget`.
- [x] Disable coordinator retries for `tool.*` capabilities to avoid duplicate
  side effects.
- [x] Add unit coverage for retryable model retries, tool non-retry behavior, and
  workflow retry-budget propagation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 13 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 13 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 35: deadline-aware model execution

Status: `[x]`

Goal: Make workflow/model execution budgets reach provider execution instead of
only bounding outer RPC waits.

Files:

- `agent_messages.h`
- `agent_services.cpp`
- `llm_provider.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `timeout_ms` to `agent_completion_request` and preserve it through
  model-agent compatibility adapters.
- [x] Propagate completion timeout into `llm_request.timeout_ms`.
- [x] Apply the smaller of request timeout and `[rasn.model] request_timeout_sec`
  as curl `max-time`.
- [x] Record provider timeout/failure events when curl exits unsuccessfully.
- [x] Add unit coverage for timeout and retry-budget preservation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 14 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 14 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 36: conditional state writes and workflow execution leases

Status: `[x]`

Goal: Prevent hidden last-writer-wins races in durable state and workflow
execution by adding explicit compare-and-swap style writes and state-backed
workflow ownership.

Files:

- `rasn.code.definition.h`
- `state_service.*`
- `agent_services.*`
- `workflow_service.*`
- `tests/CMakeLists.txt`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `state_put_request` with create-only and expected-sequence conditions.
- [x] Expose conditional state writes through
  `RPC_RASN_STATE_PUT_CONDITIONAL` and the service graph client path.
- [x] Persist workflow execution leases under `workflow-lease/<run-id>`.
- [x] Reject duplicate active starts/resumes while a lease is still valid.
- [x] Allow stale lease takeover after `[rasn.workflow] execution_lease_ms`.
- [x] Require terminal workflow state writes to still own the lease and match the
  previous whole-run sequence, preventing stale owners from overwriting newer
  executions.
- [x] Add unit coverage for state CAS, active lease rejection, and stale lease
  takeover/release.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 17 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 17 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 37: workflow execution lease renewal

Status: `[x]`

Goal: Keep active workflow leases fresh during long-running nodes so a healthy
owner cannot be mistaken for a stale execution and stolen by a resume attempt.

Files:

- `rasn.code.definition.h`
- `workflow_service.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `tests/standalone/rasn_unit_test_main.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `LPC_RASN_WORKFLOW_LEASE_RENEW_TIMER` for service-context lease
  renewal.
- [x] Add `[rasn.workflow] execution_lease_renew_ms`, deriving a safe interval
  from the lease TTL when set to `0`.
- [x] Renew active `workflow-lease/<run-id>` records with expected-sequence CAS
  writes while graph execution is in progress.
- [x] Preserve the latest renewed lease sequence for terminal state writes and
  lease release.
- [x] Keep direct/inline CLI workflow execution safe with the same renewal loop
  when no rDSN app context is attached.
- [x] Add unit coverage proving a duplicate resume cannot steal a long-running
  workflow after the original lease TTL has passed.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 18 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 18 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 38: workflow state service boundary

Status: `[x]`

Goal: Make workflow persistence use the rDSN state service boundary in RPC mode
instead of directly coupling workflow execution to the in-process state store.

Files:

- `workflow_service.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add workflow-local state helpers backed by `rasn_service_graph` state APIs.
- [x] Route whole-run persistence, terminal CAS writes, lease acquire/renew/check
  /release, resume-state queries, and per-node state writes through those helpers.
- [x] Preserve inline CLI behavior by relying on the service graph's local
  fallback when RPC clients are disabled.
- [x] Verify service-mode workflow execution still passes the RPC self-test, which
  now exercises `RPC_RASN_STATE_*` from `rasn.workflow`.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 18 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 18 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 39: policy artifact state boundary

Status: `[x]`

Goal: Make policy-managed tool-output artifact indexing use the rDSN state
service boundary in service mode instead of directly coupling policy code to the
in-process state store.

Files:

- `policy_manager.*`
- `agent_services.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add an injectable policy state writer with a local state-store default.
- [x] Install a service-graph-backed writer during rASN service graph startup so
  oversized tool-output artifact metadata routes through `rasn.state` RPC when
  service clients are enabled.
- [x] Reset the writer on service graph shutdown to avoid leaking lifecycle state.
- [x] Add unit coverage proving bounded tool output uses the configured state
  writer and records artifact metadata.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 19 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 19 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 40: observability snapshot state boundary

Status: `[x]`

Goal: Make observability snapshots durable and service-bound by indexing compact
snapshot metadata through `rasn.state` while preserving append-only JSONL trace
events.

Files:

- `observability.*`
- `agent_services.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add an injectable observability state writer with a local state-store default.
- [x] Install a service-graph-backed writer during rASN service graph startup and
  reset it during shutdown.
- [x] Index every observability snapshot as
  `observability-snapshot/<trace>/<sequence>` with trace file, event count,
  failure count, and last sequence metadata.
- [x] Extend CodePilot self-test coverage to query the snapshot index through
  `rasn.state` in both inline and service modes.
- [x] Add unit coverage proving snapshot indexing uses the configured writer.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 20 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 20 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 41: shared service graph lifecycle

Status: `[x]`

Goal: Prevent service-mode shutdown races by making the shared
`rasn_service_graph` lifecycle reference-counted across rDSN app wrappers.

Files:

- `agent_services.*`
- `workflow_service.cpp`
- `observability.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add lock-protected `acquire`/`release` lifecycle ownership APIs to
  `rasn_service_graph`.
- [x] Keep `start` idempotent for direct CLI and per-request ensure-start paths.
- [x] Defer `stop` while lifecycle owners remain, and stop the graph only on the
  final release.
- [x] Make app wrappers that depend on the shared graph retain/release it:
  model agent, tool agent, coordinator, workflow, observability, and CodePilot.
- [x] Add unit coverage proving explicit stop is deferred while owners remain and
  the graph stops after the last owner releases it.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 21 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 21 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 42: lazy CodePilot graph ownership

Status: `[x]`

Goal: Make CodePilot startup product-safe by avoiding eager global service graph
startup from CLI construction and retaining the graph only while commands run.

Files:

- `agent_services.*`
- `rasn.code.definition.h`
- `apps/codepilot/codepilot_app.cpp`
- `workflow_service.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Remove eager `_services.start()` from `codepilot_cli` construction.
- [x] Add a command-scoped lifecycle guard that acquires the shared graph for
  `codepilot_cli::run` and releases it when the command or REPL exits.
- [x] Preserve the service-app lifecycle reference added by `codepilot_app::start`
  so service mode keeps the graph alive outside the command task.
- [x] Move lifecycle start/stop work outside the lifecycle lock so RPC
  registration does not block while holding an rDSN lock.
- [x] Add a CodePilot service-mode dependency readiness gate before command
  execution.
- [x] Delay workflow startup recovery until `rasn.state` is ready instead of
  running a blocking state query during concurrent app startup.
- [x] Add unit coverage proving tool-provider installation does not start the
  service graph.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 22 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 22 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 43: comprehensive service readiness gate

Status: `[x]`

Goal: Make service-mode CodePilot startup fail deterministically and
diagnosably when any required rASN service surface is not yet accepting RPCs.

Files:

- `apps/codepilot/codepilot_app.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Replace the state-only service dependency probe with non-mutating probes
  for state, registry, coordinator, model agent identity, model health, tool
  agent identity, workflow validation, and observability.
- [x] Preserve direct CLI behavior while gating only service-mode RPC client
  execution.
- [x] Return an actionable readiness error that names every service boundary
  still failing in the last probe round.
- [x] Document the full service readiness surface.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 22 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 22 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 44: agent request cancellation tracking

Status: `[x]`

Goal: Make the generic agent cancel RPC meaningful by tracking in-flight
requests and converting cancelled work into structured cancellation failures.

Files:

- `agent_runtime.*`
- `agent_services.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add bounded in-flight request tracking and cancellation tombstones to
  `agent_runtime`.
- [x] Gate built-in model, tool, and coordinator invocations through the request
  lifecycle tracker.
- [x] Implement `RPC_RASN_AGENT_CANCEL` with `cancel_not_found`,
  already-cancelled, and accepted in-flight cancellation outcomes.
- [x] Convert completed provider/tool responses to `request_cancelled` when a
  cancellation arrives while work is in flight.
- [x] Update CodePilot self-test expectations and document cooperative
  cancellation semantics.
- [x] Add unit coverage for runtime tombstones and in-flight tool cancellation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 24 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 24 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 45: cancellation tombstone retention

Status: `[x]`

Goal: Keep cancellation terminal under sustained cancel traffic by ensuring
bounded tombstone trimming cannot evict a cancellation for work that is still in
flight.

Files:

- `agent_runtime.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Change cancellation tombstone trimming to skip still-in-flight request ids.
- [x] Trim completed tombstones again when requests finish, so memory remains
  bounded once work leaves the in-flight table.
- [x] Add a regression test that keeps one cancelled request in flight while
  forcing more than 1024 other cancellation tombstones through the runtime.
- [x] Document that active cancellations survive tombstone pressure.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 25 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 25 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 46: workflow state recovery errors

Status: `[x]`

Goal: Prevent state-service outages or corrupt state access from being reported
as missing workflows during on-demand run recovery.

Files:

- `workflow_service.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Distinguish `state key not found` from other `rasn.state` errors when
  recovering a workflow run by id.
- [x] Propagate non-missing-key state lookup failures through `workflow query`
  and duplicate/resume checks in `workflow start`.
- [x] Add injectable workflow state readers for deterministic boundary tests.
- [x] Add regression coverage for state lookup failure propagation.
- [x] Document that state recovery failures are surfaced rather than masked as
  missing workflow runs.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 26 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows service-mode RPC smoke.
- [x] WSL Linux plugin build.
- [x] WSL Linux `rasn.unit_tests` script, 26 tests passed.
- [x] WSL Linux direct `codepilot selftest`.
- [x] WSL Linux service-mode RPC smoke.

## Phase 47: restart-safe workflow cancellation

Status: `[x]`

Goal: Make `workflow cancel` use the same state-backed recovery and durability
rules as query/start, so cancellation remains reliable after process restarts and
does not report success when the terminal state transition was not persisted.

Files:

- `workflow_service.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add an injectable workflow state writer so persistence failures can be
  tested at the service boundary without bypassing `rasn.state` semantics.
- [x] Make `workflow cancel <run-id>` recover `workflow/<run-id>` from state when
  the run is absent from the in-memory table.
- [x] Persist the `cancelled` transition through a conditional state write and
  return an explicit error if the durable transition fails or conflicts.
- [x] Add regression coverage for cancelling recovered runs and surfacing cancel
  persistence failures.
- [x] Update user docs, design notes, and the ACM-style technical report.

Validation:

- [x] Windows targeted `rasn.unit_tests` binary, 28 tests passed.
- [x] WSL report compile with `pdflatex`/`bibtex`.
- [x] `git diff --check`.

## Phase 48: CodePilot tool safety hardening

Status: `[x]`

Goal: Reduce local data-loss and policy-bypass risk in CodePilot by making the
tool provider itself enforce default-deny policy and by replacing direct
truncate writes with rDSN filesystem-backed atomic replacement.

Files:

- `apps/codepilot/local_tools.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Re-check `rasn.policy` inside the CodePilot provider as defense in depth,
  so direct provider calls cannot bypass write/shell deny-by-default behavior.
- [x] Add `codepilot_write_file_atomically` using temporary files and
  `dsn::utils::filesystem::rename_path`.
- [x] Route `write` and `replace` through the atomic helper instead of truncating
  targets in place.
- [x] Add regression tests for provider-side default-deny write/shell behavior
  and atomic replacement.
- [x] Update user docs, design notes, and the ACM-style technical report.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 30 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 49: workspace-root tool sandbox

Status: `[x]`

Goal: Improve the report's policy-boundary limitation by adding an optional
repository/workspace sandbox for CodePilot local tool targets.

Files:

- `policy_manager.*`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `[rasn.policy] workspace_root` and compatibility alias
  `[rasn.codepilot.tools] workspace_root`.
- [x] Resolve tool targets and workspace roots through rDSN filesystem absolute
  and normalized path helpers.
- [x] Deny read/write-class tool targets outside the configured root before
  existence, write, or shell policy checks.
- [x] Add regression coverage for workspace containment and prefix-boundary
  rejection.
- [x] Update the ACM report table so repository sandboxing is no longer listed
  as future work. Phase 50 follows up by removing the human-approval gap.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 31 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 50: side-effect tool approvals

Status: `[x]`

Goal: Improve the report's policy-boundary limitation by adding an explicit
human approval gate for enabled CodePilot write/replace/shell tools.

Files:

- `agent_messages.h`
- `agent_tools.h`
- `agent_services.*`
- `policy_manager.*`
- `apps/codepilot/codepilot_app.*`
- `apps/codepilot/local_tools.*`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Carry tool policy labels through generic `agent_request` and
  `agent_tool_request`.
- [x] Add side-effect-specific `human_approved:<side-effect>` labels and policy
  helpers.
- [x] Require approval labels when `[rasn.policy] require_write_approval` or
  `require_shell_approval` is enabled.
- [x] Make CodePilot prompt for model-requested side-effect tool calls and allow
  explicit direct approval through `tool --yes`.
- [x] Pass approval labels to the provider-side policy re-check so
  defense-in-depth remains consistent with `rasn.tool.agent`.
- [x] Update documentation and the ACM report so human approval is no longer
  described as future work.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 32 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct approval gate check: denied `tool write` without approval
  and allowed `tool --yes write` with the same temporary config.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 51: replayable tool results

Status: `[x]`

Goal: Reduce the replay limitation by making recorded tool outcomes reusable
during replay, especially for side-effecting tools.

Files:

- `rasn_core.*`
- `agent_services.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Load `tool.ok` and `tool.error` events from replay JSONL traces.
- [x] Match replayed tool results by tool name and normalized recorded argument
  string.
- [x] Return matching recorded tool output/error without invoking the provider.
- [x] Fail closed for side-effect tools in replay mode when no recorded result
  exists, instead of repeating writes or shell commands.
- [x] Record replay hits and replay misses as structured runtime events.
- [x] Add regression coverage for replayed shell output and missing side-effect
  tool results.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 33 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 52: shell command sandbox controls

Status: `[x]`

Goal: Reduce the process-sandbox limitation for the shell escape hatch by adding
configurable command allowlists and workspace-rooted command execution.

Files:

- `apps/codepilot/local_tools.*`
- `tests/rasn_unit_tests.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add optional `[rasn.policy] shell_allowed_commands` and compatibility alias
  under `[rasn.codepilot.tools]`.
- [x] Reject shell metacharacters while the allowlist is active.
- [x] Match allowlisted commands by executable basename with Windows extension
  normalization.
- [x] Add optional `[rasn.policy] shell_working_directory`; fall back to
  `workspace_root` when set.
- [x] Wrap shell commands so they start in the configured working directory.
- [x] Add regression coverage for allowlist acceptance, allowlist denial,
  metacharacter denial, and working-directory wrapping.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 34 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 53: runtime schema manifest

Status: `[x]`

Goal: Reduce the hand-written-schema/IDL limitation by making current rASN
message contracts explicit, inspectable, and regression-tested.

Files:

- `schema_manifest.*`
- `apps/codepilot/codepilot_app.cpp`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add schema descriptors for agent, policy, observability, state, and
  workflow contracts.
- [x] Expose `rasn_schema_manifest_text()` for CLI and tests.
- [x] Add `codepilot schema` to print the manifest.
- [x] Add unit coverage proving core contracts and fields are listed.
- [x] Update docs/report to distinguish the implemented runtime schema manifest
  from future external IDL/code generation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 35 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema` smoke.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 54: model response replay

Status: `[x]`

Goal: Reduce the replay limitation by replaying recorded model responses before
calling model providers.

Files:

- `rasn_core.*`
- `agent_services.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Load `llm.response` events from replay JSONL traces.
- [x] Replay model responses in provider order before invoking the configured
  model provider.
- [x] Record replay hits and misses for model-provider responses.
- [x] Add regression coverage for provider-ordered response replay.
- [x] Update docs/report to narrow the then-remaining replay gap to scheduling,
  filesystem snapshots, and unmatched external effects. Filesystem snapshots are
  addressed in Phase 59.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 36 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 55: structured tool descriptors

Status: `[x]`

Goal: Resolve the structured-tool-schema gap by giving tool providers typed
descriptors instead of relying only on prose descriptions.

Files:

- `agent_tools.h`
- `apps/codepilot/local_tools.*`
- `schema_manifest.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add generic `tool_descriptor` and `tool_argument_descriptor` structs.
- [x] Add `agent_tool_provider::describe_tool_schemas()`.
- [x] Implement structured descriptors for CodePilot list/read/search/write/
  replace/shell tools.
- [x] Render `describe_tools()` from descriptors so text and schema stay in sync.
- [x] Add schema-manifest and unit-test coverage for tool descriptors.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 37 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 56: secret redaction boundary

Status: `[x]`

Goal: Reduce the stronger-secret-handling limitation by adding a shared
redaction boundary for traces, tool outputs/artifacts, and model-provider
prompts/responses.

Files:

- `redaction.*`
- `rasn_core.cpp`
- `policy_manager.cpp`
- `agent_services.cpp`
- `workflow.cpp`
- `schema_manifest.cpp`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `tests/CMakeLists.txt`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add deterministic redaction helpers for configured exact values, token
  environment variables, bearer tokens, and common secret key/value fields.
- [x] Redact runtime event values before in-memory observability and JSONL trace
  persistence.
- [x] Redact model prompts/context and model responses at service/workflow
  provider boundaries.
- [x] Redact tool outputs and errors before bounded previews or artifact spills.
- [x] Add config defaults, schema-manifest entry, and unit coverage for runtime,
  policy artifact, and workflow-provider redaction.
- [x] Update docs/report to narrow the remaining secret-handling gap to vault,
  permission, and hard isolation work.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 41 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema` smoke.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 57: workflow scheduler replay checks

Status: `[x]`

Goal: Reduce the scheduler-replay limitation by recording workflow node schedule
events and validating recorded node-start order during replay before executing
model/tool work.

Files:

- `rasn_core.*`
- `workflow.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Load `workflow.node.start` events from replay JSONL traces.
- [x] Record `workflow.node.start` and `workflow.node.finish` events during
  workflow execution.
- [x] Validate recorded node-start order in replay mode before dispatching a
  workflow node.
- [x] Surface schedule drift as a structured `replay.miss` failure.
- [x] Add unit coverage for recorded schedule events and schedule mismatch
  replay failures.
- [x] Update docs/report to narrow the then-open replay gap to filesystem
  snapshots and unmatched external effects. Filesystem snapshots are addressed
  in Phase 59.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 44 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema` smoke.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 58: shell execution timeout preemption

Status: `[x]`

Goal: Reduce the hard-preemption limitation for shell work by bounding shell
command runtime and terminating timed-out shell processes.

Files:

- `apps/codepilot/local_tools.*`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `[rasn.policy] shell_timeout_ms` and compatibility alias.
- [x] Launch Windows shell commands with native process APIs and explicit
  stdout/stderr pipes.
- [x] Terminate timed-out shell processes and return a structured tool error.
- [x] Add unit coverage for shell timeout behavior.
- [x] Update docs/report to narrow the remaining preemption gap to arbitrary
  provider SDKs and descendant process trees.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 44 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema` smoke.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 59: report-table limitation closure

Status: `[x]`

Goal: Resolve the remaining report-table limitations that can be addressed
inside the local prototype: external schema export, filesystem replay snapshots,
process-tree shell containment, and runnable evaluation harness support.

Files:

- `schema_manifest.*`
- `rasn_core.*`
- `apps/codepilot/codepilot_app.*`
- `apps/codepilot/local_tools.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add machine-readable JSON and IDL schema manifest exports through
  `codepilot schema json` and `codepilot schema idl`.
- [x] Record `filesystem.snapshot` events for CodePilot read/list/search tools
  and fail replay when current filesystem fingerprints drift from recorded
  snapshots.
- [x] Assign Windows shell commands to a job object so timeout termination also
  contains child processes.
- [x] Add `codepilot eval` and `codepilot eval external <template>` for runnable
  benchmark suites and external-CLI comparison harnesses.
- [x] Add unit coverage for schema export and filesystem replay snapshots.
- [x] Update docs/report tables and gap descriptions.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 45 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema json` smoke.
- [x] Windows direct `codepilot schema idl` smoke.
- [x] Windows direct `codepilot eval` smoke.
- [x] Windows direct `codepilot eval external` smoke with `cmd /c echo {prompt}`.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 60: generated C++ schema stubs

Status: `[x]`

Goal: Turn the schema manifest from an inspection artifact into a usable SDK
surface for C++ platform integrations by generating self-contained, versioned
contract stubs from the same manifest used for text, JSON, and IDL export.

Files:

- `schema_manifest.*`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add manifest records for nested agent, registry, tool, policy,
  observability, state, and workflow contracts referenced by top-level
  messages.
- [x] Add `rasn_schema_manifest_cpp_header()` to emit a self-contained C++
  header under `dsn::rasn::generated`.
- [x] Wire `codepilot schema cpp` and `schema c++` into the CLI and help text.
- [x] Add unit coverage for nested manifest records and generated C++ header
  content.
- [x] Update docs/report to narrow the then-remaining generated-stub gap to
  multi-language RPC/client generation beyond the C++ contract header. Phase 61
  follows up with TypeScript and Python data-contract stubs.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 45 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema cpp` smoke, including clean redirected
  header/JSON output and a `cl` compile of a file including the generated
  header.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 61: multi-language schema contract stubs

Status: `[x]`

Goal: Make rASN easier to integrate from non-C++ agent systems by generating
TypeScript interfaces and Python dataclasses from the same manifest that already
drives text, JSON, IDL, and C++ schema export.

Files:

- `schema_manifest.*`
- `main.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add TypeScript type mapping for strings, booleans, integer scalars,
  nested records, and repeated fields.
- [x] Add Python dataclass generation with safe `field(default_factory=list)`
  handling for repeated fields and optional nested records.
- [x] Wire `codepilot schema ts`/`typescript` and `codepilot schema py`/`python`
  through both the direct schema fast path and the normal CLI command path.
- [x] Add regression coverage for generated TypeScript and Python contracts.
- [x] Update docs/report to narrow the remaining generated-stub gap to
  executable RPC/client generation rather than data-contract generation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 45 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema ts` and `schema py` smoke with clean
  redirected output.
- [x] WSL Python syntax compile of generated dataclasses.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 62: structured JSON workflows

Status: `[x]`

Goal: Improve workflow language ergonomics by accepting structured JSON workflow
specs in addition to the compact text format, while preserving the same
validation, optimization, replay, and durable execution semantics.

Files:

- `workflow.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add strict workflow-local JSON parsing for objects, arrays, strings,
  unsigned integers, booleans, and null values without introducing a new
  dependency.
- [x] Accept either a root `nodes` array or an array of node objects.
- [x] Map JSON fields onto the existing `workflow_node` model and reuse
  `add_node` plus topological validation.
- [x] Add unit coverage for JSON workflow metadata, dependency parsing, default
  capability assignment, escaped strings, and invalid numeric ranges.
- [x] Update docs/report to narrow the remaining workflow-language ergonomics
  gap.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 46 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct JSON workflow validate/compile smoke.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 63: provider-neutral model streaming

Status: `[x]`

Goal: Add a provider-neutral streaming surface so applications can render model
responses incrementally and traces can expose chunk-level model output without
depending on provider-specific streaming protocols.

Files:

- `llm_provider.*`
- `rasn_core.*`
- `agent_services.*`
- `apps/codepilot/codepilot_app.*`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `llm_provider::complete_streaming` and a shared chunk emitter.
- [x] Record redacted `llm.response.chunk` runtime events.
- [x] Route streaming through the LLM agent, coordinator facade, and service graph
  with a non-streaming RPC fallback that chunks the final response.
- [x] Add `codepilot stream <prompt>` for operator-facing streaming output.
- [x] Add unit coverage for redacted streaming chunks and trace events.
- [x] Update docs/report to narrow the streaming gap to provider-native network
  streaming optimizations rather than the platform streaming surface.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 47 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot stream` smoke.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 64: generated C++ RPC clients

Status: `[x]`

Goal: Reduce the typed-message limitation by extending the schema system from
data-contract export into executable C++ rDSN RPC-client generation.

Files:

- `schema_manifest.*`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add an RPC operation manifest covering agent, registry, state, workflow,
  model gateway, and observability task codes.
- [x] Add `rasn_schema_manifest_cpp_clients()` to emit generated C++ `clientlet`
  wrappers that call the typed rDSN RPC task codes.
- [x] Wire `codepilot schema clients-cpp` plus aliases into CLI dispatch and help.
- [x] Add unit coverage for the operation manifest and generated client surface.
- [x] Update docs/report to narrow the generated-client limitation.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 47 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct `codepilot schema clients-cpp` smoke.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 65: model credential handles

Status: `[x]`

Goal: Reduce the credential-management limitation by replacing ad hoc token
environment settings with explicit model credential handles that can reference
environment variables, files, or commands without exposing token values.

Files:

- `model_agent.h`
- `llm_provider.cpp`
- `agent_services.cpp`
- `schema_manifest.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `credential_ref` to the model provider descriptor and schema manifest.
- [x] Support `token_ref=env:...`, `token_ref=file:...`, and
  `token_ref=cmd:...` in curl-backed providers while keeping `token_env`
  compatibility.
- [x] Redact command-backed handles in provider descriptors as
  `cmd:<configured>` and fail explicit unresolved credential handles before
  issuing HTTP requests.
- [x] Include model provider contracts in generated schema surfaces.
- [x] Update docs/report to narrow the credential limitation to external
  vault/OS-backed secret providers.

Validation:

- [x] Windows plugin build.
- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 47 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct provider/schema smoke.
- [x] WSL report compile with `build.sh`.
- [x] `git diff --check`.

## Phase 66: product-readiness limitation closure

Status: `[x]`

Goal: Reduce the remaining product limitations around multi-language clients,
unmatched external effects, local-only state durability, and shell isolation
without overclaiming full production replication or sandboxing.

Files:

- `schema_manifest.*`
- `main.cpp`
- `apps/codepilot/codepilot_app.cpp`
- `rasn_core.*`
- `agent_services.cpp`
- `state_service.*`
- `apps/codepilot/local_tools.*`
- `config.ini`
- `examples/service-rpc-smoke.ini`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Generate TypeScript and Python transport-backed RPC clients from the same
  operation manifest as the C++ client generator.
- [x] Wire `codepilot schema clients-ts` and `clients-py` through both direct
  schema fast path and normal CLI dispatch.
- [x] Add `external.effect` runtime ledger events for side-effecting tool
  commits, denials, replay hits, and replay misses, including effect class,
  operation fingerprint, replay policy, and status.
- [x] Add optional `[rasn.state.replica]` local checkpoint/journal mirroring and
  recovery seeding from the replica directory.
- [x] Add `[rasn.policy] shell_executor=container` plus
  `shell_container_template` placeholders for routing approved shell commands
  through a configured container wrapper.
- [x] Add regression coverage for generated TypeScript/Python clients, external
  effect ledger events, and container command template formatting.
- [x] Update docs and report to narrow the remaining product gaps without
  claiming quorum replication, provider-native streaming, or hardened container
  orchestration.

Validation:

- [x] Windows plugin build.
- [x] Windows `rasn.unit_tests` script, 47 tests passed.
- [x] Windows direct `codepilot selftest`.
- [x] Windows direct schema client smoke for `clients-ts` and `clients-py`.
- [x] WSL report compile with `build.sh`; `main.pdf` generated successfully.
- [x] `git diff --check`.

## Phase 67: runtime metrics and operational command surface

Status: `[~]`

Goal: Close the observability metrics gap by exporting quantitative runtime
metrics through rDSN's existing `perf_counter` subsystem and exposing them
through both the CodePilot CLI and the rDSN `command_manager`, without adding a
parallel metrics stack or a bundled scrape gateway.

rDSN modules reused:

- `perf_counter` (`include/dsn/c/api_utilities.h`, `simple_perf_counter`):
  cumulative counters and latency percentiles computed by rDSN's counter timers.
- `command_manager` (`include/dsn/tool-api/command.h`): expose a live
  `rasn.metrics` command over the rDSN local/remote CLI.

Files:

- `metrics.h`
- `metrics.cpp`
- `rasn_core.*`
- `agent_services.*`
- `apps/codepilot/codepilot_app.cpp`
- `config.ini`
- `tests/CMakeLists.txt`
- `tests/rasn_unit_tests.cpp`
- `README.md`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a `metrics_registry` process-global facade over rDSN perf counters in
  the `rasn` section: cumulative `COUNTER_TYPE_NUMBER` series per runtime event
  kind plus lazily created per-failure-class counters, and
  `COUNTER_TYPE_NUMBER_PERCENTILES` latency series for task, model, and tool.
- [x] Keep metric data types and formatting (`metrics.h`, `metrics.cpp`)
  free of rDSN dependencies so they compile and unit test in isolation; render
  snapshots as text, Prometheus exposition format, and JSON.
- [x] Guard counter creation with a node-context check and make every update
  null/thread safe so the metric path is a no-op (never a crash) in inline, CLI,
  and node-less contexts; gate the whole subsystem on `[rasn.metrics] enabled`.
- [x] Increment counters at the single `nucleus_runtime::record_event` choke
  point; pair task/model latency inside the runtime and time tool latency in
  `rasn_service_graph::run_tool`.
- [x] Register a `rasn.metrics [text|prometheus|json]` command with
  `command_manager` once per process from `rasn_service_graph::start_unlocked`,
  and add `observe metrics [text|prometheus|json]` to CodePilot.
- [x] Add regression coverage for label sanitization, the three renderers,
  snapshot series presence, cumulative counter deltas from runtime events,
  per-failure-class counters, and the `rasn.metrics` command output.

Validation:

- [x] Local `-fsyntax-only` for the dependency-light metric translation units.
- [x] Local standalone run of the formatter check harness.
- [ ] Plugin build on a supported toolchain (thrift/boost externals are not
  available on the authoring host).
- [ ] `rasn.unit_tests` run on the remote dev machines.

## Phase 68: deterministic environment-input boundary

Status: `[~]`

Goal: Route rASN's in-process nondeterministic environment inputs — wall-clock
timing and randomness — through rDSN's pluggable environment provider so replay
and emulator tooling can virtualize or seed them, strengthening the platform's
deterministic-replay guarantee instead of relying on private, wall-clock-seeded
generators.

rDSN modules reused:

- `dsn_now_ms`/`dsn_now_ns` (`include/dsn/c/api_layer1.h`, backed by
  `env_provider::now_ns`) for runtime timing and latency measurement.
- `dsn_random64` (`include/dsn/c/api_layer1.h`, backed by `env_provider::random64`,
  whose thread-local generator tooling can seed via
  `set_thread_local_random_seed`) for identifier and sampling randomness. This is
  the same primitive rDSN itself uses to mint RPC trace ids.

Files:

- `rasn_core.cpp`
- `policy_manager.cpp`
- `llm_provider.cpp`
- `docs/DESIGN.md`
- `docs/report/main.tex`
- `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Replace the private `std::mt19937_64` trace-id generator in `make_trace_id`
  with `dsn_random64`, matching how rDSN mints RPC trace ids.
- [x] Replace `std::rand()` in the policy manager's artifact spill suffix with
  `dsn_random64`.
- [x] Draw the simulator provider's first-run pseudo-random choice from
  `dsn_random64` (still captured through `resolve_nondeterminism` so recorded
  traces replay exactly), removing the wall-clock `std::time` seed.
- [x] Keep purely-local, non-behavioral filename nonces (temporary paths) and
  human-readable wall-clock timestamps on the standard library, where env-provider
  virtualization adds no replay value.
- [x] Tidy includes after the migration (`rasn_core.cpp` drops `<chrono>`/`<random>`
  and adds `<ctime>`; `llm_provider.cpp` drops `<ctime>`/`<random>`).

Validation:

- [x] Local `-fsyntax-only` check that `dsn_random64` resolves and the call sites
  type-check against the real rDSN C API header.
- [ ] Plugin build on a supported toolchain (thrift/boost externals are not
  available on the authoring host).
- [ ] `rasn.unit_tests` run on the remote dev machines.

## Phase 69: model gateway circuit breaker

Status: `[~]`

Goal: Make the platform react to a degrading model endpoint instead of merely
observing it. A consecutive-failure circuit breaker in the model gateway lets a
failing or hanging provider fail fast — before its own retry loop runs — so
latency and budget are not wasted on a dead dependency, closing the
"overload / dependency isolation" gap in the limitations table.

rDSN modules reused:

- `dsn_now_ms` (`include/dsn/c/api_layer1.h`, backed by `env_provider::now_ns`) as
  the breaker clock, so breaker timing is deterministic under replay and seedable
  by emulator tooling — the breaker stores no private clock.
- `perf_counter` (`include/dsn/c/api_utilities.h`) for the two breaker counters,
  reusing the existing `metrics_registry` choke point.
- `dsn_config` (`include/dsn/c/api_layer1.h`) for null-safe `[rasn.model]`
  tunables.
- `command_manager` (`include/dsn/tool-api/command.h`) for the `rasn.resilience`
  ops command.

Files:

- `circuit_breaker.h` / `circuit_breaker.cpp` (new)
- `metrics.cpp`, `rasn_core.h` / `rasn_core.cpp`
- `agent_services.h` / `agent_services.cpp`
- `apps/codepilot/codepilot_app.cpp`, `config.ini`
- `tests/rasn_unit_tests.cpp`
- `docs/DESIGN.md`, `README.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a dependency-light `circuit_breaker` / `circuit_breaker_registry` engine
  (closed/open/half-open, single half-open probe) with the clock injected as a
  `uint64_t now_ms`, mirroring `metrics.h`'s node-independent design.
- [x] Add `rasn_model_breaker_open_total` and
  `rasn_model_breaker_short_circuit_total` `perf_counter` series routed through
  `record_event`, plus runtime `record_model_breaker_*` methods.
- [x] Wire the breaker into `rasn_llm_agent_service::complete` /
  `complete_streaming` after the replay check and before the provider call;
  exempt only in-process providers (`in_process()`), so loopback HTTP providers
  (Ollama/llama.cpp/LM Studio) are guarded while the simulator and other
  in-process providers keep existing behavior.
- [x] Read `[rasn.model] circuit_breaker_*` once via `dsn_config`; expose state
  through the `rasn.resilience` command and CodePilot `observe resilience`, and
  append breaker state to `provider_summary`.
- [x] Add unit tests covering open/short-circuit, half-open probe recovery and
  reopen, success reset, disabled mode, registry isolation, and the ops command.

Validation:

- [x] Compile-and-run the breaker state machine standalone (all assertions pass)
  and `-fsyntax-only` `metrics.cpp` against the real rDSN headers; enum/array/map
  alignment verified.
- [ ] Plugin build on a supported toolchain (thrift/boost externals are not
  available on the authoring host).
- [ ] `rasn.unit_tests` run on the remote dev machines.

## Phase 70: model gateway admission control

Status: `[~]`

Goal: Protect a healthy-but-slow model endpoint from overload — the counterpart to
Phase 69's circuit breaker. A per-provider admission gate caps simultaneous
in-flight requests (a concurrency bulkhead) and applies graceful backpressure as
load approaches the cap, so bursts are smoothed and excess load fast-fails instead
of deepening queues and inflating tail latency. Together with the breaker this
closes the "no global concurrency cap or admission queue" half of the
"overload / dependency isolation" limitation (the remaining gap being a single
process-wide budget rather than per-provider).

rDSN modules reused:

- `exp_delay` (`include/dsn/utility/exp_delay.h`) as the backpressure curve — the
  same staged-delay admission-control primitive rDSN uses to throttle its task
  queues — so the overload policy is consistent with the host runtime rather than
  hand-rolled.
- `perf_counter` (`include/dsn/c/api_utilities.h`) for the two admission counters,
  reusing the existing `metrics_registry` choke point.
- `dsn_config` (`include/dsn/c/api_layer1.h`) for null-safe `[rasn.model]`
  admission tunables.
- `command_manager` (`include/dsn/tool-api/command.h`) — the admission state is
  surfaced through the existing `rasn.resilience` command alongside the breaker.

Files:

- `admission_gate.h` / `admission_gate.cpp` (new)
- `metrics.cpp`, `rasn_core.h` / `rasn_core.cpp`
- `agent_services.h` / `agent_services.cpp`
- `apps/codepilot/codepilot_app.cpp`, `config.ini`
- `tests/rasn_unit_tests.cpp`
- `docs/DESIGN.md`, `README.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a dependency-light `admission_gate` / `admission_gate_registry` engine: a
  per-provider concurrency bulkhead with an RAII `admission_slot` for capacity
  release, and an `exp_delay`-seeded backpressure delay (zero below the soft
  threshold, growing to a configured ceiling). The header stays rDSN-free; only the
  `.cpp` pulls in `exp_delay.h`.
- [x] Add `rasn_model_admission_rejected_total` and
  `rasn_model_admission_delayed_total` `perf_counter` series routed through
  `record_event`, plus runtime `record_model_admission_*` methods.
- [x] Wire the gate into `rasn_llm_agent_service::complete` / `complete_streaming`
  *before* the breaker and *after* the replay check, sharing the `in_process()`
  exemption so in-process providers and replayed runs are unaffected; release the
  slot via RAII on every exit path including a breaker short-circuit.
- [x] Read `[rasn.model] admission_*` once via `dsn_config`; surface live
  per-provider in-flight depth through the `rasn.resilience` command, CodePilot
  `observe resilience`, and `provider_summary`.
- [x] Add unit tests covering hard-cap rejection, RAII capacity release, disabled
  passthrough, backpressure onset/monotonicity/clamp, and registry isolation and
  ordered snapshots; extend the ops-command test to assert the admission section.

Validation:

- [x] Compile-and-run the admission engine standalone (all assertions pass) and
  `-fsyntax-only` `metrics.cpp` against the real rDSN headers; counter
  enum/array/map alignment verified.
- [ ] Plugin build on a supported toolchain (thrift/boost externals are not
  available on the authoring host).
- [ ] `rasn.unit_tests` run on the remote dev machines.

## Phase 71: model gateway rate limiter

Status: `[~]`

Goal: Govern outbound model *throughput* — the third classic dependency-protection
dimension beside Phase 69's circuit breaker (failure) and Phase 70's admission
control (concurrency). Hosted model APIs publish requests-per-minute quotas, and
exceeding them returns HTTP 429s that burn the retry budget and (on metered
endpoints) cost money; a concurrency bulkhead does not prevent this because a
single sequential caller stays at in-flight ≈ 1 yet can still exceed a per-minute
quota. A per-provider token-bucket rate limiter paces calls under the quota,
delaying briefly when slightly ahead and fast-failing when the projected wait
would exceed a bound. This adds the throughput axis the "overload / dependency
isolation" limitation still lacked.

rDSN modules reused:

- `dsn_now_ms` (env-provider clock) injected as the bucket's refill clock, so
  token accrual is deterministic under replay and seedable — the same reuse as the
  breaker's cooldown clock; the engine reads no clock of its own.
- `perf_counter` (`include/dsn/c/api_utilities.h`) for the two rate-limit counters,
  reusing the existing `metrics_registry` choke point.
- `dsn_config` (`include/dsn/c/api_layer1.h`) for null-safe `[rasn.model]`
  rate-limit tunables.
- `command_manager` (`include/dsn/tool-api/command.h`) — the rate-limiter state is
  surfaced through the existing `rasn.resilience` command alongside the breaker and
  admission gate.

Files:

- `rate_limiter.h` / `rate_limiter.cpp` (new)
- `metrics.cpp`, `rasn_core.h` / `rasn_core.cpp`
- `agent_services.h` / `agent_services.cpp`
- `apps/codepilot/codepilot_app.cpp`, `config.ini`
- `tests/rasn_unit_tests.cpp`
- `docs/DESIGN.md`, `README.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add a dependency-light `rate_limiter` / `rate_limiter_registry` engine: a
  per-provider token bucket whose time is injected as a millisecond value; decision
  is allow / allow-with-pacing-delay (reserving the next token) / reject when the
  wait would exceed `max_wait_ms`. The header stays rDSN-free.
- [x] Add `rasn_model_rate_limited_total` and `rasn_model_rate_delayed_total`
  `perf_counter` series routed through `record_event`, plus runtime
  `record_model_rate_*` methods.
- [x] Wire the limiter into `rasn_llm_agent_service::complete` /
  `complete_streaming` after admission and *before* the breaker (so a rate-rejected
  request never strands a half-open probe) and *after* the replay check; coalesce
  the admission backpressure and rate pacing into a single wait (the larger of the
  two) applied only after the breaker also admits; share the `in_process()`
  exemption.
- [x] Read `[rasn.model] rate_limit_*` once via `dsn_config` (clamped through
  `read_config_u32`); surface live per-provider rate/burst/tokens through the
  `rasn.resilience` command, CodePilot `observe resilience`, and `provider_summary`.
- [x] Add unit tests covering disabled/unlimited passthrough, full-bucket burst then
  pacing to the sustained rate, rejection past `max_wait_ms`, refill over time,
  non-monotonic-clock guard, default burst sizing, and registry isolation/ordered
  snapshots; extend the ops-command test to assert the rate-limiter section.

Validation:

- [x] Compile-and-run the rate-limiter engine standalone (all assertions pass) and
  `-fsyntax-only` `metrics.cpp` and `rate_limiter.cpp` against the real rDSN
  headers; counter enum/array/map alignment verified (23/23/23).
- [ ] Plugin build on a supported toolchain (thrift/boost externals are not
  available on the authoring host).
- [ ] `rasn.unit_tests` run on the remote dev machines.

## Phase 72: tool gateway admission and rate controls

Status: `[~]`

Goal: Close the next practical overload-isolation gap by applying the existing
rDSN-aligned admission and rate engines to tool execution. Model providers already
have failure/concurrency/throughput protection; tools now get explicit concurrency
and throughput governors keyed by tool name, without changing replay or policy
semantics.

rDSN modules reused:

- `admission_gate` / `admission_gate_registry`, including the `exp_delay`-backed
  backpressure curve and RAII `admission_slot`, now shared by model and tool
  gateways.
- `rate_limiter` / `rate_limiter_registry`, with `dsn_now_ms` supplied by the
  tool service as the token-bucket refill clock.
- `perf_counter` through the existing `record_event` / `metrics_registry` choke
  point for four tool-gateway counters.
- `dsn_config` for `[rasn.tool]` admission/rate tunables.
- `command_manager` for the expanded `rasn.resilience` command.

Files:

- `agent_services.h` / `agent_services.cpp`
- `rasn_core.h` / `rasn_core.cpp`
- `metrics.cpp`, `rate_limiter.h`, `config.ini`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`, `docs/DESIGN.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Replace the implicit all-tool execution serialization with a short provider
  lock: `rasn_tool_agent_service` takes a shared provider reference under
  `zlock`, then releases it before validation, policy, gates, and provider
  execution.
- [x] Add per-tool admission and rate helpers that run after replay lookup and
  policy approval, so replay hits, replay-missing side-effect failures, and policy
  denials never consume capacity.
- [x] Coalesce admission backpressure and rate pacing into one wait and release
  admission capacity with RAII on every exit path.
- [x] Add tool admission/rate runtime events and `perf_counter` series:
  `rasn_tool_admission_rejected_total`,
  `rasn_tool_admission_delayed_total`, `rasn_tool_rate_limited_total`, and
  `rasn_tool_rate_delayed_total`.
- [x] Expose live per-tool in-flight/rate state through `rasn.resilience` and
  CodePilot `observe resilience`.
- [x] Add targeted concurrency coverage for the tool admission cap and extend the
  ops-command assertion for tool sections.
- [x] Update user docs, design notes, and the ACM-style technical report.

Validation:

- [x] Built `rasn.unit_tests` and `codepilot` targets with repo CMake 3.22.6.
- [x] Ran `rasn.unit_tests` (`81` tests passed).
- [x] Ran direct `codepilot selftest`.
- [x] Ran `codepilot observe resilience` and confirmed model/tool sections.
- [x] `git diff --check`.
- [x] Report brace/bracket balance check (LaTeX toolchain not installed locally).

## Phase 73: remote-agent dispatch resilience

Status: `[~]`

Goal: Close the remaining remote-agent dependency-isolation gap by guarding
coordinator-to-agent RPC dispatch with the same failure/concurrency/throughput
engines already used by the model gateway. This makes service-mode custom agents
usable under production-style fan-out without turning registry-selected remote
agents into unbounded hidden dependencies.

rDSN modules reused:

- `circuit_breaker` / `circuit_breaker_registry`, keyed by remote `agent_id`, with
  retryable remote/RPC failures counting against the breaker.
- `admission_gate` / `admission_gate_registry`, including the `exp_delay`-backed
  backpressure curve and RAII `admission_slot`, keyed by remote `agent_id`.
- `rate_limiter` / `rate_limiter_registry`, with `dsn_now_ms` as the
  token-bucket refill clock.
- `perf_counter` through `record_event` / `metrics_registry` for six
  remote-agent counters.
- `dsn_config` for `[rasn.remote_agent]` circuit-breaker/admission/rate tunables.
- `command_manager` for the expanded `rasn.resilience` command.

Files:

- `agent_services.h` / `agent_services.cpp`
- `rasn_core.h` / `rasn_core.cpp`
- `metrics.cpp`, `config.ini`
- `apps/codepilot/codepilot_app.cpp`
- `tests/rasn_unit_tests.cpp`
- `README.md`, `docs/DESIGN.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add per-remote-agent breaker/admission/rate helpers in
  `rasn_coordinator_service` around the `RPC_RASN_AGENT_INVOKE` dispatch path.
- [x] Preserve inline standalone semantics: guards only run when the coordinator
  uses RPC clients.
- [x] Make the remote-agent breaker count only retryable dependency failures, so
  deterministic policy/validation/tool errors do not poison a remote agent.
- [x] Keep model-gateway ordering invariants: open-breaker precheck first,
  admission/rate before the authoritative breaker probe, rate-token refund on
  breaker short-circuit, and one coalesced delay.
- [x] Add remote-agent runtime events and `perf_counter` series:
  `rasn_remote_agent_breaker_open_total`,
  `rasn_remote_agent_breaker_short_circuit_total`,
  `rasn_remote_agent_admission_rejected_total`,
  `rasn_remote_agent_admission_delayed_total`,
  `rasn_remote_agent_rate_limited_total`,
  `rasn_remote_agent_rate_delayed_total`, and
  `rasn_remote_agent_endpoint_invalid_total`.
- [x] Preflight remote-agent endpoints before consuming any guard resource:
  resolve the descriptor address once, reject malformed/unresolvable endpoints
  with a counted `remote_agent.endpoint.invalid` event, and reuse the resolved
  address for dispatch.
- [x] Expose live per-agent state through `rasn.resilience` and CodePilot
  `observe resilience`.
- [x] Add `[rasn.remote_agent]` defaults to the main and service-smoke configs.
- [x] Update user docs, design notes, and the ACM-style technical report.

Validation:

- [x] Built `rasn.unit_tests` target with repo CMake 3.22.6 after implementation
  wiring.
- [x] Run full `rasn.unit_tests` (`81` tests passed).
- [x] Build `codepilot`.
- [x] Run direct `codepilot selftest`.
- [x] Run `codepilot observe resilience` and confirm model/tool/remote-agent
  sections.
- [x] `git diff --check`.
- [x] Report brace/bracket balance check (LaTeX toolchain not installed locally).

## Phase 74: process-wide overload budget

Status: `[~]`

Goal: Close the "limits are per-dependency rather than a single process-wide
budget" gap called out in the design's product-limitations section. Add one
global concurrency bulkhead and one global request-rate ceiling at the
coordinator `invoke` chokepoint so the total in-flight work and aggregate
throughput across every dependency (model, tool, remote-agent) are bounded,
protecting the host's shared capacity (threads, memory, sockets) even when each
individual dependency is healthy. The budget is opt-in (passthrough by default)
so zero-config behavior is unchanged.

rDSN modules reused:

- `admission_gate` (the `exp_delay`-backed backpressure curve and RAII
  `admission_slot`) as a single process-wide bulkhead — the same engine the
  per-dependency gateways use, here as a singleton rather than a registry.
- `rate_limiter` with `dsn_now_ms` as the token-bucket refill clock, as a single
  process-wide rate ceiling.
- `perf_counter` through `record_event` / `metrics_registry` for four overload
  counters.
- `dsn_config` for `[rasn.overload]` admission/rate tunables.
- `command_manager` for the `rasn.resilience` command (report already shared with
  CodePilot `observe resilience`).

Files:

- `agent_services.h` / `agent_services.cpp`
- `rasn_core.h` / `rasn_core.cpp`
- `metrics.cpp`, `config.ini`, `examples/service-rpc-smoke.ini`
- `tests/rasn_unit_tests.cpp`
- `README.md`, `docs/DESIGN.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Add `[rasn.overload]` admission/rate config readers and lazily-constructed
  process-wide `admission_gate` + `rate_limiter` singletons (under a
  `std::once_flag`) on `rasn_coordinator_service`.
- [x] Apply the budget at the outermost point of `invoke` — after
  `begin_request`/`scoped_agent_request` (so malformed/cancelled requests consume
  no budget) and before capability resolution/routing (so load sheds uniformly).
- [x] Hold the admission slot as a local RAII across retries, releasing on every
  return path; no rate refund needed (the gate is outermost, nothing
  short-circuits after the token is taken within its scope).
- [x] Coalesce admission and rate delays into a single sleep, matching the
  per-dependency gateways.
- [x] Fast-fail rejections with a structured, non-retryable `overload` response.
- [x] Add overload runtime events and `perf_counter` series:
  `rasn_overload_admission_rejected_total`,
  `rasn_overload_admission_delayed_total`, `rasn_overload_rate_limited_total`, and
  `rasn_overload_rate_delayed_total`.
- [x] Head the shared `rasn.resilience` / `observe resilience` report with live
  process-wide budget state (in-flight, caps, cached tokens).
- [x] Add `[rasn.overload]` defaults (concurrency caps `0`/passthrough) to the
  main and service-smoke configs.
- [x] Extend the metrics tests to cover the four overload counters.
- [x] Update user docs, design notes, and the ACM-style technical report.

Validation:

- [ ] Build/test deferred: this workstation has no C++ toolchain (per the
  operator constraint), so `rasn.unit_tests`/`codepilot` were not rebuilt here.
  The change mirrors the fully-tested Phase 73 remote-agent path and reuses the
  Phase 70/71 engines (covered by the `rasn_admission_gate` and `rasn_rate_limiter`
  suites); enum/array/map alignment, event-kind/counter-key agreement, and
  signature/caller/test agreement were verified by structural review.

## Phase 75: application directory layout

Status: `[x]`

Goal: Move CodePilot under an `apps/` namespace so rASN can host multiple
application adapters without mixing reusable nucleus code and product-specific
front ends.

Files:

- `apps/codepilot/*`
- `CMakeLists.txt`
- `tests/CMakeLists.txt`
- `tests/rasn_unit_tests.cpp`
- `README.md`, `docs/DESIGN.md`, `docs/report/main.tex`,
  `docs/IMPLEMENTATION_PLAN.md`

Work items:

- [x] Move the CodePilot executable sources from `codepilot/` to
  `apps/codepilot/`.
- [x] Keep the reusable rASN engine as the top-level `rasn` static library and
  build CodePilot as a thin executable with `add_subdirectory(apps/codepilot)`.
- [x] Update CodePilot include roots, config binplace paths, and unit-test helper
  source references for the deeper application directory.
- [x] Update user docs, design notes, and the technical report to describe
  `apps/codepilot/` as the first application adapter location.

Validation:

- [x] `rasn.unit_tests` target builds after the move.
- [x] `codepilot` target builds after the move.
- [x] Direct `codepilot selftest` passes after the move.
- [x] `git diff --check`.

## Dependency order

```text
Phase 1 task model
  -> Phase 2 runtime
  -> Phase 3 generic RPC
  -> Phase 4 registry
  -> Phase 5 coordinator
  -> Phase 6 state
  -> Phase 7 workflow
  -> Phase 8 model migration
  -> Phase 9 tool/policy
  -> Phase 10 observability/failure
  -> Phase 11 CodePilot migration
  -> Phase 12 examples/validation
  -> Phase 13 RPC hardening
  -> Phase 14 service-mode RPC self-test
  -> Phase 15 provider adapter hardening
  -> Phase 16 durable state journal
  -> Phase 17 workflow optimizer
  -> Phase 18 observability diagnosis
  -> Phase 19 CLI UX/package/tutorials
  -> Phase 20 review-driven hardening
  -> Phase 21 platform hardening
  -> Phase 22 cross-platform operator usability
  -> Phase 23 registry usability hardening
  -> Phase 24 workflow cancellation semantics
  -> Phase 25 workflow run recovery
  -> Phase 26 service startup state auto-recovery
  -> Phase 27 rASN and CodePilot unit tests
  -> Phase 28 request deadlines and workflow budgets
  -> Phase 29 rDSN module reuse for state recovery
  -> Phase 30 configurable service endpoints
  -> Phase 31 registry heartbeats and dynamic leases
  -> Phase 32 active registry lease cleanup
  -> Phase 33 resumable workflow execution
  -> Phase 34 bounded coordinator retries
  -> Phase 35 deadline-aware model execution
  -> Phase 36 conditional state writes and workflow execution leases
  -> Phase 37 workflow execution lease renewal
  -> Phase 38 workflow state service boundary
  -> Phase 39 policy artifact state boundary
  -> Phase 40 observability snapshot state boundary
  -> Phase 41 shared service graph lifecycle
  -> Phase 42 lazy CodePilot graph ownership
  -> Phase 43 comprehensive service readiness gate
  -> Phase 44 agent request cancellation tracking
  -> Phase 45 cancellation tombstone retention
  -> Phase 46 workflow state recovery errors
  -> Phase 47 restart-safe workflow cancellation
  -> Phase 48 CodePilot tool safety hardening
  -> Phase 49 workspace-root tool sandbox
  -> Phase 50 human approval gates
  -> Phase 51 replayable tool results
  -> Phase 52 shell command sandbox controls
  -> Phase 53 runtime schema manifest
  -> Phase 54 model response replay
  -> Phase 55 structured tool descriptors
  -> Phase 56 secret redaction boundary
  -> Phase 57 workflow scheduler replay checks
  -> Phase 58 shell execution timeout preemption
  -> Phase 59 report-table limitation closure
  -> Phase 60 generated C++ schema stubs
  -> Phase 61 multi-language schema contract stubs
  -> Phase 62 structured JSON workflows
  -> Phase 63 provider-neutral model streaming
  -> Phase 64 generated C++ RPC clients
  -> Phase 65 model credential handles
  -> Phase 66 product-readiness limitation closure
  -> Phase 67 runtime metrics and operational command surface
  -> Phase 68 deterministic environment-input boundary
  -> Phase 69 model gateway circuit breaker
  -> Phase 70 model gateway admission control
  -> Phase 71 model gateway rate limiter
  -> Phase 72 tool gateway admission and rate controls
  -> Phase 73 remote-agent dispatch resilience
  -> Phase 74 process-wide overload budget
  -> Phase 75 application directory layout
```

Some phases can overlap after Phase 3, but the public message model and generic
RPC layer should stabilize first.

## Open design questions

- Should `rasn.policy` be a service app from the beginning, or remain a
  config-backed library until dynamic policies are needed?
- Should `rasn.observability` persist full event streams through `rasn.state`, or
  keep JSONL as the append-only event log and use `rasn.state` only for compact
  snapshot/replay indexes?
- Should state service add an `apps.skv`/replication-backed store after the NFS
  import and local replica paths, and should prefix query semantics be implemented
  above SKV keys or through a separate index?
- Should generic workflow specs remain the current text format initially, or
  move to a more structured format before implementation?
- Should provider adapters stop using curl config files entirely in favor of a
  small HTTP client with in-memory headers or provider SDKs with native
  streaming?
- Should shell container execution remain a configurable command template, or
  become a first-class orchestrated executor with image policy, mount policy, and
  lifecycle tracking?
- Now that request-count overload is bounded per-dependency and process-wide,
  should the admission/rate engines gain an optional token/cost dimension (e.g. a
  tokens-per-minute bucket sized from provider usage) so metered model spend is
  governed alongside request rate, or is that better handled by a separate budget
  accountant above the coordinator?
