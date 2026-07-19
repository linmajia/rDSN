#!/usr/bin/env python3
from __future__ import print_function

import argparse
import errno
import io
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import uuid


MODULES = [
    "agent_control_plane",
    "agent_message_bus",
    "task_orchestration_kernel",
    "determinism_ledger",
    "sandbox_runtime",
    "capability_directory",
    "resource_budget",
    "recovery_supervisor",
    "blackboard",
    "contract_verifier",
    "human_interaction",
]

TYPED_RUNTIME_SCHEMAS = {
    "agent_control_plane": (
        "RPC_RASN_AGENT_CONTROL",
        "agent_control_request",
        "agent_control_response",
    ),
    "agent_message_bus": (
        "RPC_RASN_MESSAGE_BUS",
        "message_bus_request",
        "message_bus_response",
    ),
    "task_orchestration_kernel": (
        "RPC_RASN_TASK_ORCHESTRATION",
        "task_orchestration_request",
        "task_orchestration_response",
    ),
    "determinism_ledger": (
        "RPC_RASN_DETERMINISM_LEDGER",
        "determinism_request",
        "determinism_response",
    ),
    "capability_directory": (
        "RPC_RASN_CAPABILITY_DIRECTORY",
        "capability_directory_request",
        "capability_directory_response",
    ),
    "resource_budget": (
        "RPC_RASN_RESOURCE_BUDGET",
        "resource_budget_request",
        "resource_budget_response",
    ),
    "recovery_supervisor": (
        "RPC_RASN_RECOVERY_SUPERVISOR",
        "recovery_supervisor_request",
        "recovery_supervisor_response",
    ),
    "blackboard": (
        "RPC_RASN_BLACKBOARD",
        "blackboard_request",
        "blackboard_response",
    ),
    "contract_verifier": (
        "RPC_RASN_CONTRACT_VERIFIER",
        "contract_verifier_request",
        "contract_verifier_response",
    ),
    "human_interaction": (
        "RPC_RASN_HUMAN_INTERACTION",
        "human_interaction_rpc_request",
        "human_interaction_rpc_response",
    ),
    "sandbox_runtime": (
        "RPC_RASN_SANDBOX_RUNTIME",
        "sandbox_runtime_request",
        "sandbox_runtime_response",
    ),
}

TYPED_RUNTIME_SCHEMA_ENTRY = re.compile(
    r"^    // rasn\.runtime\.(?P<module>[a-z0-9_]+)\."
    r"(?P<method>[a-z0-9_]+) -> (?P<task_code>RPC_RASN_[A-Z0-9_]+)\n"
    r"(?:^    // routing: [^\n]+\n)?"
    r"^    std::pair< ::dsn::error_code, "
    r"(?P<response>dsn::rasn::rpc::[a-z0-9_]+)>\n"
    r"^    (?P=method)_sync\(const "
    r"(?P<request>dsn::rasn::rpc::[a-z0-9_]+) &request,\n"
    r"^                  std::chrono::milliseconds timeout = "
    r"std::chrono::milliseconds\(0\),\n"
    r"^                  int thread_hash = 0,\n"
    r"^                  std::uint64_t partition_hash = 0\)\n"
    r"^    \{\n"
    r"^        return ::dsn::rpc::wait_and_unwrap<(?P=response)>"
    r"\(::dsn::rpc::call\(\n"
    r"^            _server, (?P=task_code), request, nullptr, empty_callback, "
    r"timeout, thread_hash, partition_hash\)\);\n"
    r"^    \}$",
    re.MULTILINE,
)

SERVICE_SPECS = [
    ("registry", "apps.rasn.registry", "registry"),
    ("llm_agent", "apps.rasn.llm.agent", "llm_agent"),
    ("tool_agent", "apps.rasn.tool.agent", "tool_agent"),
    ("state", "apps.rasn.state", "state"),
    ("coordinator", "apps.rasn.coordinator", "coordinator"),
    ("workflow", "apps.rasn.workflow", "workflow"),
    ("observability", "apps.rasn.observability", "observability"),
    ("rasn_runtime", "apps.rasn.runtime", "rasn_runtime"),
]

