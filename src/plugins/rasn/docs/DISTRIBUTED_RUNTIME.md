# Distributed rASN runtime

This document is the design for the rASN **runtime**: the layer that hosts
the eleven shared modules every pilot (CodePilot, SREPilot, …) depends on, and
makes them deployable either in-process or as distributed services across one or
many rDSN nodes.

It captures the architecture, the provider model, the request/response contract,
the resilience and consistency policies, and the multi-node roadmap. For the
broader rASN design see [DESIGN.md](DESIGN.md); for operator-facing configuration
and the module/port table see the "Distributed rASN runtime modules" section of
[../README.md](../README.md).

## 1. Motivation

The rASN runtime modules (agent control plane, message bus, task orchestration kernel,
determinism ledger, capability directory, resource budget, recovery supervisor,
blackboard, contract verifier, human interaction, sandbox runtime) started life
as plain C++ objects embedded in the CLI process. That is fine for a single-process
prototype but does not match the intended rASN architecture, where:

- apps should talk to shared services through a **runtime/provider API**, not by
  reaching into module objects, and
- modules should be **independently deployable** so a cluster can scale, isolate,
  and place them per workload.

The rASN runtime turns that module layer into an rDSN-native, provider-selected
service tier that apps consume through one stable facade.

## 2. Design principles

1. **The runtime is a pluggable provider, like an rDSN environment/aspect provider.**
   There is a `local` provider, a `distributed` provider, and a `hybrid` provider.
2. **The deployment shape is chosen by configuration, not by app code.** Switching
   providers changes where modules run; apps are untouched.
3. **Apps depend only on the facade API.** `rasn_runtime` exposes typed,
   coarse-grained operations. Apps never see LPC vs. RPC, endpoints, retries, or
   serialization.
4. **The distributed provider talks to remote module services over RPC.** Services
   deploy on one or many nodes; modules are decoupled and independently placeable;
   operators supply endpoints (shared or per module).
5. **The local provider calls modules directly in-process** (or over an intra-node
   LPC when an rDSN node is active), with no network on the path.

These five principles are the user's design; the sections below realize them and
add the contracts a real network needs (resilience, idempotency, consistency,
discovery, security).

## 3. Architecture

```mermaid
flowchart TD
    subgraph App["App process (CodePilot / SREPilot)"]
        A[Agents / coordinator] --> F["rasn_runtime facade<br/>(typed module API)"]
        F --> P{{rasn_runtime_provider}}
    end

    P -- local --> L["invoke_local_module<br/>(direct call or intra-node LPC)"]
    P -- distributed --> R["invoke_remote_module<br/>(rDSN RPC + resilience)"]
    P -- hybrid --> H["per-module routing<br/>module_is_remote(module)"]
    H -- local --> L
    H -- remote --> R

    L --> S["rasn_runtime_service_store<br/>(process-global, in-process)"]

    subgraph Cluster["Module service nodes"]
        REG["rasn.registry<br/>(capability leases)"]
        R -->|resolve rasn.runtime.<module>| REG
        R -->|RPC envelope| SVC1["rasn.runtime<br/>(aggregate, :27107)"]
        R -->|RPC envelope| SVC2["rasn.runtime.budget<br/>(standalone, :27115)"]
        R -->|RPC envelope| SVCn["rasn.runtime.blackboard<br/>(standalone, :27117)"]
        SVC1 -->|register/heartbeat| REG
        SVC2 -->|register/heartbeat| REG
        SVCn -->|register/heartbeat| REG
        SVC1 --> STORE1[(module store)]
        SVC2 --> STORE2[(module store)]
        SVCn --> STOREn[(module store)]
    end
```

Layers:

- **Facade — `rasn_runtime`.** The only surface apps use. Methods such as
  `reserve_budget`, `put_blackboard`, `acquire_agent_lease`, plus health/topology
  (`ping_module`, `module_health`, `describe_module_health`, `describe_topology`).
- **Provider — `rasn_runtime_provider`.** Strategy behind the facade. Concrete
  providers: `local`, `distributed`, `hybrid`. Providers implement two protected
  primitives — `call_module_api(request)` and `write_state(...)` — plus health and
  topology hooks. The facade builds every operation on top of `call_module_api`.
- **Transport helpers.** `invoke_local_module`, `invoke_remote_module`, and
  `ping_remote_module` are shared free functions so all three providers apply the
  same resilience/idempotency policy; there is exactly one copy of the RPC logic.
- **Service — `rasn_runtime_rpc_service`.** A serverlet that registers a
  per-module RPC handler and dispatches into the service store. Hosted by the
  aggregate `rasn.runtime` app or by a standalone `rasn.runtime.<module>` app.
- **State — `rasn_runtime_service_store`.** Owns the actual module objects. It is
  the authority for module state and is where server-side idempotency lives.

## 4. The module API contract (envelope)

Every operation, regardless of provider, is expressed as one request/response pair
so a call can travel unchanged over a direct call, an LPC, or an RPC:

```cpp
struct rasn_runtime_request {
    uint32_t    schema_version;
    std::string module;       // e.g. "resource_budget"
    std::string operation;    // e.g. "reserve", "put", "mirror_state:usage"
    std::string key;
    std::string payload;      // module-specific encoded fields
    std::string request_id;   // optional idempotency id (see §7)
    uint32_t    route_partition; // optional exact shard for fan-out reads
    std::string auth_token;   // optional RPC shared-token credential (see §9)
};

struct rasn_runtime_response {
    uint32_t    schema_version;
    bool        ok;
    std::string error;
    std::string module;
    std::string operation;
    std::string key;
    std::string payload;
};
```

Design notes:

- **Coarse-grained on purpose.** The network is a leaky abstraction: latency,
  partial failure, and reordering are real. Operations are whole module actions
  (reserve a budget, put a blackboard entry), not chatty getters/setters, so one
  app action is one round trip.
- **Serialization is append-only.** `marshall`/`unmarshall` write fields in order;
  new fields (like `request_id`, `route_partition`, and `auth_token`) are
  appended at the end so the encoding stays forward-compatible within a build.
- **Typed schemas are future work.** Today the payload is a generic encoded field
  map. The roadmap replaces it with generated per-module RPC schemas while keeping
  the same envelope shape.

## 5. Provider model

| Provider | Where modules run | Module API path | State service | Select with |
| --- | --- | --- | --- | --- |
| `local` | In-process (direct call, or intra-node LPC under a node) | LPC / direct | disabled | `rasn_runtime_provider = local` (or `embedded`) |
| `distributed` | Remote service node(s) | rDSN RPC | enabled (mirrors writes) | `rasn_runtime_provider = distributed` |
| `hybrid` | Per module: local **or** remote | LPC or RPC per module | enabled for remote-routed modules | `rasn_runtime_provider = hybrid` |

- **Selection** mirrors the rDSN provider-by-config-name pattern:
  `normalize_rasn_runtime_provider_name` maps friendly aliases
  (`embedded`/`in-process` → `local`; `rdsn`/`remote` → `distributed`;
  `mixed`/`per-module` → `hybrid`), and `create_rasn_runtime` constructs the
  matching provider. Apps are unaffected.
- **Hybrid routing** reads `[rasn.service] <module>_mode` (falling back to
  `rasn_runtime_default_mode`); `remote`/`distributed`/`rpc` route over RPC,
  everything else stays local. This lets an operator co-locate latency-sensitive
  modules with the app while pushing shared stateful modules onto their own nodes.
- **State mirroring and hydration.** The distributed provider writes each
  successful state mutation to the shared state service (`put_state`); the hybrid
  provider mirrors only modules it routes remotely, matching each module's actual
  owner. A runtime module service queries `rasn_runtime_state_prefix` on startup,
  filters records by hosted module, sorts by state sequence, and replays typed
  hydration operations before opening its RPC handlers. If hydration is enabled
  and the configured state service cannot be queried, the service retries the query
  while a co-located state service is still registering its handlers — up to
  `rasn_runtime_state_hydration_max_attempts` (default 20, honored as configured
  with no hidden cap) spaced by `rasn_runtime_state_hydration_retry_backoff_ms`
  (default 250 ms) — and fails closed instead of serving empty state only after
  that readiness budget is exhausted or on a non-transient error. This keeps a cold multi-process start,
  where `rasn.state` may still be coming up, from aborting on the first miss while
  still refusing to serve empty state; set `rasn_runtime_state_hydration_enabled = false`
  only for intentionally cold local experiments. This gives standalone module services
  restart recovery from the mirror. When
  `rasn_runtime_state_watermark_enabled` is true, each mirrored mutation also
  writes a per-module `watermark` record containing the committed state-record
  sequence; hydration verifies those watermarks before replay when
  `rasn_runtime_state_watermark_verify_enabled` is true. Any hosted module that
  has mirrored data records must also have a valid watermark, so a torn,
  incomplete, or pre-watermark mirror fails closed instead of silently serving
  partial state.
  Operators can run `codepilot state compact` (or the equivalent SREPilot
  command) with an optional `--prefix` to query the mirror, verify existing
  watermarks, checkpoint the configured recovery image, and compact its journal.
  Supplying an explicit checkpoint path creates a verified export but deliberately
  retains the configured recovery journal because that export is not necessarily
  the next startup image. `state migrate <checkpoint> [--prefix ...]` preflights a
  sequence-preserving import into the configured state service and defaults to a
  dry run; `--apply` performs resumable CAS writes and restores the source sequence
  floor. `state prune --prefix ... --max-sequence ...` is a separate, default-dry-run
  **logical delete** for an explicitly obsolete namespace. It is not part of
  compaction, and operators must not prune the active runtime-mirror prefix merely
  because its records are present in a checkpoint: hydration still queries those
  live records. A prefix beginning with `-` uses `--prefix=-tenant/...`; use `--`
  before a leading-dash query prefix or checkpoint/recovery path. Locally routed
  hybrid modules are intentionally not mirrored, so
  flipping a module from `local` to `remote` is still a **cold migration** unless
  an operator exports/imports that module's prior local state.

## 6. Endpoints, placement, and topology

- **Endpoint resolution** (`resolve_rasn_runtime_endpoint`): an operator-declared
  `<module>_uri`/`<module>_host`/`<module>_port` (or shared
  `rasn_runtime_uri`/`rasn_runtime_host`/`rasn_runtime_port`) is authoritative
  and is used without registry discovery.
  This makes an app's configured runtime address deterministic. For modules left
  unconfigured, the distributed path queries `rasn.registry` when
  `rasn_runtime_registry_discovery_enabled` is true. Sharded modules first query
  the exact `rasn.runtime.<module>.shard.<n>` capability, then fall back to
  `rasn.runtime.<module>`; non-sharded modules query only the module capability.
  If discovery is empty or unavailable, routing falls back to the static
  localhost/default-port endpoint (`27107` for the aggregate service). A module
  can therefore point at an explicitly configured service, a registry-selected
  live service, a standalone role, or a URI-backed rDSN cluster independently.
- **Shard-aware placement.** Modules whose descriptor is `sharded`
  (`agent_message_bus`, `resource_budget`, `blackboard`) can set
  `<module>_shard_count > 1`. Mutating and keyed read operations route by the
  module's natural key (`message_id`, budget `scope`, blackboard `key`) using the
  existing stable FNV-1a hash, including the empty string so its historical
  hash/modulo placement is unchanged. That full hash is now carried in the rDSN RPC
  `partition_hash`: when the endpoint is a module-level `dsn://` URI, the core
  invokes `dist::partition_resolver` to map it to the current partition replica
  group and invalidates the resolver cache after access failure. Static and
  registry endpoints retain deterministic hash/modulo selection as compatibility
  fallbacks because they are not meta-server tables. Fan-out reads such as
  snapshots query every URI-backed partition even though all share one logical
  URI, or each distinct static endpoint, then merge the typed results. Static
  per-shard endpoint knobs are
  `<module>_shard_<n>_{uri,host,port}` with the normal per-module endpoint as the
  fallback and remain authoritative per-shard overrides. Resolver-backed clients
  must configure `<module>_shard_count` equal to the meta-server table's partition
  count; the public resolver API does not expose that count for startup
  cross-validation. rASN therefore emits a once-per-process warning when a sharded
  module shares one resolver URI across partitions. Explicit hosted-shard ingress
  checks can reject some mismatched routes, but operators must treat any count
  mismatch as invalid configuration. For
  registry-routed shards, a module service can set
  `<module>_hosted_shards = 0,2` (or `all`) so it publishes explicit
  `rasn.runtime.<module>.shard.<n>` capabilities. If no shard-specific descriptor
  is live, clients retain the older module-level fallback that maps sorted live
  descriptors across configured shard indexes.
- **Registry publication.** Runtime module service apps publish one lease-tracked
  descriptor per hosted module when
  `rasn_runtime_registry_registration_enabled` is true. The base descriptor
  capability is `rasn.runtime.<module>`. Its endpoint uses
  `rasn_runtime_advertise_host` (or `<module>_advertise_host`) when configured and
  otherwise retains the app's rDSN primary address; the real listen port is
  preserved. When `<module>_hosted_shards` or `<module>_shard_index` is configured
  for a sharded module, the descriptor also advertises
  `rasn.runtime.<module>.shard.<n>` for those partitions. A heartbeat keeps the
  lease live and stop unregisters it. Aggregate services therefore publish all
  eleven base capabilities, while standalone services publish only their module
  plus any explicitly hosted shard labels.
- **Standalone roles.** Each module has a `rasn.runtime.<module>` role and a
  standalone app; launch it with a reachable state service for hydration (for
  example `codepilot serve config.rasn.ini "rasn.state;resource_budget"`, or a
  remote `[rasn.service] state_uri`). The app-list is normalized to the role.
  This is what makes modules independently deployable.
- **Compatibility aliases.** App-list normalization and app registration still
  accept the earlier `rasn.common.*` roles, so existing distributed-runtime
  app-list scripts keep working while new deployments use the `rasn.runtime.*`
  names.
