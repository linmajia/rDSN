#pragma once

#include <string>
#include <vector>

namespace dsn {
namespace rasn {

std::string redact_sensitive_text(const std::string &text);
std::string redact_sensitive_text(const std::string &text, const std::vector<std::string> &secret_values);

} // namespace rasn
} // namespace dsn