STATIC_FALLBACK_PORT = 27107
OWNERSHIP_HANDOFF_TIMEOUT_SECONDS = 60.0
OWNERSHIP_PRE_HANDOFF_PROBE_TIMEOUT_SECONDS = 20.0
OWNERSHIP_RETRY_BACKOFF_MS = 250
OWNERSHIP_RETRY_MARGIN_SECONDS = 15.0
OWNERSHIP_ACQUIRE_MAX_ATTEMPTS = (
    int(
        (
            OWNERSHIP_HANDOFF_TIMEOUT_SECONDS
            + OWNERSHIP_PRE_HANDOFF_PROBE_TIMEOUT_SECONDS
            + OWNERSHIP_RETRY_MARGIN_SECONDS
        )
        * 1000
        / OWNERSHIP_RETRY_BACKOFF_MS
    )
    + 1
)


class HarnessError(RuntimeError):
    pass


class ChildProcess(object):
    def __init__(self, name, process, log_path, log_file):
        self.name = name
        self.process = process
        self.log_path = log_path
        self.log_file = log_file
        self.closed = False


def read_text(path):
    with io.open(path, "r", encoding="utf-8") as source:
        return source.read()


def write_text(path, text):
    with io.open(path, "w", encoding="utf-8", newline="\n") as target:
        target.write(text)


def set_ini_value(text, section, key, value):
    lines = text.splitlines()
    section_header = "[{0}]".format(section)
    section_start = None
    section_end = len(lines)
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped == section_header:
            section_start = index
            continue
        if section_start is not None and stripped.startswith("[") and stripped.endswith("]"):
            section_end = index
            break

    replacement = "{0} = {1}".format(key, value)
    if section_start is None:
        if lines and lines[-1] != "":
            lines.append("")
        lines.extend([section_header, replacement])
        return "\n".join(lines) + "\n"

    key_pattern = re.compile(r"^\s*" + re.escape(key) + r"\s*=")
    for index in range(section_start + 1, section_end):
        stripped = lines[index].lstrip()
        if stripped.startswith(";") or stripped.startswith("#"):
            continue
        if key_pattern.match(lines[index]):
            lines[index] = replacement
            return "\n".join(lines) + "\n"

    lines.insert(section_end, replacement)
    return "\n".join(lines) + "\n"


def apply_ini_updates(text, updates):
    for section, key, value in updates:
        text = set_ini_value(text, section, key, str(value))
    return text


def make_directory(path):
    try:
        os.makedirs(path)
    except OSError as error:
        if error.errno != errno.EEXIST:
            raise


def install_binary(source, directory):
    target = os.path.join(directory, os.path.basename(source))
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)
    os.chmod(target, os.stat(target).st_mode | 0o111)
    return target


def allocate_ports(names, excluded=None):
    excluded = set(excluded or [])
    sockets = []
    ports = {}
    try:
        for name in names:
            while True:
                listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                listener.bind(("127.0.0.1", 0))
                port = listener.getsockname()[1]
                if port in excluded or port in ports.values():
                    listener.close()
                    continue
                listener.listen(1)
                sockets.append(listener)
                ports[name] = port
                break
    finally:
        for listener in sockets:
            listener.close()
    return ports


def allocate_service_ports():
    return allocate_ports(
        [spec[0] for spec in SERVICE_SPECS],
        excluded=[STATIC_FALLBACK_PORT],
    )


def host_config_updates(directory, ports):
    updates = [
        ("core", "data_dir", os.path.join(directory, "data")),
    ]
    for port_name, app_section, service_key in SERVICE_SPECS:
        updates.append((app_section, "ports", ports[port_name]))
        updates.append(("rasn.service", service_key + "_host", "127.0.0.1"))
        updates.append(("rasn.service", service_key + "_port", ports[port_name]))
    updates.append(("rasn.service", "rasn_runtime_advertise_host", "127.0.0.1"))
    return updates