- **Topology reporting.** `describe_topology()` renders, per module, its routing
  (local/remote), resolved endpoint source (`registry:`, `static:`, or
  resolver-backed `resolver:`; sharded modules list
  `shardN=...#shard=N/count`), standalone role, intended consistency model, and
  statefulness — the operator's view for validating a multi-node layout.

### 6.1 Configuration file layout

Two things are fully **config-driven** and independent of the command line:

1. **Placement** — where an app's runtime modules run — comes from
   `[rasn.runtime] rasn_runtime_provider` (`local` | `distributed` | `hybrid`) in
   the app's own `config.ini`. There is **no `--dsn` mode switch**; the same
   binary is a local app or a thin client of a remote runtime purely by config.
2. **Deployment role** — whether a process is an *app* or a standalone *runtime
   host* — is selected by the first CLI token: `./app <command>` runs the app;
   `./app serve` launches the standalone runtime host. `serve` is a **role**, not
   a placement selector (`--dsn` is kept only as a deprecated alias of `serve`).

Configuration is **three files**, composed with rDSN's stock, mandatory
`@include` (the rASN plugin does **not** modify rDSN's config parser):

| File | Audience | Contents |
| --- | --- | --- |
| `config.ini` | applications | **One per app** (`apps/<app>/config.ini`), thin and self-contained. rDSN bootstrap (`[modules]` + `[core]`), the app's own `[apps.rasn.<app>]` gateway, the runtime **placement** (`[rasn.runtime]`), and — commented by default — the LLM endpoint (`[rasn.model]`) and, for `distributed`, the remote runtime **address** (`[rasn.service] rasn_runtime_host`/`_port`). It carries no runtime-internal `[rasn.service]` endpoint map beyond that optional client address and no `[apps.rasn.*]` service apps. It **may** optionally `@include config.rasn.defaults.ini` near the top to tune an in-process (`local`) runtime; that line is commented out, so a plain app runs on built-in defaults. |
| `config.rasn.ini` | runtime hosts | **The standalone runtime host config** (`src/plugins/rasn/config.rasn.ini`), loaded by `./app serve`. rDSN `[modules]`/`[core]`/thread pools, the `[rasn.service]` endpoint map + `[rasn.rpc]` timeouts, and every `[apps.rasn.*]` **service/module** app. It hosts **services only** and carries **no** app-gateway section — so it is self-contained and needs no reverse include of any app config. It **ends with `@include config.rasn.defaults.ini`** to pull in the shared module tuning. |
| `config.rasn.defaults.ini` | shared | **Shared module tuning defaults** (`[rasn.model]`, `[rasn.tool]`, `[rasn.overload]`, `[rasn.policy]`, `[rasn.state]`, `[rasn.workflow]`, …) — identical whether a module runs in-process in an app or inside a runtime host. Included by `config.rasn.ini`, and optionally by a local app's `config.ini`. It holds **no** infra, `[rasn.service]`, or `[apps.*]` deployment sections, so it is safe for either side to include. |

**Include semantics (stock rDSN, last-write-wins).** An `@include` is expanded
*inline*: the included file's keys override same-named keys **before** the include,
and keys **after** the include override the included file. This is why the
inversion is clean — a runtime host includes the shared defaults last (defaults
win over the host's own earlier scaffolding only where intended), while a local
app can include the defaults near the top and still override any of them below.

Who loads what:

- **Local app (`./app <command>`, `rasn_runtime_provider = local`)** loads only
  its thin `config.ini` and builds the runtime modules **in-process** on built-in
  defaults (or on `config.rasn.defaults.ini` if the app opts into the include). It
  runs the command on a node-less fast path and sees no service/deployment config.
- **Thin client → remote runtime (`./app <command>`,
  `rasn_runtime_provider = distributed`)** still loads only its `config.ini`, but
  the entry point starts a lightweight rDSN client node (`mimic`) and attaches to
  it so remote module RPC has a node context, then dials the runtime address from
  `[rasn.service]`. It never deploys any service config. Note the two placement
  axes are **independent**: the thin client enables the runtime-module provider
  (the 11 nucleus modules over RPC) but **not** the core service-graph RPC clients
  (`rpc_clients_enabled()` stays false — only `serve`/`--dsn` hosts enable those),
  so the client's own `model`/`state`/`tool`/`workflow` adapters run **in-process**
  against its local config, not against the deployed `rasn.state`/`rasn.registry`
  services. Consequently a distributed-client **`selftest` validates runtime-module
  reachability over RPC but not deployed-core connectivity** — its output now labels
  the two axes separately (`core service graph: in-process (inline)` vs
  `rDSN RPC`, and `runtime modules: local|distributed|hybrid`) so a passing inline
  result is not misread as end-to-end fleet validation. (Making the thin client a
  full core-service RPC client is deferred to the planned distributed-mode redesign;
  see §13.3.)
- **Standalone runtime host (`./app serve`)** loads `config.rasn.ini` beside the
  binary and starts the services-only `-app_list`
  (`rasn.registry;rasn.state;rasn.runtime;…`) headless — no app gateway. A
  missing runtime-host config is a clear startup error; it never falls back to the
  thin app config and sleeps with an empty service fleet. An explicit
  `./app serve <config> [app_list]` selects another host config/role set; the
  optional `app_list` is validated against the config's effective `[apps.*]`
  `run`/`count` settings and optional `@instance` selectors. An override that
  would start **no runnable instance** (unknown/disabled app, zero count, or
  out-of-range instance) fails clearly instead of starting a host that binds
  nothing and sleeps forever. Ignored selectors in a partially valid list are
  warned; malformed instance selectors are rejected.

The runtime host is an ordinary rDSN service process: run it co-located on the
same machine as the apps (clients dial `127.0.0.1`) or on a dedicated node
(clients dial its routable address). Either way an app only ever needs the
runtime's address — never its internal `[rasn.service]`/`[apps.*]` topology.

**Endpoint resolution — explicit wins over discovery.** When an app configures an
explicit runtime address (`[rasn.service] rasn_runtime_host`/`_uri`, or a
`rasn_runtime_port` or per-module/per-shard variant), that endpoint is
**authoritative**: the client
dials exactly it and does **not** consult registry discovery, so it always reaches
the runtime you pointed it at. Registry discovery
(`rasn_runtime_registry_discovery_enabled`, default on) is used only for modules
left unconfigured, with the static localhost endpoint as the final fallback.
Symmetrically, a runtime host registers its modules under the address in
`rasn_runtime_advertise_host` (per-module `<module>_advertise_host`) when set —
`127.0.0.1` for co-located clients, a routable IP/DNS name for multi-machine — so
discovery never hands back an unreachable auto-selected NIC address. Leaving it
blank keeps the previous behavior (register the rDSN `primary_address()`). Invalid
or unresolvable advertise hosts are rejected and the affected module is not
published rather than registering `0.0.0.0` as healthy.

CMake binplaces all three files beside each executable: the app always consumes
only its own `config.ini`, while `serve` consumes `config.rasn.ini` plus
`config.rasn.defaults.ini`. A dedicated runtime deployment copies the latter two
with the binary. They remain one shared host config/defaults pair in the source
tree, so app configs never duplicate runtime topology.

## 7. Resilience and idempotency (runtime-owned)

Because the network is on the path in `distributed`/`hybrid`, the runtime — not
the apps — owns the failure contracts. All of this lives in `invoke_remote_module`
/ `ping_remote_module`, so every module and provider inherits it uniformly.

```mermaid
sequenceDiagram
    participant Facade
    participant Remote as invoke_remote_module
    participant Breaker as circuit breaker (module + endpoint)
    participant Svc as module service

    Facade->>Remote: call_module_api(request)
    Remote->>Breaker: allow(now)?
    alt breaker open
        Breaker-->>Remote: deny
        Remote-->>Facade: error (short-circuit, no RPC)
    else allowed
        Remote->>Remote: stamp request_id (stable across retries)
        loop up to max_attempts
            Remote->>Svc: RPC(request, timeout)
            alt ok
                Svc-->>Remote: response
                Remote->>Breaker: report(success)
                Remote-->>Facade: response
            else transient error
                Svc-->>Remote: ERR_TIMEOUT/NETWORK/BUSY/...
                Remote->>Remote: backoff (linear * attempt)
            end
        end
        Remote->>Breaker: report(failure) if exhausted
        Remote-->>Facade: error
    end
```

- **Timeouts & bounded retries.** Per-module RPC timeout (falls back to
  `[rasn.rpc] timeout_ms`), a bounded retry count on *transient* transport errors
  (`ERR_TIMEOUT`, `ERR_NETWORK_FAILURE`, `ERR_NETWORK_INIT_FAILED`, `ERR_BUSY`,
  `ERR_CAPACITY_EXCEEDED`, `ERR_TRY_AGAIN`), and linear backoff. Non-transient
  errors fail fast.
- **Per-endpoint circuit breaker.** A process-global `circuit_breaker_registry`
  keyed by `module + shard + resolved endpoint`. For a URI this is intentionally
  the logical URI/partition: rDSN first invalidates and retries a failed physical
  replica internally, and rASN records a failure only if that resolver path returns
  a terminal error. On repeated transport failures
  the breaker opens and calls short-circuit for a cooldown before a single
  half-open probe is admitted, so one dead module endpoint cannot stall every
  request or amplify load. Health pings check every configured shard, consult
  `is_open`, and skip a probing round trip while open. Reuses the same
  `circuit_breaker` engine as the model gateway and remote-agent dispatch. With
  `[rasn.coordination] shared_breaker_enabled = true`, the registry resolves a
  coordination context for the current rDSN app and delegates to the fenced
  cluster-global backend in §13.7.
- **Idempotent retries.** A retry after a *lost reply* is dangerous: the server may
  have already applied the write. So `invoke_remote_module` stamps each logical
  call with a unique `request_id` that is **stable across its own retries**. For
  mutating operations, the service store installs an in-flight placeholder keyed
  by the full request signature plus `request_id` (including any route partition)
  before applying the mutation; an identical concurrent duplicate waits for the
  first response and then returns it, while an accidental id collision on another
  key, shard, or payload is treated as a distinct request. Read-only operations
  are not retained in the dedup cache. The
  window is bounded by `rasn_runtime_dedup_capacity` and
  `rasn_runtime_dedup_ttl_ms`; duplicate waiters are additionally capped by
  `rasn_runtime_dedup_wait_timeout_ms` so a slow or failed owner cannot occupy
  RPC handler threads indefinitely. Only successful responses are cached; failed
  responses clear the placeholder and let later retries re-execute. Dedup
  activity is exposed through metrics
  (`rasn_runtime_dedup_{hit,miss,wait,evicted,expired}_total`). This suppresses
  lost-reply double-apply within one service process; it is not cross-process
  exactly-once.
- **Strict vs. degrade.** Facade calls return explicit errors to their caller;
  when `rasn_runtime_strict` is true, a successful in-memory mutation whose
  state mirror or watermark write fails is surfaced as a failed facade call
  instead of being reported as success with only a warning. In non-strict mode,
  mirror failures remain degradation warnings.

### 7.1 Core-service client resilience

The failure contracts above cover the 11 runtime **modules** reached through
`invoke_remote_module`. rASN also depends on a handful of older, always-on **core
services** reached over ordinary rDSN RPC — `rasn.state`, `rasn.workflow`,
`rasn.observability`, and the `rasn.registry` discovery lookup that gates request
routing. Historically each of those made a single one-shot `::dsn::rpc::call`, so a
transient blip on any of those hops surfaced as a hard failure with no breaker and
no retry, even though the module path was fully hardened. That asymmetry is closed
by the shared `resilient_rpc_call` helper in `rpc_resilience.h`, which gives every
cross-node core dependency the *same* policy, reusing the shared `circuit_breaker`
engine rather than introducing another bespoke mechanism:

- **Per-endpoint circuit breaker.** A `circuit_breaker_registry`
  (`global_rasn_core_breakers()`) keyed by service + resolved endpoint. It is
  process-local by default. When `[rasn.coordination] shared_breaker_enabled =
  true`, the same registry API delegates to the lock-serialized authoritative
  backend described in §13.7, so every participating process observes one breaker
  state. While an endpoint is unhealthy, calls short-circuit with `ERR_BUSY`
  without touching the dependency, exactly as the module path does.
- **Idempotency-aware retries.** Errors that prove the request never reached
  application dispatch (`ERR_NETWORK_INIT_FAILED`, `ERR_BUSY`,
  `ERR_CAPACITY_EXCEEDED`, `ERR_TRY_AGAIN`) are retried for *any* operation.
  `ERR_NETWORK_FAILURE` can be synthesized when an in-flight connection drops
  after the server applied the request, so it is ambiguous like `ERR_TIMEOUT`;
  both are retried **only** for operations the caller declares idempotent. Reads
  and key-overwrite writes are idempotent; compare-and-swap (`put_conditional`)
  and starting a workflow run are not, so a lost reply on those fails fast
  instead of risking a double-apply. Linear backoff (`backoff_ms * attempt`)
  separates retries; total attempts are `rasn_core_rpc_retries + 1`.
- **Config.** `[rasn.service] rasn_core_rpc_retries`, `rasn_core_rpc_backoff_ms`,
  `rasn_core_rpc_breaker_enabled`, `rasn_core_rpc_breaker_failures`, and
  `rasn_core_rpc_breaker_open_ms`. The RPC timeout reuses `[rasn.rpc] timeout_ms`.
  The in-process (non-distributed) path is unchanged: resilience wraps only the
  RPC-client branch, so single-process runs pay nothing.

With shared breakers disabled, this remains per-service-process resilience. With
them enabled on the ZooKeeper coordination backend, breaker transitions and the
single half-open probe are cluster-global. Retry idempotency and module dedup are
separate contracts: this does not make mutations cross-process exactly-once and
does not replace the replicated backends discussed in §8 and §13.

## 8. Consistency models

Stateful modules are not all the same, so each declares an **intended** consistency
model (`rasn_runtime_module_descriptors()`). Two backings are available: the
lightweight standalone in-memory service roles, or the native type-1 tables in
`config.rasn.runtime.replicated.ini` (§13.15). The latter commit every mutation
through rDSN quorum replication and recover each partition from framework
checkpoints; they do not depend on the `rasn.state` mirror.

| Model | Meaning | Modules |
| --- | --- | --- |
| `singleton` | One authoritative instance; small control-surface state. | `human_interaction`, `sandbox_runtime` |
| `sharded` | Partition state by a natural key; scale horizontally. | `agent_message_bus` (topic), `resource_budget` (scope), `blackboard` (key) |
| `replicated` | Ownership/ledger state that needs consensus for correctness/HA. | `agent_control_plane`, `task_orchestration_kernel`, `determinism_ledger`, `capability_directory`, `recovery_supervisor`, `contract_verifier` |

Deterministic partition routing spreads sharded keys across independent module
partitions. A module-level `dsn://` endpoint delegates key-hash-to-replica-group
resolution, failed-access invalidation, and retry to rDSN's native
`partition_resolver` (§6). The checked-in profile uses four partitions for
`agent_message_bus`, `resource_budget`, and `blackboard`, one partition for every
other module, and three replicas per partition.

`describe_topology()` reports `actual=rdsn_type1_replica_group` for remote modules
when `rasn_runtime_native_replication_enabled = true` (with per-module overrides
for mixed deployments); standalone/local modules continue to report
`actual=single_writer_in_memory`.

> **Standalone alternative — one active writer per shard.** Static and registry
> endpoints can still target the lightweight standalone module roles. Multiple
> instances of the same standalone shard do **not** replicate state. When
> `rasn_runtime_ownership_gate_enabled = true`, each standalone module service
> acquires ownership of every module (or hosted shard) it serves through the
> coordination module (§13.7) — reusing rDSN `distributed_lock_service` — **before**
> hydrating its state and opening its RPC API, and fails closed if another node
> already owns that module/shard. Acquiring ownership *before* hydration means a
> standby that waits for the active owner to release cannot open handlers on a
> snapshot the active owner has since superseded — once it owns the resource no
> other node writes, so its hydration observes the latest committed state. The gate
> is wired into `rasn_runtime_app::start()` (acquire → hydrate → `open_service`) and
> defaults **off**: the `inproc` and `simple` backends coordinate only within one
> process/facade, so real cross-process single-writer enforcement needs
> `provider = zookeeper`.
> Together with `rasn.state.replicated`, that path provides durable elected
> active/standby failover. Prefer the native module tables when direct quorum commit,
> automatic primary failover, and replica learning are required.
>
> **Shard-ingress enforcement (always on when sharded).** Independently of the
> ownership gate, a runtime service that hosts only a subset of a sharded module's
> partitions (`<module>_hosted_shards` / `<module>_shard_index`) now rejects any
> request that routes to a shard it does **not** host, before that request reaches
> the module store. This closes the gap where a stale registry entry, a static
> endpoint, or a direct client could send a shard-1 request to the shard-0 owner and
> mutate state the process does not own. Services that host the whole module (the
> default single-process case) admit every request, so this is a no-op outside
> multi-shard deployments.

## 9. Discovery and security

- **Discovery over static endpoints.** Static `host`/`port`/`uri` config remains
  the baseline and fallback. The distributed/hybrid remote path now resolves
  module services through the existing `rasn.registry` capability API first, so a
  client can find a live service instance by module capability instead of carrying
  every module's endpoint in its own config. Discovery uses registry health/lease
  filtering and the same bounded registry RPC timeout as dynamic agent
  registration.
- **Failover boundary.** Registry discovery selects a live descriptor at call time,
  and the circuit breaker is keyed by the resulting endpoint. A module can
  therefore fail over to a different registered endpoint after the registry stops
  returning the unhealthy descriptor, while static config remains the deterministic
  fallback when registry is unavailable.
- **HA registry authority (opt in).** `[rasn.registry] shared_state_enabled = true`
  replaces a registry frontend's private map with the existing rDSN
  `meta_state_service` through the coordination facade. Every frontend can serve
  reads; one frontend owns `rasn.registry.primary` through
  `distributed_lock_service` and performs registration, heartbeat, unregister,
  live-record epoch promotion, static-descriptor reconciliation, and lease expiry.
  Each new writer copies the previous committed live snapshot below its
  grant-version children, omits tombstones, reconciles static records, and only
  then publishes a schema-versioned epoch marker. Readers consume one exact
  committed epoch and perform one authoritative ZooKeeper lock-tree validation
  after the read; that final check proves the selected epoch remained authoritative
  through the operation, so a delayed former leader cannot overwrite or introduce
  visible records. This is epoch fencing rather than a transactionally consistent
  snapshot: the active writer can update individual records concurrently within
  its current epoch.
  The active writer also keeps a bounded epoch window, deleting old per-agent
  children before their commit markers and retrying transient cleanup failures.
  Shared-mode backend/record failures are returned as errors, never converted into
  a successful empty roster.
- **HA frontend routing.** `[rasn.service] registry_addresses` builds an rDSN group
  address after `registry_uri` precedence and before legacy host/port fallback.
  Registration, heartbeat, unregister, query, list, CLI diagnostics, readiness, and
  runtime-module discovery all use the same helper and bounded retry policy. rDSN
  advances a failed group leader on transport failure; a standby's typed
  `registry_not_primary` response explicitly advances it before retry. Configure
  `client_max_attempts` at least as high as the frontend count.
- **Cross-node standalone-module auth.** Distributed runtime module RPC can require a shared
  service token with `[rasn.service] rasn_runtime_auth_enabled = true` and
  a deployment-supplied `rasn_runtime_auth_token`. Remote providers stamp the token onto the
  envelope before an RPC; `rasn_runtime_rpc_service` verifies it before dispatch
  and clears it before module handlers see the request. Invalid or missing tokens
  are rejected as normal runtime responses and counted by
  `rasn_runtime_auth_rejected_total`. Local/direct and intra-node LPC paths bypass
  this gate, so trusted single-process experiments remain unchanged. The token is
  a prototype shared-secret control, not transport encryption; deploy it only on
  trusted networks or under an encrypted/authenticated transport. Native type-1
  module apps reject this mode at startup: interception commits writes before the
  app handler can authenticate, and node-local token drift could otherwise make
  one decree apply differently across replicas. Protect native tables at the
  transport/network layer until identity can be checked before replication.

## 10. Deployment examples

Aggregate (all modules behind one service):

```ini
[rasn.runtime]
rasn_runtime_provider = distributed
[rasn.service]
rasn_runtime_registry_discovery_enabled = false
rasn_runtime_host = modules-host
rasn_runtime_port = 27107
```

Per-module placement (standalone services on their own nodes):

```ini
[rasn.runtime]
rasn_runtime_provider = distributed
[rasn.service]
rasn_runtime_registry_discovery_enabled = false
resource_budget_host = budget-node
resource_budget_port = 27115
blackboard_uri      = dsn://meta-server:34601/rasn-blackboard
```

Registry-backed placement (services register capabilities, clients discover them):

```ini
[rasn.runtime]
rasn_runtime_provider = distributed
[rasn.service]
rasn_runtime_registry_discovery_enabled = true
rasn_runtime_registry_registration_enabled = true
registry_host = registry-node
registry_port = 27100
; Runtime-host config: publish the address clients can actually dial.
rasn_runtime_advertise_host = modules-host
```

HA registry frontends (one registry process per endpoint, all sharing ZooKeeper):

```ini
; config.rasn.ini (host topology)
[rasn.service]
registry_addresses = registry-a:27100,registry-b:27100,registry-c:27100

; config.rasn.defaults.ini (loaded last by every registry frontend)
[rasn.registry]
shared_state_enabled = true
shared_state_prefix = registry/v1
leader_resource = rasn.registry.primary
client_max_attempts = 3

[rasn.coordination]
provider = zookeeper
lock_namespace = /rasn/locks
state_namespace = /rasn/state

[zookeeper]
hosts_list = zk-1:2181,zk-2:2181,zk-3:2181

[apps.rasn.registry]
; Use the matching port on each process and select only rasn.registry in its
; app-list. Do not use count>1 as a substitute for process/node redundancy.
ports = 27100
pools = THREAD_POOL_DEFAULT,THREAD_POOL_META_SERVER,THREAD_POOL_DLOCK

[threadpool.THREAD_POOL_META_SERVER]
partitioned = false
worker_count = 2

[threadpool.THREAD_POOL_DLOCK]
partitioned = true
```

All frontend processes open the existing registry RPC contract. Standbys serve
shared reads and reject mutations with `registry_not_primary`; clients rotate to
the elected writer. A writer crash is detected by the ZooKeeper-backed lock lease,
one standby acquires a greater fencing token, reconciles `[rasn.agent.*]`, and
continues from the shared descriptor/heartbeat records. A ZooKeeper build is
mandatory: requesting shared registry state while the resolved coordination
provider is not `zookeeper` aborts registry startup instead of silently falling
back to process memory.

Service-to-service auth for runtime RPC:

```ini
[rasn.runtime]
rasn_runtime_provider = distributed
[rasn.service]
rasn_runtime_auth_enabled = true
; supply rasn_runtime_auth_token from a deployment-specific config overlay
rasn_runtime_auth_token =
```

Key-sharded placement (static endpoints, deterministic by key):

```ini
[rasn.runtime]
rasn_runtime_provider = distributed
[rasn.service]
rasn_runtime_registry_discovery_enabled = false
blackboard_shard_count = 2
blackboard_shard_0_host = blackboard-a
blackboard_shard_0_port = 27117
blackboard_shard_1_host = blackboard-b
blackboard_shard_1_port = 27127
resource_budget_shard_count = 2
resource_budget_shard_0_uri = dsn://meta-server:34601/rasn-budget-a
resource_budget_shard_1_uri = dsn://meta-server:34601/rasn-budget-b
```

Meta-server partition resolution (client path; the referenced runtime module must
be deployed as a compatible rDSN table):

```ini
[modules]
dsn.dist.uri.resolver

[rasn.runtime]
rasn_runtime_provider = distributed

[rasn.service]
blackboard_shard_count = 4
blackboard_uri = dsn://rasn-cluster/rasn-blackboard

[uri-resolver.dsn://rasn-cluster]
factory = partition_resolver_simple
arguments = meta-1:27601,meta-2:27601
```

The module URI is one authoritative logical endpoint. rASN supplies the stable key
hash (or the small exact `route_partition` for fan-out/ping) to the RPC header;
modulo the same partition count both forms select the same partition. rDSN resolves
and refreshes the physical replica-group endpoint. Resolver failure is returned
rather than silently falling through to registry/static routing. Circuit breakers
remain keyed by logical module URI plus partition because the public call surface
does not expose the resolved physical replica; this is deliberate because rDSN
already invalidates and retries physical replicas before returning failure.
`config.rasn.runtime.replicated.ini` wires this path to eleven native type-1
tables, including matching four-partition counts for the three sharded modules;
those tables provide quorum durability, primary failover, checkpoint learning,
and meta-server placement (§13.15). Standalone URI/static endpoints retain the
single-writer behavior described in §8.

Registry-discovered shard ownership on a standalone module service:

```ini
[rasn.service]
rasn_runtime_registry_registration_enabled = true
blackboard_shard_count = 2
blackboard_hosted_shards = 0
```

Single-writer ownership across two nodes (active/standby for one shard). Deploy the
**same** stanza on two nodes that both host `blackboard` shard 0. With a cross-process
coordination backend, exactly one node acquires ownership and opens its RPC API; the
other **fails closed** at startup (`refusing to open module APIs`). Failing closed
aborts the losing process — rDSN asserts on a non-`ERR_OK` app start — so the loser
does **not** stay alive on its own. To run a true active/standby pair, put both nodes
under an external supervisor (systemd `Restart=always`, a Kubernetes Deployment, etc.)
that restarts the loser; on each restart it retries ownership and wins once the active
node releases (clean stop) or its lease lapses (crash). Raising
`rasn_runtime_ownership_acquire_max_attempts` lets the standby retry in-process first,
so it can ride out a brief ownership handover during a rolling restart without a full
process restart:

```ini
[rasn.service]
rasn_runtime_registry_registration_enabled = true
blackboard_shard_count = 2
blackboard_hosted_shards = 0
; Acquire single-writer ownership of the hosted shard before serving writes.
rasn_runtime_ownership_gate_enabled = true
; Optional: retry a contended acquire a few times (spaced by the backoff below)
; before failing closed, so a brief handover does not force a supervised restart.
rasn_runtime_ownership_acquire_max_attempts = 5
rasn_runtime_ownership_acquire_retry_backoff_ms = 1000
rasn_runtime_request_drain_timeout_ms = 30000

[rasn.coordination]
; inproc coordinates only within one process; use zookeeper for real cross-node
; single-writer. Requires a --build_plugins build + the THREAD_POOL_META_SERVER and
; THREAD_POOL_DLOCK pools (see below and §13.7).
provider = zookeeper
lock_namespace = /rasn/locks
acquire_timeout_ms = 5000
operation_timeout_ms = 5000

[zookeeper]
hosts_list = zk-1:2181,zk-2:2181,zk-3:2181
timeout_ms = 8000

; The zookeeper backend needs two thread pools declared AND listed in the hosting
; app's `pools` (e.g. pools = THREAD_POOL_DEFAULT,THREAD_POOL_META_SERVER,THREAD_POOL_DLOCK).
; THREAD_POOL_META_SERVER carries the rASN facade's grant/lease callbacks;
; THREAD_POOL_DLOCK carries the zookeeper lock provider's own lock tasks and MUST be
; partitioned (the provider asserts single-thread access per lock). The HA registry
; checks this wiring before provider initialization; other ZooKeeper-backed app
; roles that omit THREAD_POOL_DLOCK can still abort at provider startup with
; "pool THREAD_POOL_DLOCK not ready".
[threadpool.THREAD_POOL_META_SERVER]
partitioned = false
worker_count = 2

[threadpool.THREAD_POOL_DLOCK]
partitioned = true
```

> **Automated multi-process validation.** A Linux harness is binplaced beside
> CodePilot as `run_multinode.py`, with `test.sh` as its standard rDSN test entry
> point. It allocates loopback ports dynamically, gives every process separate
> config/data/log directories, starts each child in its own process group, and
> preserves the complete artifact tree on failure. The always-on scenarios prove:
> (1) explicit endpoint routing reaches all 11 runtime modules in a separate process;
> and (2) registry-only discovery resolves every module to a `registry:` endpoint
> while port 27107 (the static fallback) is deliberately reserved by a non-runtime
> listener, preventing a false pass. Run it from a Linux build with
> `builder/bin/codepilot/test.sh`.
>
> The same harness runs the active/standby ownership handoff when
> `RASN_MULTINODE_ZK_HOSTS=zk-1:2181,zk-2:2181,zk-3:2181` is set. It starts a shared
> state service plus two full runtime hosts under one unique ZooKeeper lock
> namespace, confirms the active serves all modules while the contending standby
> fails `runtime health`, terminates the active by process group, and confirms the
> still-retrying standby acquires ownership, hydrates, and begins serving. This
> scenario requires a `--build_plugins` build and a reachable ZooKeeper ensemble.
> `inproc` and `simple` cannot validate cross-process ownership.


State mirror durability watermarks:

```ini
[rasn.service]
rasn_runtime_state_hydration_enabled = true
rasn_runtime_state_watermark_enabled = true
rasn_runtime_state_watermark_verify_enabled = true
```

Compact the verified mirror into the configured recovery checkpoint/journal
baseline (omit the path for actual journal compaction):

```bat
codepilot.exe state compact --prefix rasn/runtime
```

Create a verified export while attached to the source state service. The export
path is local to that service process; run the command on the source host or copy
the file through the existing NFS workflow. Then point `[rasn.service]` at the
destination state service and preflight/apply from a CLI process that can read the
export. Quiesce **all writers to the destination state service** during `--apply`;
the tool rechecks the exact prefix and global sequence before and after each phase,
fails on concurrent target changes, and remains safe to resume:

```bat
codepilot.exe state compact --prefix rasn/runtime rasn/state/runtime-mirror-export.chkpt
codepilot.exe state migrate rasn/state/runtime-mirror-export.chkpt --prefix rasn/runtime
codepilot.exe state migrate rasn/state/runtime-mirror-export.chkpt --prefix rasn/runtime --apply
```

Delete only a namespace that the owning module/operator has declared obsolete.
The cutoff preserves any concurrently written newer record:

```bat
codepilot.exe state prune --prefix retired/runtime/namespace --max-sequence 42000
codepilot.exe state prune --prefix retired/runtime/namespace --max-sequence 42000 --apply
```

Direct/one-shot local commands recover the configured checkpoint/journal once per
service graph before any state read, write, prune, sequence barrier, or checkpoint.
A recovery error is cached and all later local state operations fail closed, so a
fresh CLI process cannot checkpoint or prune an empty in-memory store over durable
state. RPC-client mode delegates this responsibility to the state-service process.

If a mirror failure cannot be rolled back conclusively, the state store fail-stops
and leaves quarantine evidence beside or in place of the affected primary or
configured replica journal (`.quarantine`, `.quarantined`, and, when possible, an
in-journal marker). Stop the role, preserve those files for diagnosis, reconcile
both journal copies against a known-good checkpoint, and replace them with verified
recovery images before removing the external markers and restarting. There is
intentionally no
in-process "clear quarantine" path: once a live store observes external evidence it
remains quarantined until restart. Deleting only the sidecar is insufficient when
the journal itself contains the marker.

Hybrid (agents call most modules in-process; shared state on dedicated nodes):

```ini
[rasn.runtime]
rasn_runtime_provider = hybrid
[rasn.service]
rasn_runtime_default_mode = local
blackboard_mode      = remote
blackboard_host      = blackboard-node
resource_budget_mode = remote
resource_budget_host = budget-node
```

Launch one standalone module service (deploy both `config.rasn.ini` and
`config.rasn.defaults.ini` beside the binary; the runtime config is self-contained
and the app-list selects which service roles start):

```bat
codepilot.exe serve config.rasn.ini "rasn.state;resource_budget"
```

## 11. Roadmap

- **Distributed coordination — DELIVERED (§13.7):** a coordination module reusing
  rDSN `distributed_lock_service` (leader election / single-writer ownership) and
  `meta_state_service` (cluster-shared state), with `inproc`/`simple`/`zookeeper`
  backends. Ownership wiring is now landed: `rasn_runtime_app::start()` acquires
  ownership of each hosted module/shard before opening its RPC API when
  `rasn_runtime_ownership_gate_enabled = true` (default off; fail-closed on
  contention). Cluster-global breakers are also delivered with fenced state
  versions. Remaining: move admission/rate/cost/overload/dedup authorities onto
  safe leased or transactional primitives, and validate cross-process ownership
  and breaker propagation on ZooKeeper under multi-node.
- **HA discovery — DELIVERED (§13.14):** registry descriptors/leases can live in
  ZooKeeper-backed `meta_state_service`; a fenced
  `distributed_lock_service` primary serializes mutations while every frontend
  serves shared reads; and `registry_addresses` uses an rDSN group with bounded
  failover for every registry call family. Remaining deployment evidence is a
  multi-process ZooKeeper frontend-failover scenario in the Linux harness.
- Multi-process integration tests for the distributed/hybrid RPC paths.
- Generated typed RPC schemas per module (replace the generic envelope payload).
- **State lifecycle tooling — DELIVERED:** watermarks are written and verified;
  `state compact` checkpoints the configured recovery image and compacts only its
  covered journal; custom checkpoint exports retain that journal; `state migrate`
  preflights/resumes an exact-prefix, sequence-preserving checkpoint import into
  the configured state service (destination-only keys are conflicts, including
  when the source prefix is fully deleted); and cutoff-guarded
  `RPC_RASN_STATE_DELETE_PREFIX` plus durable
  journal tombstones support explicit obsolete-namespace deletion. Compaction
  intentionally does not delete current logical mirror records because those
  records remain the hydration query surface. `RPC_RASN_STATE_ADVANCE_SEQUENCE`
  restores a migrated checkpoint's sequence floor even when deletion left no live
  record at its last committed sequence. Standalone-state-to-native-module-table
  migration and export-file transport between hosts remain separate work under
  §13.15/the existing rDSN NFS operator workflow.
- **Direct runtime-module replication — DELIVERED (§13.15):** eleven module-specific
 `replicated_service_app_type_1` app types, separate read/write task codes,
 deterministic replay, checkpointed bounded dedup, decree checkpoints, learning,
 and a deployable multi-table profile. Remaining deployment evidence is a real
 multi-host failover/checkpoint-transfer run and migration tooling from standalone
 mirrors.
- URI-backed multi-node cluster coverage beyond the automated explicit,
  registry-discovery, and optional ZooKeeper ownership scenarios.
- End-to-end trace propagation across core/module RPC envelopes — **DONE** (§13.4).
- **Robustness hardening — DELIVERED (§13.8):** cold multi-process start no longer
  aborts on a state-hydration race (readiness retry), the `ERR_UNKNOWN` diagnostic-log
  storm is cleared in both modes, and LLM chat-completion parsing handles
  reasoning-only / error-envelope replies. Validated by unit tests, a single-box
  multi-process `--dsn` run, and a libfiu fault-injection campaign.
- **Local/distributed mode & config-include redesign — DELIVERED (§13.12):**
  placement is config-only via `rasn_runtime_provider`; `serve` is the independent
  runtime-host role; `config.rasn.ini` is self-contained and includes only shared
  `config.rasn.defaults.ini`; and distributed/hybrid app commands bootstrap a
  lightweight client node instead of co-hosting the service fleet.

See §13 for the full production-readiness audit that drives this roadmap, including
which gaps are resolved in code, mitigated client-side, or tracked as framework work.

## 12. Code map

| Concern | Location |
| --- | --- |
| Facade + provider base, envelopes, descriptors | `runtime_provider.h` |
| Providers (`local`/`distributed`/`hybrid`), transport helpers, breaker, dedup, service store, apps | `runtime_provider.cpp` |
| Core-service client RPC resilience (breaker + idempotency-aware retries) | `rpc_resilience.h` / `rpc_resilience.cpp` |
| Distributed coordination facade (ownership election + cluster-shared state) reusing rDSN `distributed_lock_service` / `meta_state_service` | `coordination_service.h` / `coordination_service.cpp` |
| Local + HA registry facade, fenced shared records, frontend group/retries | `agent_registry.h` / `agent_registry.cpp`; `[rasn.registry]` + `[rasn.service] registry_addresses` |
| RPC/LPC task codes | `rasn.code.definition.h` |
| Native runtime replica stores, type-1 apps, checkpoints, and deterministic dedup | `runtime_provider.h` / `runtime_provider.cpp`; `config.rasn.runtime.replicated.ini` |
| Reusable circuit breaker engine | `circuit_breaker.h` / `circuit_breaker.cpp` |
| Provider/endpoint/resilience config | `[rasn.runtime]` and the optional client endpoint in each app's `config.ini`; host `[rasn.service]`/`[rasn.rpc]` in `config.rasn.ini` |
| Coordination config | `[rasn.coordination]` in `config.rasn.defaults.ini` |
| Config file layout (thin app, standalone host, shared tuning defaults) | `config.ini` + `config.rasn.ini` + `config.rasn.defaults.ini` (see §6.1) |
| Tests | `tests/rasn_unit_tests.cpp`, `tests/rasn_coordination_test.cpp`; opt-in libfiu fault-injection harness `tests/fault_injection/run_fault_injection.sh` (§13.8) |

## 13. Production-readiness audit — findings, remediation, and status

This section merges a focused production-readiness audit of `src/plugins/rasn`
against the "fully distributed, reuse rDSN, no missing critical modules" bar this
document sets. Findings are grouped by three lenses and severity-ranked. Each
carries an explicit **status**:

- **RESOLVED (code)** — fixed in this codebase and behaviorally validated.
- **MITIGATED (code)** — materially improved client-side; full fix needs a
  framework-backed component below.
- **DOCUMENTED (roadmap)** — the correct rDSN-native design is identified and
  captured here; implementation is framework work that depends on rDSN subsystems
  (replication, meta-server/ZooKeeper, Thrift codegen) that a rASN-only change
  cannot honestly land or verify in isolation.

The guiding principle is **reuse rDSN, don't reinvent it**: wherever rASN grew a
bespoke mechanism, the audit names the rDSN facility that should back it.

### 13.1 Lens 1 — Is rASN *fully* distributed?

| # | Sev | Finding | Status |
| --- | --- | --- | --- |
| 1.1 | P0 | **Stateful modules execute in single-writer memory.** Running >1 active writer for the same module/shard is split-brain; durable failover also requires an authoritative mirror. | **RESOLVED (code/config)** — standalone roles retain the default-off ownership gate (§13.7), while `config.rasn.runtime.replicated.ini` deploys all eleven modules as direct rDSN type-1 tables with quorum mutation commit, resolver-backed partitions, checkpoint learning, and primary failover (§13.15). `rasn.state.replicated` remains available for standalone mirror durability (§13.13) |
| 1.2 | P0 | **Core services had no RPC resilience.** `rasn.state` / `rasn.workflow` / `rasn.observability` clients made one-shot RPCs — no breaker, no retry — while the module path was fully hardened. A single transient blip failed the call. | **RESOLVED (code)** — §7.1, `rpc_resilience.h`, wrapped in `agent_services.cpp` |
| 1.3 | P0 | **Registry discovery was an in-memory SPOF on the request path.** Routing resolved live endpoints through one process-local `rasn.registry`; restart lost dynamic membership and one frontend failure interrupted discovery. | **RESOLVED (code/config)** — §9/§13.14: opt-in ZooKeeper-backed `meta_state_service` records, fenced active-writer election over `distributed_lock_service`, shared reads on every frontend, strict backend errors, and rDSN group-address failover/retry across `registry_addresses`. Local map + legacy one-address behavior remain the default |
| 1.4 | P1 | **RPC envelopes carry no end-to-end trace id.** `agent_request`/`response` carry `trace_id`, but the runtime-module envelope (`make_module_request`) didn't propagate it, so a call couldn't be followed across nodes in logs. | **RESOLVED (code)** — §13.4; `trace_id` added to the runtime-module envelope (EOF-safe), stamped from an ambient scope on egress, restored/echoed on ingress |
| 1.5 | P1 | **Resilience/quota/dedup state is per serving process.** Breaker, dedup, admission, and rate state live on whichever node serves the RPC; there is no shared view, so protection is per-replica, not cluster-global. | **PARTIALLY RESOLVED (code)** — all four circuit-breaker families can use the fenced coordination adapter (§13.7). Native module tables now replicate deterministic quota mutations and a bounded FIFO dedup window with each partition, including checkpoints (§13.15). Admission, rate/cost, and process-wide overload authorities remain per process because they need leased capacity or transactional allocation. |
| 1.6 | P2 | **Core service endpoints are bound at construction.** Some core clients resolve their peer once, limiting failover for non-registry paths. | **PARTIALLY RESOLVED** — registry clients now bind one durable rDSN group whose leader changes in place; other core-service addresses still need resolver/group-based rebinding |

### 13.2 Lens 2 — Reinvention vs. reuse of rDSN

rASN correctly reuses `serverlet`/`clientlet` RPC, `perf_counter` metrics,
`command_manager`, `zlock`, `exp_delay` backpressure, NFS
(`dsn::file::copy_remote_files`) in the state service, and — as of the coordination
module (§13.7) — `dist::distributed_lock_service` and `dist::meta_state_service` for
ownership election and cluster-shared state. The audit found four places
where rASN grew a parallel mechanism that an existing rDSN facility should own:

| Concern | rASN today | rDSN facility to reuse | Status |
| --- | --- | --- | --- |
| State replication / HA | standalone journaled store remains available; optional `rasn.state.replicated` plus eleven direct runtime-module tables | `replicated_service_app_type_1` (layer-2 replication SM: `checkpoint`/`learn`/`apply`) | **DELIVERED (§13.13/§13.15)** |
| Partition routing | stable application key hash with static/registry shard fallbacks; native module tables consume the same hash | `dist::partition_resolver` (partition→endpoint resolution with config/meta integration) | **DELIVERED (§6/§13.15)** |
| Discovery + failure detection | local map by default; opt-in shared descriptor/lease records with fenced primary and frontend group | `meta_state_service` + `distributed_lock_service` on `ext/zookeeper`; rDSN group address for client failover | **DELIVERED (§13.14)** |
| Wire schema / IDL | generic envelope with a field-map payload + `schema_manifest` codegen | Thrift IDL + `dsn.tools` codegen (typed, versioned RPC structs) | DOCUMENTED |

The coordination module (§13.7), replicated state backend (§13.13), and direct
runtime tables (§13.15) prove this
reuse pattern is viable end-to-end: the
`rDSN.dist.service` ext plugin builds and links into rASN under a full
`--build_plugins` checkout, and its `distributed_lock_service`/`meta_state_service`
providers and type-1 replication applications are consumed directly. The primary
remaining data-plane migration is the generic field-map wire schema, which stays
explicitly sequenced rather than being approximated with another rASN-local RPC
framework.

### 13.3 Lens 3 — Missing critical modules

Toward a production agent nucleus, the audit flags these gaps (all DOCUMENTED
roadmap items, ordered by importance):

- **P0 durable/vector agent memory** — a first-class long-term memory module
  (semantic + episodic) with a durable, queryable backend, distinct from the
  short-lived blackboard/session stores.
- **P0 distributed coordination** — **DELIVERED (code, §13.7).** Leader election and
  distributed locks now reuse rDSN `distributed_lock_service`, and a cluster-shared
  state store reuses `meta_state_service`, so single-writer modules (§8) can elect
  exactly one owner cluster-wide via real rDSN primitives (meta-server/ZooKeeper
  under the ZooKeeper backend), not a rASN-local lock. Making a **global** quota/rate
  authority consume this shared store is the remaining per-module wiring.
- **P0 secrets vault** — a real secret provider behind the existing `env:`/`file:`/
  `cmd:` credential handles (see DESIGN "Credential storage").
- **P0 multi-tenancy / isolation** — per-tenant identity, quota, and data
  isolation across all modules.
- **P0 distributed scheduler / placement** — placement of module shards and agent
  work across nodes (today placement is static config + deterministic hashing).
- **P1** — centralized durable **audit log**, automatic cross-node **recovery**
  (supervisor promotes a new owner on node loss), cross-node **exactly-once/saga**
  (current dedup is per-process), and cross-node **backpressure / distributed
  cache**.

### 13.4 Trace propagation (finding 1.4) — RESOLVED

One trace id now threads through every runtime-module hop so a request can be
followed across nodes. As built:

1. **Ambient context.** `runtime_provider.cpp` holds a `thread_local` ambient
   trace id exposed via `current_rasn_runtime_trace_id()` and the RAII
   `rasn_runtime_trace_scope` (declared in `runtime_provider.h`). An operation
   origin installs a scope for the duration of the call; nested module requests
   inherit it. An empty id is a no-op, so callers install unconditionally.
2. **Stamp on egress.** `make_module_request(...)` reads the ambient trace id and
   stamps it onto the envelope. A `trace_id` field was appended to both
   `rasn_runtime_request` and `rasn_runtime_response`, unmarshalled EOF-safe
   exactly like `route_partition`/`auth_token`, so old and new peers interoperate.
3. **Restore + echo on ingress.** `rasn_runtime_rpc_service::reply_module_request`
   installs a `rasn_runtime_trace_scope` from the incoming envelope before
   dispatch, so server-side logs and any nested module calls share the id. The
   central response builder `make_rasn_runtime_response` echoes `request.trace_id`
   onto every response (success and error), so a caller can correlate the reply.
4. **Origins.** `rasn_coordinator_service::invoke` and `rasn_service_graph::invoke`
   seed the scope from the operation's `agent_request.trace_id`, falling back to
   `nucleus_runtime::trace_id()`, so module RPCs issued while coordinating share
   one end-to-end trace.

Coverage: `request_marshalling_round_trips_request_metadata` (extended),
`response_marshalling_round_trips_trace_id`,
`trace_scope_sets_and_restores_ambient_trace_id`, and
`dispatch_echoes_request_trace_id_onto_response` in `tests/rasn_unit_tests.cpp`
assert wire round-trip, legacy back-compat (empty on decode), scope
nesting/restore, and end-to-end echo. The change is purely additive to the wire
and logs — no behavior change.

The id now flows and is echoed end-to-end; surfacing it as an explicit label on
individual resilience/dedup/auth metrics and log lines is a small additive
follow-up (the ambient id is already readable via `current_rasn_runtime_trace_id()`
inside the server dispatch scope) and does not change the propagation contract.

### 13.5 rDSN-native target architecture (findings 1.1, 1.3, 1.5) — RESOLVED for module state

The end state keeps the app-facing `rasn_runtime` facade and the module API
contract (§4) exactly as they are, and swaps the *backing* of stateful modules:

- The state-service half is now delivered: `rasn.state.replicated` uses
  `replicated_service_app_type_1`, so quorum-committed mutations plus
  checkpoint/learn/apply provide an authoritative mirror (§13.13).
- The direct module half is now delivered (§13.15): every runtime module has a
  dedicated type-1 app/table. `describe_topology()` reports
  `actual=rdsn_type1_replica_group` when the endpoint is declared native replicated;
  the standalone path remains explicit as `single_writer_in_memory`.
- URI-backed sharded clients now pass the stable application key hash to rDSN RPC,
  so `dist::partition_resolver` maps partitions to replica groups and refreshes
  failed access (§6). The static/registry fallback remains for standalone
  single-writer shards; `config.rasn.runtime.replicated.ini` supplies the
  meta-managed module tables.
- Membership/discovery HA is now delivered (§13.14) using the generic rDSN
  facilities that actually match descriptor storage: ZooKeeper-backed
  `meta_state_service` for records, `distributed_lock_service` for a fenced writer,
  and an rDSN group address for frontend failover. The replication meta-server and
  `failure_detector` remain table/replica and node-liveness facilities; forcing
  arbitrary capability descriptors into their partition protocol would create a
  second, mismatched registry rather than reuse an API designed for this data.

The operational choice is now explicit. Lightweight standalone module roles use
the §8 one-writer contract and may point at `rasn.state.replicated` for durable
mirror/standby hydration. Native module tables use rDSN's primary/secondary
protocol directly and must disable the legacy state mirror.

### 13.6 What changed — core-service resilience + trace round

Resolved/mitigated **in code and validated** (committed as
`rasn: core-service RPC resilience + end-to-end trace propagation`):

- Core-service client RPC resilience (§7.1) — new `rpc_resilience.{h,cpp}` reusing
  the `circuit_breaker` engine; all `rasn_service_graph` state/workflow/
  observability call sites in `agent_services.cpp` and the routing-critical
  registry discovery query in `coordinator_service.cpp` now fail fast on unhealthy
  endpoints and retry transient transport errors with idempotency-aware policy
  (findings 1.2 fully, 1.3/1.6 client-side).
- Config knobs `rasn_core_rpc_*` (`[rasn.service]`) in the shared `config.rasn.ini`.
- Focused unit coverage: `rasn_rpc_resilience` in `tests/rasn_unit_tests.cpp`
  (pre-apply retry, idempotency-aware timeout policy, breaker short-circuit).
- End-to-end trace propagation across the runtime-module envelope (§13.4, finding
  1.4) — `trace_id` added to the request/response envelope (EOF-safe), an ambient
  `rasn_runtime_trace_scope` stamped on egress and restored/echoed on ingress, with
  four focused tests for wire round-trip, legacy back-compat, scope nesting, and
  dispatch echo.

Everything else above is either landed in the coordination (§13.7), replicated
state (§13.13), HA discovery (§13.14), resolver-aware routing (§6), and direct
module replication (§13.15) rounds or DOCUMENTED with its rDSN-native target
because it depends on a larger migration such as IDL codegen. These sections are
the source of truth for §11.

### 13.7 Distributed coordination module (findings 1.1 ownership, 1.5 shared state) — RESOLVED (code)

The latest refinement round lands a first-class **coordination module** that reuses
rDSN's own distributed facilities instead of reinventing them, directly closing the
ownership half of finding 1.1 and the shared-state half of finding 1.5. Validated on
real hardware (Ubuntu, `--build_plugins`): all four coordination unit tests pass,
including the two that drive the real rDSN provider, and `codepilot`/`srepilot` link
the module cleanly.

**Facade.** `coordination_service.h` defines `rasn_coordination_service`, a small
provider-agnostic interface with two concerns:

- *Ownership / leader election* — `acquire_ownership(resource, owner_id[, timeout])`,
  `release_ownership`, `query_owner`. Exactly one caller holds a resource at a time;
  re-acquire by the same owner is idempotent; every grant exposes a fencing version,
  and callers that need monotonic fencing can release without destroying the lock
  object.
- *Cluster-shared state* — `put_state` / `get_state` / `delete_state` / `list_state`
  over a hierarchical, znode-style key space rooted at a configured namespace.

**Three backends, selected by `[rasn.coordination] provider`:**

| Backend | Reuses | Use |
| --- | --- | --- |
| `inproc` | in-process maps (no external dep) | default single-node fallback; always compiled |
| `simple` | rDSN `distributed_lock_service_simple` + `meta_state_service_simple` | unit tests / single-writer dev; **coordinates only within one in-process facade instance** (see note); durable simple meta-state log |
| `zookeeper` | rDSN `distributed_lock_service_zookeeper` + `meta_state_service_zookeeper` | production HA over a ZooKeeper ensemble (`[zookeeper] hosts_list`); the only backend that coordinates across independent processes/apps |

> **`simple` is single-instance.** The rDSN simple providers keep their lock table
> and state tree in per-object members, so each `dist_coordination_service` that
> constructs them coordinates only with itself. Two facade instances in one process
> — or two processes — share nothing. Use `simple` where exactly one facade instance
> is the coordinator (unit tests, single-writer dev); use `zookeeper` for real
> cross-process / multi-app coordination, whether the participants are on one box or
> many.

The `simple`/`zookeeper` backends come from the `rDSN.dist.service` ext plugin
(`src/plugins_ext/`). The facade wraps their async, `error_code`-returning task API
behind blocking helpers. Those helpers wait no longer than the configured
`operation_timeout_ms` for completion callbacks delivered on
**`THREAD_POOL_META_SERVER`** — the pool the reused rDSN dist providers themselves
depend on: `distributed_lock_service_simple`/`_zookeeper` enqueue their own internal
work there (notably the `LPC_DIST_LOCK_SVC_RANDOM_EXPIRE` lease timer), so any app
running the `simple`/`zookeeper` backend must declare that pool regardless of where
callbacks land. It is distinct from `THREAD_POOL_DEFAULT` / `THREAD_POOL_RASN_WORKFLOW`
(which run rASN request handlers), so a blocked caller cannot starve
the worker that runs its own callback.

Because the shipped configs default to `provider = inproc`, they deliberately do
**not** declare `THREAD_POOL_META_SERVER`. That pool is registered by the
`rDSN.dist.service` providers, which the default `[modules]` list does not load, so
declaring it (in a `pools` list or a `[threadpool.*]` section) under the default
config fails config parsing with `invalid enum configuration ... THREAD_POOL_META_SERVER`
at startup. To run a distributed backend: (1) build with `--build_plugins` so the
providers register the pool, (2) set `provider = simple|zookeeper`, (3) add
`THREAD_POOL_META_SERVER` to each rASN app's `pools` list, and (4) uncomment the
`[threadpool.THREAD_POOL_META_SERVER]` section that both configs ship (commented) for
exactly this purpose. The helpers create parent znodes on demand and track held lease
tasks.

**Correctness hardening (as-built).** Beyond the happy path, the facade fails closed
and stays retry-safe under races: (a) a lock acquire that *times out* re-checks the
grant/cancel outcome and, if it actually won the race (`cancel_pending_lock` reports
`ERR_OBJECT_NOT_FOUND` with our own `owner_id`, or the grant callback already stored
`ERR_OK`), records the hold and returns `ERR_OK` instead of leaking a granted-but-
abandoned lock; (b) `put_state` treats a lost `node_exist`→`create_node` race
(`ERR_NODE_ALREADY_EXIST`) as last-writer-wins by falling back to `set_data`, so
concurrent first-writers all succeed; (c) namespace/parent-znode creation tolerates
only `ERR_NODE_ALREADY_EXIST` and surfaces every other error — `start()` aborts if the
state namespace cannot be materialized rather than limping on against a missing root;
(d) cancellation, unlock, state I/O, and cleanup are all bounded, callbacks retain
heap-owned result state, and a provider with an unresolved operation is quarantined
rather than finalized under a late callback; (e) once unlock begins, the cached hold
is unusable, so an ambiguous unlock can never be mistaken for an idempotent re-acquire.
If a timed-out cancel later grants the lock, a completion-side cleanup releases it.

**Build wiring (reuse, not reinvent).** `src/CMakeLists.txt` now configures
`plugins_ext` before `plugins` so rASN can see the dist targets. The rASN library,
`codepilot`, `srepilot`, and `rasn.unit_tests` gate the dist include paths, link the
`dsn.dist.service.*` closure, and define `RASN_HAS_DIST_COORDINATION=1` **only when**
`TARGET dsn.dist.service.meta_server_lib` exists (i.e. a `--build_plugins` build).
A plain build without the ext plugin still compiles rASN with just the `inproc`
fallback, so nothing regresses for lightweight checkouts.

**Config.** `[rasn.coordination]` in the shared defaults:
`provider` (`inproc`|`simple`|`zookeeper`), `lock_namespace`, `state_namespace`,
`acquire_timeout_ms`, `operation_timeout_ms`, and `state_work_dir` (durable-log
directory for the `simple` backend; ignored by the others), plus the shared-breaker
controls below.

**Tests.** `tests/rasn_coordination_test.cpp` asserts the ownership contract
(acquire / idempotent re-acquire / mutual exclusion / hand-off / query) and the
shared-state contract (absent-get / put / overwrite / list / delete / idempotent
delete) against the facade. The `inproc` path and config defaults always run; the
`simple`-provider cases run under `RASN_HAS_DIST_COORDINATION` and exercise the real
rDSN lock + meta-state providers, including a concurrent-`put_state` case that races
eight writers on one fresh key and asserts they all succeed (last-writer-wins).
Focused cases also cover preserved monotonic grant versions, post-release fence
barriers, shared-breaker propagation, exclusive/recoverable probes, stale reports,
lease caps, snapshots, and configuration mismatch.

**Wired into the runtime (as-built).** Consuming the facility inside the module
services is now landed for the ownership half: `rasn_runtime_app::start()` calls
`acquire_module_ownership()` **before** state hydration and `open_service()`. When
`rasn_runtime_ownership_gate_enabled = true`, it builds a coordination service from
`[rasn.coordination]`, then acquires ownership of each module (or hosted shard) the
service serves — resources named `rasn.runtime.<module>` for an unsharded module or
`rasn.runtime.<module>.shard.<n>` per served shard (a host of the whole sharded module
locks *every* shard resource, so it still contends with any shard-specific peer), owner
id = the node's primary address plus a per-runtime session nonce — and **fails
closed** (refuses to open the RPC API, releasing anything it took) if a resource is
already owned. Ownership is acquired before
hydration so a standby that waits for the active owner to release then hydrates the
*latest* committed snapshot rather than a stale one (review finding 1); a contended
acquire is retried up to `rasn_runtime_ownership_acquire_max_attempts` (default 1)
before failing closed, and an always-on standby relies on a supervisor restart to
retry past that (§10). Lease-loss callbacks use lifetime-safe shared state and
fail-stop if ownership disappears while the service can accept requests. On normal
stop, the server first rejects new RPCs and drains admitted handlers for at most
`rasn_runtime_request_drain_timeout_ms`; it then releases ownership. A drain timeout
or ambiguous unlock fail-stops the process, because rDSN shares the underlying
ZooKeeper session by app name and finalizing this facade alone cannot prove that an
ephemeral ghost owner disappeared. Independently of the
gate, the RPC ingress path (`reply_module_request`) rejects any request routed to a
shard the service does not host, so a misrouted request cannot mutate unowned state
(review finding 2). The resource derivation and the ingress guard are unit-tested
(`rasn_runtime_module_ownership_resources`, `rasn_runtime_service_hosts_request`).
Default off, so single-node/local runs and the `inproc` backend are unaffected;
turning §8's operator-discipline single-writer into an *enforced* one requires
`provider = zookeeper` across independent processes (`simple` is only
single-facade).

**Cluster-global circuit breakers (finding 1.5, as-built).**
`coordination_breaker.{h,cpp}` adapts the existing `circuit_breaker_registry`
without creating another breaker state machine. It is wired into every outbound
dependency family:

- core-service clients (`core_rpc`, keyed by service + endpoint);
- runtime-module clients (`runtime_module`, keyed by module + shard + endpoint);
- model providers (`model_provider`);
- remote-agent dispatch (`remote_agent`).

The adapter stores `rasn-breaker-v1` records below
`shared_breaker_state_prefix/v1/<hex-scope>/<hex-key>/<fencing-version>`. Its
distributed lock ID is a bounded, slash-free hash rather than this hierarchical
state path (ZooKeeper lock IDs reject `/`). Every mutation takes a process-local
per-key mutex and then acquires the rDSN lock with its monotonic grant version and a
per-context, per-transition owner nonce (equal owner values are unsafe across
independent ZooKeeper lock nodes).
The version becomes a fenced state child; readers always select the greatest
version, while obsolete children beyond a short reader-safety tail are pruned
best-effort. Therefore an old holder whose
lease expires may write only a lower fenced child — it cannot overwrite or become
newer than the successor's record even though `meta_state_service::put_state` has
no CAS and is last-writer-wins. Lock release preserves the lock directory so grant
versions remain monotonic.

Fenced final-state ordering alone is not enough for half-open admission: a paused
old holder could write and verify its lower child before the newer holder publishes
its child. Probe claims therefore use a completion barrier. After persisting, the
claim releases its uniquely-owned lock, checks the ZooKeeper lock directory for an
already-active newer grant, and rereads the greatest fenced record. It returns an
admission only when both checks still identify its exact fence/revision/probe token.
An ambiguous release, queued/newer owner, or superseding record fails closed. This
may conservatively delay recovery for one probe lease during a handoff, but it never
turns an uncertain stale claim into an executable probe.

Records carry that fence, a monotonic diagnostic revision, a closed-state
generation, state/failure/open timestamps, active probe token/deadline, and the
breaker/shared timing tunables. Coordination contexts are retained per current
rDSN app identity because ZooKeeper callbacks are bound to the initializing app's
thread pools; no provider instance is shared across app roles in one process.
Unknown/malformed records, coordination errors, lock timeouts, stale fences, and
config mismatches fail closed and surface in logs/snapshots rather than silently
falling back to a process-local breaker.

The admission returned by `allow()` carries both a probe token and the current
closed-state generation through `report()`. Only the current token may resolve
`half_open`; after recovery, an ordinary request admitted before the prior open
generation is also ignored, so it cannot reopen a healthy breaker. The probe
deadline starts with the larger of `shared_breaker_probe_lease_ms` and the call's
timeout/retry budget, clamps it to `shared_breaker_max_probe_lease_ms`, then adds
`shared_breaker_clock_skew_ms`. This preserves one live cluster-wide probe,
recovers abandoned probes, and prevents an unbounded request timeout from creating
an effectively permanent lease. Model-provider hints use the provider's actual
effective `[rasn.model] request_timeout_sec` when a request leaves `timeout_ms = 0`,
rather than the unrelated 20-second agent fallback. Runtime ping prepares and
authenticates its request before claiming admission, so local auth failure cannot
abandon a probe.

Enable this only on participants configured with the same ZooKeeper ensemble,
namespaces, and breaker tunables:

```ini
[rasn.coordination]
provider = zookeeper
operation_timeout_ms = 5000
shared_breaker_enabled = true
shared_breaker_state_prefix = resilience/circuit_breakers
shared_breaker_lock_timeout_ms = 1000
shared_breaker_probe_lease_ms = 120000
shared_breaker_max_probe_lease_ms = 600000
shared_breaker_clock_skew_ms = 5000
```

The hosting app must also declare `THREAD_POOL_META_SERVER` and
`THREAD_POOL_DLOCK` as described above. `inproc` and `simple` exercise the same
adapter for development but do not make state authoritative across processes.
All ZooKeeper participants must synchronize physical clocks (for example with
NTP) and set `shared_breaker_clock_skew_ms` at least as large as the operational
skew bound; cooldown and probe deadlines are persisted physical timestamps.
Because the persisted record verifies its tunables, a rolling deployment with
different breaker thresholds/timers deliberately fails closed. Drain the old
participants and either prune the old breaker subtree or select a new
`shared_breaker_state_prefix` when intentionally changing those values.

**What remains.** (a) Design leased/transactional global admission, rate/cost,
overload, and dedup authorities; they cannot safely use a naive
`get`/modify/last-writer-wins `put`. (b) Extend the multi-process harness to assert
shared breaker propagation and leased probe takeover on ZooKeeper. Direct quorum **replication** of runtime module state remains the §13.5 item; the
shared `rasn.state` authority is now replicated as described in §13.13.

### 13.8 Robustness hardening — cold-start readiness, diagnostic-leak cleanup, LLM parsing — RESOLVED (code)

A robustness pass validated on real hardware (Ubuntu, `--build_plugins`: 151/151
unit tests, a single-box multi-process `--dsn` deployment, and a libfiu
fault-injection campaign) lands three rASN-confined fixes:

- **Cold-start state-hydration readiness retry** (`runtime_provider.cpp`,
  `hydrate_modules_from_state()`). The startup hydration query previously made a
  single `rasn.state` attempt; on a cold multi-process start the co-located state
  service may still be registering its RPC handlers, so that attempt could
  `ERR_TIMEOUT`, `start()` returned the error, and rDSN escalated it to a fatal
  `dassert(err == ERR_OK)` / "start app failed" abort. The query now retries on
  transient errors (`is_retryable_rasn_runtime_error`) up to
  `rasn_runtime_state_hydration_max_attempts` (default 20) with
  `rasn_runtime_state_hydration_retry_backoff_ms` (default 250 ms) backoff, mirroring
  the workflow app's `recover_workflow_state_after_start()`. It still fails closed
  after the budget or on a non-transient error, so the §5 durability contract is
  unchanged — only the cold-start race is closed. (The hard-abort-on-`start()`-error
  policy itself lives in rDSN core, outside rASN.)

- **Diagnostic error-code leak cleanup** (`rpc_resilience.h` `resilient_rpc_call`,
  `runtime_provider.cpp` `invoke_remote_module`). Both parked an unread `ERR_UNKNOWN`
  sentinel across their success return; in a `TRACK_ERROR_CODE` build that tripped
  rDSN's "error code is not handled" destructor diagnostic on every successful call —
  one line per startup and ~29 lines per distributed ask — burying real warnings. The
  sentinels are removed (failure paths already return the true error), so success and
  failure behaviour is otherwise identical. This is log hygiene, not a semantics
  change, and clears the storm in both local and distributed modes.

- **LLM response parsing** (`llm_provider.{h,cpp}`, `parse_chat_completion()`).
  Chat-completion decoding is centralised so a reply whose text arrives in
  `reasoning_content` (with empty `content`) is no longer surfaced as raw JSON, and a
  provider error envelope is reported as an error instead of being mistaken for a
  successful answer. Covered by six new `rasn_llm_provider` unit tests.

**Fault-injection outcome.** Under injected `malloc`/`strdup`/POSIX-I/O/network
failures (libfiu `fiu-run`, 8 targets × 8 fault profiles), the binaries either
fail-stop cleanly (allocation faults → `std::bad_alloc` → `SIGABRT`) or propagate
graceful non-zero-exit errors — no `SIGSEGV`/`SIGBUS`/`SIGFPE`/`SIGILL` and no
reproducible hang. The harness is `tests/fault_injection/run_fault_injection.sh`
(opt-in, POSIX-only, not wired into CMake/CI).

### 13.9 Ownership-gate review follow-ups — stale-read, shard ingress, failover honesty, config include — RESOLVED (code)

Review of the single-writer ownership gate (§13.7) surfaced four issues; all are now
fixed within `src/plugins/rasn`:

- **Acquire ownership before hydration (finding 1).** `rasn_runtime_app::start()`
  previously hydrated in-memory state and *then* waited for ownership, so a standby
  could hydrate snapshot N, block while the active owner committed N+1, win the lock,
  and open handlers on stale state. `start()` now runs `acquire_module_ownership()`
  **before** `hydrate_modules_from_state()` (and releases ownership if hydration then
  fails). Once a node owns the resource no other node writes, so the subsequent
  hydration always observes the latest committed snapshot. Gate-off deployments are
  unaffected (acquire/release are no-ops).

- **Shard-ownership enforced on RPC ingress (finding 2).** `reply_module_request()`
  dispatched any request for a hosted module regardless of the shard it routed to, so
  a stale registry entry or a direct client could drive a shard-1 request into the
  shard-0 owner and mutate unowned state. The ingress path now rejects a request whose
  resolved partition is not in this service's hosted-shard set
  (`rasn_runtime_service_hosts_request`, cached per module), returning a clear error
  and a `runtime.shard.misrouted` metric. It is a no-op for unsharded modules and for
  services that host the whole module, and applies whether or not the ownership gate
  is enabled.

- **Honest active/standby failover (finding 3).** A losing node fails closed, which
  aborts the process (rDSN asserts on a non-`ERR_OK` app start); it does not stay alive
  as a self-healing warm standby. §10 now states that an external supervisor must
  restart the loser to retry ownership, and a bounded in-process retry knob
  (`rasn_runtime_ownership_acquire_max_attempts`, default 1;
  `rasn_runtime_ownership_acquire_retry_backoff_ms`, default 1000 ms) lets a standby
  ride out a brief handover before failing closed.

- **Runtime-config `@include` resolves beside the config (finding 4).**
  Before the §13.12 redesign, `config.rasn.ini` ended with
  `@include config.ini`, while rDSN resolves includes relative to the process
  working directory. Auto-detecting the runtime config next to the binary while
  launching elsewhere could therefore miss the sibling app config (or pull in an
  unrelated one). `align_working_directory_to_runtime_config` fixed that path
  and remains in the `serve` entry point because the self-contained runtime config
  now includes sibling `config.rasn.defaults.ini`. The runtime no longer includes
  an app config. No rDSN core change.

### 13.10 Ownership-gate review follow-ups (round 2) — whole-module split brain, Windows `--dsn` chdir — RESOLVED (code)

A second review pass on the ownership gate (§13.7, §13.9) surfaced two more issues; both
are fixed within `src/plugins/rasn`:

- **A whole-module host of a sharded module now locks every shard (split brain).**
  `rasn_runtime_module_ownership_resources()` mapped an unqualified sharded module to the
  bare `rasn.runtime.<module>` resource, while shard-specific services lock
  `rasn.runtime.<module>.shard.<n>`. Those are *distinct* locks, so a node hosting the
  whole module (`blackboard_shard_count = 2`, no `blackboard_hosted_shards`) and a node
  hosting shard 0 (`blackboard_hosted_shards = 0`) never contended — both could serve
  shard 0. The resource derivation now expands a whole-module host of a sharded module to
  the full `rasn.runtime.<module>.shard.0 … shard.N-1` lock set, so it contends with every
  shard-specific peer; an unsharded module still takes a single module-level lock. The
  branch logic is factored into a pure, config-free
  `rasn_runtime_module_ownership_resources_for()` and unit-tested across the explicit
  shard-subset, whole-sharded-module, and unsharded cases.

- **Windows `--dsn` chdir is no longer a no-op.**
  `align_working_directory_to_runtime_config()` guarded its `chdir` under
  `#if !defined(_WIN32)`, so on Windows `codepilot.exe --dsn` / `srepilot.exe --dsn` only
  passed an absolute config path while rDSN still resolved `@include config.ini` against
  the process working directory — reintroducing finding 4 on Windows. The chdir is now
  cross-platform (`::_chdir` on Windows, `::chdir` elsewhere), so the sibling include
  resolves beside the runtime config regardless of launch directory or OS. No rDSN core
  change.

### 13.11 Distributed robustness testing — cross-process single-writer + coordination pool wiring — RESOLVED (docs/config) + REPORTED

A dedicated robustness round exercised the ownership gate **cross-process** on real
hardware (Ubuntu, `--build_plugins`) against a live single-node ZooKeeper ensemble
(`provider = zookeeper`), plus a re-run of the libfiu fault-injection campaign on the
current build. Two standalone `rasn.runtime.blackboard` processes were launched on
different ports, both hosting `blackboard` shard 0 with the gate enabled.

- **Cross-process single-writer confirmed.** Exactly one process acquired the
  `rasn.runtime.blackboard.shard.0` ZooKeeper lock and opened its module handlers; the
  other blocked for `acquire_timeout_ms`, got `ERR_TIMEOUT`, logged `failed to acquire
  ownership of rasn.runtime.blackboard.shard.0: ERR_TIMEOUT; refusing to open module
  APIs`, and registered **zero** module handlers. This is the first end-to-end,
  cross-*process* validation of the gate (previously only `inproc`/`simple` single-process
  and unit coverage); it confirms the split-brain-fix resource derivation (§13.10) denies a
  second writer through a real distributed lock backend.

- **Coordination thread-pool wiring corrected (docs/config bug — FIXED).** The zookeeper
  backend does **not** run its lock work on `THREAD_POOL_META_SERVER` (only the `simple`
  backend and the rASN facade's own callbacks do). The zookeeper lock provider
  (`distributed_lock_service_zookeeper`) runs `TASK_CODE_DLOCK` on **`THREAD_POOL_DLOCK`**
  and asserts single-thread access per lock, so a zookeeper-backed app must declare BOTH
  `THREAD_POOL_META_SERVER` and `THREAD_POOL_DLOCK` (the latter `partitioned = true`).
  Following the previous guidance (only `THREAD_POOL_META_SERVER`) core-dumped at startup
  with `pool THREAD_POOL_DLOCK not ready`. `config.rasn.ini`, this document's §10 example,
  and the `coordination_service` comments now document the correct per-backend pool set.
  (`THREAD_POOL_META_STATE` from the canonical `config-zk.ini` is the replication
  meta-server's pool and is **not** required by the coordination facade — verified by a
  minimal-pool start.)

- **Loser lingers instead of aborting (REPORTED, launch-path dependent).** §10 states the
  fail-closed loser aborts (`rDSN asserts on a non-ERR_OK app start`) and relies on a
  supervisor restart. In the observed `codepilot --dsn` path (with `enable_default_app_mimic
  = true`) the loser process instead **stayed alive** with its node port bound but no module
  handlers — correctness is preserved (it serves nothing), but a process-liveness supervisor
  would not restart it and it does not re-attempt ownership on its own. The hard-abort policy
  lives in rDSN core (`tool_api.cpp` `dassert(err == ERR_OK)`), whose reachability depends on
  the launch/mimic path; the reliable active/standby failover story therefore needs either an
  rASN-side retry/exit loop or a readiness/health probe that checks *handlers open*, not just
  *process alive*. Deferred pending the planned distributed-mode redesign (standalone runtime
  service).

- **libfiu campaign PASS on the current build.** Re-running the fault-injection harness
  (8 targets × 8 fault classes × 10 runs, now covering the ownership/coordination/split-brain
  code via the `rasn_*` gtests) produced no `SIGSEGV/SIGBUS/SIGFPE/SIGILL` and no reproducible
  hang: `malloc` faults fail-stop via `std::bad_alloc` (`SIGABRT`); I/O and network faults
  propagate as graceful errors. Matches the reference result.

### 13.12 Local/distributed mode & config-include redesign — RESOLVED (code)

A design review of the app entry model and the then-two-file config layout (§6.1)
validated a cleaner target, which has now been **implemented**. §6.1 has been
rewritten to the as-built model; this section keeps the rationale and records the
outcome. The redesign was sequenced deliberately because the config-include
direction had already been flipped and reversed once (see §13.9 finding 4 and the
`align_working_directory_to_runtime_config` shim).

**Root observation — `--dsn` conflated two orthogonal axes.** Before this round,
the flag selected *both* the process role *and* which config loaded, while runtime
placement was separately config-driven. The redesign separates them cleanly:

1. **Process role** — one-shot CLI (`app <cmd>`, runs and exits) vs. long-running
   service host (boots an rDSN node, launches the `[apps.rasn.*]` fleet). This is an
   argv concern, not a placement concern, and is now spelled `serve` (`--dsn`
   remains only as a deprecated compatibility alias).
2. **Runtime placement** — `local` / `distributed` / `hybrid`, already driven by
   `[rasn.runtime] rasn_runtime_provider`. No flag should be required to pick it.

**Decisions (validated).**

1. **Placement is config-only.** An app does not pass a flag to "be distributed";
   `rasn_runtime_provider` (+ the `[rasn.service]` endpoint map) fully determines
   whether each module is invoked in-process or over RPC. `--dsn` is retired as the
   mode selector.
2. **The runtime is deployed independently of any app.** A standalone runtime node
   is an ordinary rDSN service process whose config **is** `config.rasn.ini` (service
   apps + tuning; **no** app-gateway section, **no** app include). An app in
   `distributed` mode only holds client stubs and the runtime's `[rasn.service]`
   address; it never sees how or where the runtime runs. All module internals stay
   hidden behind the `rasn_runtime` facade.
3. **An app may also be its own multi-node rDSN app** (its own `config.ini`,
   `[apps.*]`, `[meta_server]`/`[replication]`, deployed per the app.kv / meta_server
   pattern). That is orthogonal to the rASN runtime — so app-config and runtime-config
   stay **separate deployment units** and are never force-merged.
4. **Invert the include.** `config.rasn.ini` is now self-contained: the trailing
   `@include config.ini` was replaced by
   `@include config.rasn.defaults.ini`. The working-directory alignment helper is
   retained so this sibling defaults include resolves correctly. For a **local**
   in-process runtime, the app's own `config.ini` may include the same shared
   defaults near the top and override specific keys below.
5. **rDSN include semantics make the inversion clean — verified.** `@include` is
   processed **inline at its textual position** and assignments are **last-write-wins**
   (`src/dev/utility/configuration.cpp:216-263`; the `WARNING: overwrite option … (line
   X => Y)` startup lines are this mechanism). So an included file overrides same-named
   keys defined *before* it, and keys defined *after* the include override the included
   file. Placing `@include config.rasn.defaults.ini` near the top, then app
   overrides below, yields exactly the intended precedence with no parser change.

**Implementation outcome.**

- **A. Client-node bootstrap for CLI-side distributed mode.** A remote module call
  requires an rDSN *service-node context*. App entry points now pre-read
  `[rasn.runtime]`; `distributed`/`hybrid` commands start and attach to a
  lightweight `mimic` client node, while `local` commands keep the node-less fast
  path. No command-line placement flag is involved.
- **B. Split `config.rasn.ini`.** Host deployment (`[apps.rasn.*]`, `[core]`,
  `[modules]`, thread pools, `[rasn.service]`) remains in `config.rasn.ini`;
  placement-independent behavior tuning moved to
  `config.rasn.defaults.ini`. A local app can include only the latter without
  inheriting service deployment sections.

**Target topologies after the refactor.**

| Topology | App process | Runtime |
| --- | --- | --- |
| Local app (dev) | `app` loads its `config.ini`; `rasn_runtime_provider = local`; optionally `@include`s runtime **defaults** | modules linked in-process, no RPC |
| Thin client → remote runtime | `app` loads its `config.ini`; `rasn_runtime_provider = distributed`; `[rasn.service]` → remote; starts a lightweight client node | separate runtime node(s) started from `config.rasn.ini` |
| App as its own distributed system | `app` deployed multi-node from its own `config.ini` (`[apps.*]`/`[meta_server]`) | independent; reached as a client |

Build validation covered `codepilot`, `srepilot`, and `rasn.unit_tests`.
Two-process smoke validation covered both explicit-address routing and
registry-only discovery with `rasn_runtime_advertise_host`; a negative control
without the advertise override reproduced the original unreachable-NIC timeout.

### 13.13 Quorum-replicated `rasn.state` authority — RESOLVED (code/config)

`rasn.state.replicated` is an opt-in application built directly on rDSN
`replicated_service_app_type_1`. It preserves the existing state RPC schema and
`rasn_service_graph`/`rasn_runtime` facades, so applications and runtime modules
switch durability backends only by setting:

```ini
[rasn.service]
state_uri = dsn://rasn-cluster/rasn-state

[uri-resolver.dsn://rasn-cluster]
factory = partition_resolver_simple
arguments = <meta-server-host>:27601
```

`config.rasn.state.ini` is the deployable single-machine profile: one meta server,
three replica servers, one `rasn-state` partition, and replication factor three.
It requires a `--build_plugins` build because it loads
`dsn.dist.service.meta_server`, `dsn.dist.service.stateful.type1`, and
`dsn.dist.uri.resolver`. Co-locating all replicas is useful for development and
quorum-path validation but does not survive host loss; production derives
per-node configs with one replica server and unique listen/data paths, plus an HA
meta-server deployment.

The application owns a journal-free `state_store`: rDSN's committed mutation log
is the durability authority, so a second per-replica append journal would introduce
filesystem-dependent divergence. `RPC_RASN_STATE_PUT` and
`RPC_RASN_STATE_PUT_CONDITIONAL`, `RPC_RASN_STATE_DELETE_PREFIX`, and
`RPC_RASN_STATE_DELETE_PREFIX_DETAILED`, and `RPC_RASN_STATE_ADVANCE_SEQUENCE` are marked
`rpc_request_is_write_operation`; the replication layer commits them before
dispatching the same deterministic mutation on every replica. Ordered replay and
checkpoint-restored `_last_sequence` make server-assigned sequences,
cutoff-guarded deletions, migration sequence barriers, and conditional-write
outcomes deterministic. Replicated startup validates every mutating task flag and
fails closed when an older/custom config omits one, preventing a newly registered
handler from bypassing quorum during a configuration upgrade.
`RPC_RASN_STATE_RECOVER` is also classified as a write but is rejected by replicated
instances: operators cannot replace one live replica from an arbitrary path.

Framework checkpoints are named `rasn-state-checkpoint.<decree>` in each partition
data directory. `sync_checkpoint()` snapshots the committed store,
`get_checkpoint()` exports the latest durable file for learning, and
`apply_checkpoint()` validates all input before acting. `DSN_CHKPT_COPY` persists
the learned checkpoint without replacing live memory (so copying a newer snapshot
cannot roll a serving primary backward); `DSN_CHKPT_LEARN` persists and atomically
replaces the in-memory store, after which rDSN replays later mutations.
The app explicitly returns `ERR_NOT_IMPLEMENTED` from `async_checkpoint()`:
`state_store::checkpoint()` releases its store lock during file I/O, and the
synchronous type-1 callback guarantees that decree `N+1` cannot apply before the
snapshot for decree `N` is captured. Any future asynchronous implementation first
needs an app-level decree/snapshot barrier rather than reusing this path directly.
Operator `RPC_RASN_STATE_CHECKPOINT` and `RPC_RASN_STATE_RECOVER` requests are
rejected in replicated mode so files outside the framework lifecycle cannot be
mistaken for durable replica checkpoints.

The standalone state journal records PUTs, cutoff-guarded prefix tombstones, and
sequence barriers in commit order. Recovery applies tombstones without deleting
records newer than their cutoff and restores barriers even when no live record
carries the checkpoint's final sequence. A checkpoint written to the configured
recovery directory entry (including an equivalent normalized/absolute spelling)
removes its covered journal only when no concurrent mutation landed. Leaf
symlinks/reparse points are rejected; distinct hard-link aliases and explicit
custom export paths never remove that recovery journal. Journal records are flushed before
acknowledgement; checkpoint/replica temporary files are flushed before atomic
replacement, with directory metadata flush, macOS `F_FULLFSYNC` for regular files,
or Windows write-through replacement. Linux regular files use `fdatasync` so
unrelated metadata is not forced. Standalone mode deliberately performs one
durable sync per mutation; write-heavy production placement should use
`rasn.state.replicated`, where rDSN owns mutation-log batching and quorum
durability. Windows state storage fails closed on FAT/exFAT and requires NTFS,
ReFS, or CSVFS rather than claiming equivalent directory durability. The volume
capability check happens during lifecycle-path validation; individual mutations
do not re-query volume metadata and rely on file flushes plus write-through moves.
Configured checkpoint/recovery calls still re-check links and path collisions on
every lifecycle operation while reusing only the filesystem-capability result.
The validator resolves and indexes each existing-file and canonical-entry
identity once, avoiding pairwise lifecycle-path comparisons.
Checkpoint/export targets and their `.tmp`/`.bak` staging names are rejected when
they alias either the primary or configured-replica journal lifecycle paths.
Validation covers effective replica basenames and `.nfs.tmp` copy staging, uses
native identity for existing files/hard links, resolves canonical parents for
future names, and honors Windows and case-insensitive macOS naming. Any legacy `.bak` is
first synchronized to the new flushed image; journal compaction proceeds only
after that alias is durably removed. Recovery files are opened no-follow, and a
live journal, trusted recovery input, or stale staging entry is accepted only
when it is a regular single-link file. A hard-link custom export remains safe
because atomic replacement detaches the export entry before writing.
Ancestor directories are an operator-owned trust boundary and must not be
writable by untrusted users; parent symlinks/mount indirections remain supported.
Recovery restores
independently missing checkpoint/journal artifacts from the local replica. NFS is
then used only as a coherent cold seed when both remain absent; distinct staging
uses a fresh per-attempt subdirectory, preventing shorter or timed-out transfers
from contaminating retries. A durable pending marker ensures a crash or partial
install discards both destinations before retry rather than mixing checkpoint
generations; it stays present through retry and is removed only after parsing.
Completed failed attempt directories are removed, while possibly active timed-out
directories are orphaned and never reused. Local file copies reject short/failed/changing source reads before
replacement, and deletion retries flush directory metadata even when the target
name is already absent.
Checkpoint/recovery lifecycles are serialized so a recovery image cannot resurrect
a key deleted after its journal read. Mutations serialize their durable append and
in-memory commit but do not hold the read lock during disk/replica I/O. Checkpoint
file writing permits concurrent mutations and retains the journal when its captured
write epoch was superseded. When local write-through mirroring
is enabled, a mirror append failure rolls the primary append back before returning
failure; inability to prove that rollback fail-stops instead of leaving a
success-shaped or restart-visible partial mutation. It also persists quarantine
evidence that blocks reads, mutations, imports, checkpoints, and recovery across
restart until an operator installs a verified checkpoint/journal and removes the
marker. Mutations and lifecycle calls refresh external quarantine evidence
immediately; healthy GET/QUERY calls cache only a negative probe for
`quarantine_probe_interval_ms` (default `1000`), may serve the last in-memory
image until that interval expires after an external marker appears, and latch
permanently once evidence is observed. Set the interval to `0` to probe on every
read. Local service-graph access performs one fail-closed recovery before its
first state operation, including checkpoints and prune mutations; an explicit
successful or failed recovery becomes that graph's recorded recovery outcome. The
detailed checkpoint RPC
returns the server-resolved path and whether journal compaction actually occurred;
mixed-version fallback keeps the old checkpoint RPC but reports that outcome as
unknown. Apply-mode pruning skips the full-value dry-run query and prefers a bounded
detailed delete response containing only the deleted count; old servers remain
supported through the legacy response.

Checkpoint files are currently retained without automatic garbage collection.
rDSN may still be transferring a path returned by `get_checkpoint()` after that
method returns, and this application has no transfer-release callback with which
to prove an older file is unused. Operators must monitor partition data-directory
growth and prune obsolete files only while the affected replica is stopped,
retaining its newest valid checkpoint. Failed conditional writes and rejected
RECOVER requests still consume a committed decree because classification precedes
application dispatch; they are deterministic no-ops, trading small log overhead
for identical replay on every replica.

The current profile intentionally has **one partition**. Existing `GET`/`QUERY`
semantics address one state authority and prefix queries do not fan out or merge
across replica groups. A multi-partition state table still requires client
fan-out/merge semantics. The eleven direct runtime-module tables in §13.15 use
their own quorum logs and checkpoints as the authority and therefore disable this
legacy mutation mirror. `rasn.state.replicated` remains useful for standalone
active/standby modules and application state outside the direct module stores.

Focused unit source covers deterministic sequence allocation and COPY-versus-LEARN
store semantics. Automated tests do not yet drive a real type-1 replica group
through checkpoint transfer, learning, and primary failover; operators can exercise
that path with `config.rasn.state.ini`, and cluster automation remains a tracked
deployment-validation gap.

### 13.14 HA registry membership/discovery — RESOLVED (code/config)

The default remains the lightweight process-local `agent_registry`, preserving
single-node behavior and builds without `plugins_ext`. Enabling
`[rasn.registry] shared_state_enabled = true` changes only the registry app's
backing authority:

1. `rasn_registry_app` creates its own registry instance and starts the current
   app's `rasn_coordination_context`.
2. Startup requires the resolved provider to be exactly `zookeeper`, the hosting
   app to list `THREAD_POOL_META_SERVER` and `THREAD_POOL_DLOCK`, and DLOCK to be
   partitioned. Pool wiring is validated before coordination initialization, so
   an incomplete deployment fails immediately rather than timing out. A missing
   dist plugin or accidental `inproc`/`simple` setting is likewise a startup error
   rather than an HA-shaped local fallback.
3. Every frontend opens the existing register/unregister/query/list/heartbeat task
   codes and reads the common `meta_state_service` tree.
4. Frontends contend for the preserved `rasn.registry.primary` lock. The winner
   installs its monotonically increasing grant version as the mutation fence,
   copies every live record from the previous committed global epoch while
   treating tombstones as absence, rebases dynamic leases into its own clock
   domain, reconciles
   `[rasn.agent.*]`, writes a schema-validated epoch marker, and only then exposes
   the new epoch. It owns dynamic writes plus lease sweeping; standby reads rely
   on the writer's expiry tombstones rather than comparing persisted timestamps
   with a different node's clock. After commit, the successor suppresses expiry
   for one complete lease so a long promotion cannot immediately tombstone the
   rebased records. HA startup therefore rejects
   `lease_ms != 0` together with `sweep_interval_ms = 0`.
   Standbys remain live read replicas and periodically retry election; reads fail
   closed while a newly acquired writer is still promoting its snapshot.
5. Loss callbacks revoke the local writer fence. A mutation that was already in
   flight writes only beneath its old epoch child, then rechecks the exact current
   lock owner/version and returns `registry_mutation_outcome_unknown` if leadership
   changed or the submitted write failed, so callers never replay a possibly
   committed mutation. Readers select
   the greatest valid **committed epoch**, read every agent from that exact epoch,
   and validate the marker's owner/fence against the authoritative ZooKeeper lock
   tree afterward. This final validation proves the selected epoch remained
   authoritative through the operation; a delayed old writer therefore cannot
   overwrite a successor or introduce an id that the successor never copied.
   Individual record updates within the active epoch are not a transactional
   snapshot. Shared reads do not hold the registry's local-map/writer mutex across
   backend I/O, so one frontend can execute independent reads concurrently.
   Because ordinary ZooKeeper reads may come from a lagging follower,
   that final `query_owner()` first completes a leader-ordered shared-state write
   barrier, then
   lists the lock children, reads the minimum-sequence owner's payload, and lists
   again to reject a concurrent handoff. The public rDSN `meta_state_service`
   interface exposes no native ZooKeeper `sync()`, so the same-session write is the
   one remaining barrier per registry read rather than two.
   If a timed-out acquisition or failed promotion cannot prove that its ZooKeeper
   node was released, the process fail-stops so the app-shared session cannot
   strand a ghost registry owner. The current rDSN provider also maps transient
   disconnect and true session expiry to the same loss callback; consequently an
   active registry process fail-stops on either event. Supervisors must restart it,
   and operators should treat ZooKeeper stability as an availability dependency.

The persisted record has an explicit magic/schema, descriptor, heartbeat time,
lease flag, tombstone, and writer fence. Agent ids (including URI-bearing runtime
ids with `/`) are hex encoded as path components. Decoding is bounded by the
existing agent wire-vector limits and rejects malformed, trailing, mismatched-id,
or mismatched-fence data. The committed marker separately records its schema,
fence, and writer identity. Backend/list/decode/ownership errors fail the typed
RPC; a missing agent child means absence only after a current committed epoch has
been validated.

`configured_rasn_registry_address()` is now the one address path used by the core
service graph, coordinator, runtime module discovery/registration/heartbeat, CLI,
and readiness logic. Its precedence is:

1. `registry_uri`;
2. comma-separated `registry_addresses` (a process-lifetime rDSN group);
3. legacy `registry_host`/`registry_port`, falling back to `host`.

All five registry client methods apply bounded linear-backoff failover. Transport
failure uses rDSN's automatic group-leader advancement; a standby mutation reply
uses the explicit `registry_not_primary` code to advance before retry. Query/list
also retry ambiguous `ERR_TIMEOUT` and rotate after a typed
`registry_backend_unavailable` read failure. Register/unregister/heartbeat retry
only typed pre-apply `registry_not_primary`/`registry_backend_unavailable` errors;
ambiguous timeouts/in-flight network failures and post-submission
`registry_mutation_outcome_unknown` responses rotate the group for the next call
but are returned to the caller because replay can overwrite a newer registration
or heartbeat (an ABA race). Heartbeat callers re-register only after the typed
`registry_agent_not_found` response, not after a generic rejection or uncertain
outcome. Existing outer
circuit-breaker protection in coordinator discovery remains useful but is no
longer the only failover layer.

This deliberately does **not** force descriptors into the replication
meta-server's table/partition protocol: `failure_detector` exposes node beacons
but not arbitrary capability records, while `partition_resolver` resolves
replicated tables. The generic facilities already supplied by rDSN for this shape
are `meta_state_service`, `distributed_lock_service`, and the RPC group address;
the ZooKeeper provider supplies cross-process session/lease failure detection.

Focused unit source covers persisted-record round trip/corruption including fence
zero, bounded frontend-list parsing/deduplication, absent-unregister semantics,
HA pool preconditions, tombstone-free promotion, and bounded epoch pruning. The
active writer retains `history_retained_epochs` (minimum 2, default 3), deletes
each old epoch's per-agent children before its marker, leaves shared agent parent
paths intact to avoid racing a successor, and retries every
`history_prune_interval_ms` (0 means promotion-only). Cleanup is best effort:
failure never revokes a valid writer or blocks service, and all deletes are
idempotent and restricted to epochs older than its fence.

The remaining evidence gap is an automated Linux scenario that launches multiple
registry-only frontend processes against ZooKeeper, proves concurrent shared
reads and standby mutation forwarding, kills the writer, observes a committed
greater-epoch takeover, and validates online retention against the real provider.

### 13.15 Direct quorum-replicated runtime modules — RESOLVED (code/config)

`config.rasn.runtime.replicated.ini` deploys every common runtime module as its
own rDSN `replicated_service_app_type_1` table. Separate tables are intentional:
the three naturally sharded modules need four partitions in the reference
profile, while the other modules use one, and every module needs an independent
placement, mutation-log, checkpoint, learning, and failover boundary. Every
partition has three replicas in the development profile.

The wire API remains the common `rasn_runtime_request`/`response` facade, but
task classification is now explicit:

- Existing `RPC_RASN_<MODULE>` codes are reads. Eleven parallel
  `RPC_RASN_<MODULE>_WRITE` codes are statically marked
  `rpc_request_is_write_operation = true`, so rDSN commits mutations through the
  replication protocol before app dispatch.
- A replicated handler rejects a mutation on the read channel, a read on the
  write channel, and a request whose normalized partition does not match the
  handler GPID. This prevents an old or misrouted client from bypassing quorum
  commit.
- For rolling standalone upgrades only, a new client retries a mutation on the
  legacy read code when the write code returns exactly
  `ERR_HANDLER_NOT_FOUND`. Ambiguous or application errors never trigger that
  compatibility replay.

Replica apply is deterministic. Clients materialize generated IDs and timestamps
before sending mutations; the replica rejects operations that still require
local wall-clock or ID generation. Every mutation also requires a stable
`request_id`. Each partition checkpoints a bounded FIFO request-signature/result
window with module state, so an ambiguous retry after primary failover returns
the original result instead of applying twice. Native eviction uses a
protocol-fixed capacity of 8192; app startup fails unless the visible config
matches that constant, so node-local tuning cannot make replicas diverge.

The `human_interaction` singleton is a live replicated queue rather than a
checkpoint-only mirror. The runtime facade routes open, answer, cancel, and
deadline-expiry mutations through its write code, and routes find, requester-
filtered pending, snapshot, and describe operations through its read code.
Request IDs and transition timestamps are materialized by the client facade
before replication; checkpoint hydration remains an internal recovery-only
operation. It is one partition in the reference profile. Global expiry,
snapshot, and pending operations nevertheless use the partition fan-out path;
replicated ingress requires each expiry write to carry its explicit partition.
This prevents a future shard-count change from silently expiring only the shard
selected by the literal `"*"` key.

Checkpoint files are named `rasn-runtime-checkpoint.<decree>` in each partition
data directory and reuse the validated `state_store` snapshot format without
enabling its standalone journal. Every image carries a required module/schema/
dedup-capacity/GPID/decree manifest plus a record count and content digest, so
truncation, a wrong table/partition, or inconsistent replica capacity fails
validation instead of becoming an empty store. Checkpoint record keys use a
length-prefixed full-byte encoding and are checked for collisions before write.
`sync_checkpoint()` snapshots module state and dedup together;
`get_checkpoint()` exports the latest durable image;
`DSN_CHKPT_COPY` validates and persists without replacing live memory; and
`DSN_CHKPT_LEARN` validates, persists, atomically replaces the store, then lets
rDSN replay later decrees. Startup scans newest-to-oldest and recovers the newest
valid checkpoint. Automatic old-checkpoint deletion is intentionally deferred
because the public framework callback does not signal when a transferred path is
no longer in use; prune offline while retaining the newest valid image.

Launch the single-process development cluster with:

```text
codepilot serve config.rasn.runtime.replicated.ini "meta;replica"
# or
srepilot serve config.rasn.runtime.replicated.ini "meta;replica"
```

That profile co-hosts one meta server and three replica servers for convenience.
Production must separate and supervise meta/replica roles across failure domains,
protect the quorum log/checkpoints at rest, and keep each configured module shard
count equal to its meta-table partition count. The profile disables legacy
runtime-state hydration/mirroring and the standalone ownership gate because the
quorum log, checkpoints, and rDSN primary lease are authoritative.

Shared-token authentication cannot be evaluated safely after type-1 write
interception: node-local token drift would make one committed decree apply
differently across replicas. Native apps therefore fail startup when it is
enabled; use network/transport identity and isolation until authentication can be
validated before replication. Remaining work is deployment evidence rather than
an alternate consensus implementation: automate real multi-host primary
failover/checkpoint transfer, add standalone-to-table migration tooling, and
generate typed module RPC schemas.
