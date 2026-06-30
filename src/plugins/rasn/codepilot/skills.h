#pragma once

#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct codepilot_skill_metadata
{
    std::string name;
    std::string description;
    std::string prompt;
};

std::vector<codepilot_skill_metadata> codepilot_skills();
bool find_codepilot_skill(const std::string &name, codepilot_skill_metadata *skill);
std::string describe_codepilot_skills();

} // namespace rasn
} // namespace dsn