def prepare_host(args, root, name, ports, config_updates=None, defaults_updates=None):
    directory = os.path.join(root, name)
    make_directory(directory)
    binary = install_binary(args.binary, directory)

    runtime_text = read_text(args.runtime_config)
    runtime_text = apply_ini_updates(
        runtime_text,
        host_config_updates(directory, ports) + list(config_updates or []),
    )
    write_text(os.path.join(directory, "config.rasn.ini"), runtime_text)

    defaults_text = read_text(args.defaults_config)
    defaults_text = apply_ini_updates(defaults_text, list(defaults_updates or []))
    write_text(os.path.join(directory, "config.rasn.defaults.ini"), defaults_text)
    return directory, binary


def prepare_client(args, root, name, service_updates):
    directory = os.path.join(root, name)
    make_directory(directory)
    binary = install_binary(args.binary, directory)
    updates = [
        ("core", "data_dir", os.path.join(directory, "data")),
        ("rasn.runtime", "rasn_runtime_mode", "distributed"),
        ("rasn.runtime", "rasn_runtime_provider", "distributed"),
    ]
    updates.extend(service_updates)
    client_text = apply_ini_updates(read_text(args.client_config), updates)
    write_text(os.path.join(directory, "config.ini"), client_text)
    return directory, binary


def tail_file(path, lines=80):
    try:
        content = read_text(path).splitlines()
    except (IOError, OSError):
        return "<log unavailable>"
    return "\n".join(content[-lines:])


def terminate_process_group(process, grace_seconds=5.0):
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except OSError as error:
        if error.errno != errno.ESRCH:
            raise
    try:
        process.wait(timeout=grace_seconds)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        os.killpg(process.pid, signal.SIGKILL)
    except OSError as error:
        if error.errno != errno.ESRCH:
            raise
    process.wait()


