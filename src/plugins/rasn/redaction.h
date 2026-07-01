#pragma once

#include <string>
#include <vector>

namespace dsn {
namespace rasn {

std::string redact_sensitive_text(const std::string &text);
std::string redact_sensitive_text(const std::string &text, const std::vector<std::string> &secret_values);

// Register a resolved secret value (e.g. a bearer token loaded from a file: or
// cmd: credential_ref) so it is scrubbed from traces even though it never appears
// in an environment variable. Thread-safe; process-wide.
void register_runtime_secret(const std::string &secret);

} // namespace rasn
} // namespace dsn