class Harness(object):
    def __init__(self, args, root):
        self.args = args
        self.root = root
        self.children = []
        self.command_counter = 0

    def start_host(self, name, directory, binary, app_list=None):
        log_path = os.path.join(directory, "host.log")
        log_file = io.open(log_path, "w", encoding="utf-8")
        command = [binary, "serve", os.path.join(directory, "config.rasn.ini")]
        if app_list is not None:
            command.append(app_list)
        try:
            process = subprocess.Popen(
                command,
                cwd=directory,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except Exception:
            log_file.close()
            raise
        child = ChildProcess(name, process, log_path, log_file)
        self.children.append(child)
        return child

    def stop_child(self, child):
        if child.closed:
            return
        terminate_process_group(child.process)
        child.log_file.close()
        child.closed = True

    def stop_all(self):
        for child in reversed(self.children):
            try:
                self.stop_child(child)
            except Exception as error:
                print("warning: failed to stop {0}: {1}".format(child.name, error), file=sys.stderr)

    def assert_running(self, child):
        status = child.process.poll()
        if status is not None:
            child.log_file.flush()
            raise HarnessError(
                "{0} exited with status {1}\n{2}".format(
                    child.name,
                    status,
                    tail_file(child.log_path),
                )
            )

    def wait_for_ports(self, child, ports, timeout=40.0):
        deadline = time.monotonic() + timeout
        pending = set(ports)
        while pending and time.monotonic() < deadline:
            self.assert_running(child)
            for port in list(pending):
                try:
                    connection = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                    connection.close()
                    pending.remove(port)
                except OSError:
                    pass
            if pending:
                time.sleep(0.1)
        if pending:
            child.log_file.flush()
            raise HarnessError(
                "{0} did not listen on ports {1}\n{2}".format(
                    child.name,
                    sorted(pending),
                    tail_file(child.log_path),
                )
            )

    def run_client(self, directory, binary, command, label, timeout=60.0):
        self.command_counter += 1
        log_path = os.path.join(
            directory,
            "{0}-{1:03d}.log".format(label, self.command_counter),
        )
        process = subprocess.Popen(
            [binary] + list(command),
            cwd=directory,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        timed_out = False
        try:
            try:
                output, _ = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                terminate_process_group(process, grace_seconds=1.0)
                output, _ = process.communicate()
        finally:
            if process.poll() is None:
                terminate_process_group(process, grace_seconds=1.0)
        output = (output or b"").decode("utf-8", "replace")
        write_text(log_path, output)
        return (124 if timed_out else process.returncode), output

    def wait_for_topology(self, directory, binary, source, port, timeout=40.0):
        deadline = time.monotonic() + timeout
        last_output = ""
        while time.monotonic() < deadline:
            status, output = self.run_client(
                directory,
                binary,
                ["runtime", "topology"],
                "topology",
                timeout=20.0,
            )
            last_output = output
            endpoints = dict(
                re.findall(
                    r"^\s+([a-z_]+)\s+routing=remote\s+endpoint=([^\s]+)",
                    output,
                    flags=re.MULTILINE,
                )
            )
            if status == 0 and all(
                module in endpoints
                and endpoints[module].startswith(source + ":")
                and ":{0}".format(port) in endpoints[module]
                for module in MODULES
            ):
                return output
            time.sleep(0.5)
        raise HarnessError(
            "runtime topology did not resolve all modules through {0} port {1}\n{2}".format(
                source,
                port,
                last_output,
            )
        )

    def wait_for_health(self, directory, binary, timeout=40.0, host=None):
        deadline = time.monotonic() + timeout
        last_status = None
        last_output = ""
        while time.monotonic() < deadline:
            if host is not None:
                self.assert_running(host)
            last_status, last_output = self.run_client(
                directory,
                binary,
                ["runtime", "health"],
                "health",
                timeout=25.0,
            )
            if host is not None:
                self.assert_running(host)
            if (
                last_status == 0
                and "[PASS] all 11 rASN runtime modules reachable" in last_output
                and "provider=distributed" in last_output
            ):
                return last_output
            time.sleep(0.5)
        raise HarnessError(
            "runtime health did not become ready (status={0})\n{1}".format(
                last_status,
                last_output,
            )
        )

    def assert_selftest(self, directory, binary):
        status, output = self.run_client(
            directory,
            binary,
            ["selftest"],
            "selftest",
            timeout=90.0,
        )
        if (
            status != 0
            or "rASN self-test passed" not in output
            or "all 11 rASN runtime modules reachable (provider=distributed, mode=distributed)"
            not in output
        ):
            raise HarnessError(
                "distributed self-test failed (status={0})\n{1}".format(status, output)
            )

    def assert_typed_runtime_schema(self, directory, binary):
        status, output = self.run_client(
            directory,
            binary,
            ["schema", "clients-cpp"],
            "typed-runtime-schema",
            timeout=30.0,
        )
        expected = set()
        for module, schema in TYPED_RUNTIME_SCHEMAS.items():
            task_code, request_type, response_type = schema
            qualified_request = "dsn::rasn::rpc::{0}".format(request_type)
            qualified_response = "dsn::rasn::rpc::{0}".format(response_type)
            expected.add(
                (module, module, task_code, qualified_request, qualified_response)
            )
            expected.add(
                (
                    module,
                    module + "_write",
                    task_code + "_WRITE",
                    qualified_request,
                    qualified_response,
                )
            )

        found = [
            (
                match.group("module"),
                match.group("method"),
                match.group("task_code"),
                match.group("request"),
                match.group("response"),
            )
            for match in TYPED_RUNTIME_SCHEMA_ENTRY.finditer(output)
        ]
        found_set = set(found)
        runtime_declarations = re.findall(
            r"^    // rasn\.runtime\.[^\n]+$", output, re.MULTILINE
        )
        if (
            status != 0
            or len(found) != len(found_set)
            or found_set != expected
            or len(runtime_declarations) != len(expected)
        ):
            missing = sorted(expected - found_set)
            unexpected = sorted(found_set - expected)
            raise HarnessError(
                "generated typed runtime schema discovery failed "
                "(status={0}, entries={1}, declarations={2}, "
                "missing={3}, unexpected={4})\n{5}".format(
                    status,
                    len(found),
                    len(runtime_declarations),
                    missing,
                    unexpected,
                    output,
                )
            )


def explicit_scenario(harness):
    root = os.path.join(harness.root, "explicit")
    make_directory(root)
    ports = allocate_service_ports()
    host_dir, host_binary = prepare_host(
        harness.args,
        root,
        "host",
        ports,
        config_updates=[
            ("rasn.service", "rasn_runtime_registry_discovery_enabled", "false"),
        ],
    )
    host = harness.start_host("explicit runtime host", host_dir, host_binary)
    harness.wait_for_ports(
        host,
        [ports["registry"], ports["state"], ports["rasn_runtime"]],
    )

    client_dir, client_binary = prepare_client(
        harness.args,
        root,
        "client",
        [
            ("rasn.service", "rasn_runtime_registry_discovery_enabled", "false"),
            ("rasn.service", "rasn_runtime_host", "127.0.0.1"),
            ("rasn.service", "rasn_runtime_port", ports["rasn_runtime"]),
        ],
    )
    harness.wait_for_topology(
        client_dir,
        client_binary,
        "static",
        ports["rasn_runtime"],
    )
    harness.assert_typed_runtime_schema(client_dir, client_binary)
    harness.wait_for_health(client_dir, client_binary, host=host)
    harness.assert_selftest(client_dir, client_binary)
    harness.stop_child(host)
    print("[PASS] explicit endpoint routing: all 11 runtime modules")


def reserve_static_fallback():
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", STATIC_FALLBACK_PORT))
        listener.listen(8)
    except OSError:
        listener.close()
        raise HarnessError(
            "registry scenario requires static fallback port {0} to be unused".format(
                STATIC_FALLBACK_PORT
            )
        )
    return listener


def registry_scenario(harness):
    root = os.path.join(harness.root, "registry")
    make_directory(root)
    fallback_listener = reserve_static_fallback()
    try:
        ports = allocate_service_ports()
        host_dir, host_binary = prepare_host(
            harness.args,
            root,
            "host",
            ports,
            config_updates=[
                ("rasn.service", "rasn_runtime_registry_discovery_enabled", "true"),
                ("rasn.service", "rasn_runtime_registry_registration_enabled", "true"),
                ("rasn.service", "rasn_runtime_advertise_host", "127.0.0.1"),
            ],
        )
        host = harness.start_host("registry runtime host", host_dir, host_binary)
        harness.wait_for_ports(
            host,
            [ports["registry"], ports["state"], ports["rasn_runtime"]],
        )

        client_dir, client_binary = prepare_client(
            harness.args,
            root,
            "client",
            [
                ("rasn.service", "registry_host", "127.0.0.1"),
                ("rasn.service", "registry_port", ports["registry"]),
                ("rasn.service", "rasn_runtime_registry_discovery_enabled", "true"),
            ],
        )
        harness.wait_for_topology(
            client_dir,
            client_binary,
            "registry",
            ports["rasn_runtime"],
            timeout=60.0,
        )
        harness.wait_for_health(client_dir, client_binary, host=host)
        harness.assert_selftest(client_dir, client_binary)
        harness.stop_child(host)
    finally:
        fallback_listener.close()
    print(
        "[PASS] registry-only discovery: all 11 endpoints resolved as registry: "
        "(static fallback {0} reserved)".format(STATIC_FALLBACK_PORT)
    )


def ownership_client_updates(runtime_port):
    return [
        ("rasn.service", "rasn_runtime_registry_discovery_enabled", "false"),
        ("rasn.service", "rasn_runtime_host", "127.0.0.1"),
        ("rasn.service", "rasn_runtime_port", runtime_port),
        ("rasn.service", "rasn_runtime_timeout_ms", "300"),
        ("rasn.service", "rasn_runtime_ping_timeout_ms", "200"),
        ("rasn.service", "rasn_runtime_retries", "0"),
        ("rasn.service", "rasn_runtime_breaker_enabled", "false"),
    ]


def ownership_host_updates(state_port, runtime_port):
    return [
        ("apps.rasn.runtime", "ports", runtime_port),
        (
            "apps.rasn.runtime",
            "pools",
            "THREAD_POOL_DEFAULT,THREAD_POOL_META_SERVER,THREAD_POOL_DLOCK",
        ),
        ("rasn.service", "state_host", "127.0.0.1"),
        ("rasn.service", "state_port", state_port),
        ("rasn.service", "rasn_runtime_port", runtime_port),
        ("rasn.service", "rasn_runtime_registry_discovery_enabled", "false"),
        ("rasn.service", "rasn_runtime_registry_registration_enabled", "false"),
        ("rasn.service", "rasn_runtime_ownership_gate_enabled", "true"),
        (
            "rasn.service",
            "rasn_runtime_ownership_acquire_max_attempts",
            OWNERSHIP_ACQUIRE_MAX_ATTEMPTS,
        ),
        (
            "rasn.service",
            "rasn_runtime_ownership_acquire_retry_backoff_ms",
            OWNERSHIP_RETRY_BACKOFF_MS,
        ),
        ("threadpool.THREAD_POOL_META_SERVER", "partitioned", "false"),
        ("threadpool.THREAD_POOL_META_SERVER", "worker_count", "2"),
        ("threadpool.THREAD_POOL_DLOCK", "partitioned", "true"),
    ]


def ownership_scenario(harness, zk_hosts):
    root = os.path.join(harness.root, "ownership")
    make_directory(root)
    namespace = "/rasn/integration/{0}".format(uuid.uuid4().hex)
    defaults_updates = [
        ("rasn.coordination", "provider", "zookeeper"),
        ("rasn.coordination", "lock_namespace", namespace + "/locks"),
        ("rasn.coordination", "state_namespace", namespace + "/state"),
        ("rasn.coordination", "acquire_timeout_ms", "1000"),
        ("zookeeper", "hosts_list", zk_hosts),
        ("zookeeper", "timeout_ms", "3000"),
        ("zookeeper", "logfile", "zookeeper.log"),
    ]

    state_ports = allocate_service_ports()
    state_dir, state_binary = prepare_host(
        harness.args,
        root,
        "state",
        state_ports,
        config_updates=[
            ("rasn.service", "rasn_runtime_ownership_gate_enabled", "false"),
            ("rasn.service", "rasn_runtime_registry_registration_enabled", "false"),
        ],
    )
    state = harness.start_host("ownership state service", state_dir, state_binary, "rasn.state")
    harness.wait_for_ports(state, [state_ports["state"]])

    active_ports = allocate_service_ports()
    active_dir, active_binary = prepare_host(
        harness.args,
        root,
        "active",
        active_ports,
        config_updates=ownership_host_updates(
            state_ports["state"],
            active_ports["rasn_runtime"],
        ),
        defaults_updates=defaults_updates,
    )
    active = harness.start_host("ownership active", active_dir, active_binary, "rasn.runtime")
    active_client_dir, active_client_binary = prepare_client(
        harness.args,
        root,
        "active-client",
        ownership_client_updates(active_ports["rasn_runtime"]),
    )
    harness.wait_for_health(
        active_client_dir,
        active_client_binary,
        timeout=OWNERSHIP_HANDOFF_TIMEOUT_SECONDS,
        host=active,
    )

    standby_ports = allocate_service_ports()
    standby_dir, standby_binary = prepare_host(
        harness.args,
        root,
        "standby",
        standby_ports,
        config_updates=ownership_host_updates(
            state_ports["state"],
            standby_ports["rasn_runtime"],
        ),
        defaults_updates=defaults_updates,
    )
    standby = harness.start_host("ownership standby", standby_dir, standby_binary, "rasn.runtime")
    time.sleep(1.0)
    harness.assert_running(standby)
    standby_client_dir, standby_client_binary = prepare_client(
        harness.args,
        root,
        "standby-client",
        ownership_client_updates(standby_ports["rasn_runtime"]),
    )
    status, output = harness.run_client(
        standby_client_dir,
        standby_client_binary,
        ["runtime", "health"],
        "health-before-handoff",
        timeout=OWNERSHIP_PRE_HANDOFF_PROBE_TIMEOUT_SECONDS,
    )
    if status == 0:
        raise HarnessError(
            "standby served runtime RPC before owning the modules (status={0})\n{1}".format(
                status,
                output,
            )
        )
    if "[FAIL]" not in output:
        raise HarnessError(
            "standby health probe did not report the expected ownership-blocked failure "
            "(status={0})\n{1}".format(status, output)
        )

    harness.stop_child(active)
    harness.wait_for_health(
        standby_client_dir,
        standby_client_binary,
        timeout=OWNERSHIP_HANDOFF_TIMEOUT_SECONDS,
        host=standby,
    )
    harness.stop_child(standby)
    harness.stop_child(state)
    print("[PASS] ZooKeeper ownership gate: standby took over after active exit")


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Run isolated multi-process rASN deployment checks",
    )
    script_dir = os.path.dirname(os.path.abspath(__file__))
    parser.add_argument("--binary", default=os.path.join(script_dir, "codepilot"))
    parser.add_argument("--client-config", default=os.path.join(script_dir, "config.ini"))
    parser.add_argument(
        "--runtime-config",
        default=os.path.join(script_dir, "config.rasn.ini"),
    )
    parser.add_argument(
        "--defaults-config",
        default=os.path.join(script_dir, "config.rasn.defaults.ini"),
    )
    parser.add_argument(
        "--scenario",
        choices=["all", "explicit", "registry", "ownership"],
        default="all",
    )
    parser.add_argument("--keep-artifacts", action="store_true")
    return parser.parse_args(argv)


def validate_inputs(args):
    for label, path in [
        ("CodePilot binary", args.binary),
        ("client config", args.client_config),
        ("runtime config", args.runtime_config),
        ("defaults config", args.defaults_config),
    ]:
        if not os.path.isfile(path):
            raise HarnessError("{0} not found: {1}".format(label, path))
    if not os.access(args.binary, os.X_OK):
        raise HarnessError("CodePilot binary is not executable: {0}".format(args.binary))


def main(argv):
    args = parse_args(argv)
    if sys.platform != "linux":
        print("SKIP: rASN multi-process integration harness is Linux-only")
        return 0
    try:
        validate_inputs(args)
    except HarnessError as error:
        print("rASN multi-process integration checks failed: {0}".format(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("rASN multi-process integration checks interrupted", file=sys.stderr)
        return 130

    artifact_parent = os.environ.get("RASN_MULTINODE_ARTIFACT_ROOT") or os.environ.get(
        "DSN_TEST_TMP_DIR"
    )
    if artifact_parent:
        make_directory(artifact_parent)
    root = tempfile.mkdtemp(prefix="rasn-multinode-", dir=artifact_parent)
    keep_artifacts = args.keep_artifacts or os.environ.get("RASN_MULTINODE_KEEP_ARTIFACTS") == "1"
    harness = Harness(args, root)
    succeeded = False
    try:
        if args.scenario in ("all", "explicit"):
            explicit_scenario(harness)
        if args.scenario in ("all", "registry"):
            registry_scenario(harness)

        zk_hosts = os.environ.get("RASN_MULTINODE_ZK_HOSTS", "").strip()
        if args.scenario == "ownership" and not zk_hosts:
            raise HarnessError(
                "RASN_MULTINODE_ZK_HOSTS is required for the ownership scenario"
            )
        if zk_hosts and args.scenario in ("all", "ownership"):
            ownership_scenario(harness, zk_hosts)
        elif args.scenario == "all":
            print("[SKIP] ZooKeeper ownership handoff (RASN_MULTINODE_ZK_HOSTS is unset)")
        succeeded = True
        print("rASN multi-process integration checks passed")
        return 0
    except HarnessError as error:
        print("rASN multi-process integration checks failed: {0}".format(error), file=sys.stderr)
        return 1
    finally:
        harness.stop_all()
        if succeeded and not keep_artifacts:
            shutil.rmtree(root)
        else:
            print("artifacts: {0}".format(root), file=sys.stderr)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
