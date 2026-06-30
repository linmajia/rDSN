#include "skills.h"

#include <sstream>

namespace dsn {
namespace rasn {

std::vector<codepilot_skill_metadata> codepilot_skills()
{
    std::vector<codepilot_skill_metadata> skills;
    skills.push_back({"rdsn-plugin",
                      "Design or modify an rDSN plugin.",
                      "Act as an rDSN plugin engineer. Identify the plugin CMake wiring, service_app or module entry points, runtime config, and focused validation steps."});
    skills.push_back({"code-review",
                      "Review code for correctness, reliability, and security.",
                      "Act as a strict code reviewer. Focus only on correctness, security, reliability, resource lifetime, and testability. Avoid style-only comments."});
    skills.push_back({"debug-build",
                      "Diagnose compiler, linker, or test failures.",
                      "Act as a build failure investigator. Extract the first root-cause diagnostic, map it to source, suggest a minimal fix, and propose a targeted rebuild command."});
    skills.push_back({"feature-plan",
                      "Plan a feature implementation.",
                      "Act as a senior engineer planning a feature. Produce a small phased plan, name affected files, identify risks, and define validation."});
    skills.push_back({"docs",
                      "Write user-facing engineering documentation.",
                      "Act as a technical writer for systems software. Explain design, operations, configuration, and troubleshooting with concrete commands."});
    return skills;
}

bool find_codepilot_skill(const std::string &name, codepilot_skill_metadata *skill)
{
    const std::vector<codepilot_skill_metadata> skills = codepilot_skills();
    for (const codepilot_skill_metadata &candidate : skills)
    {
        if (candidate.name == name)
        {
            if (skill != nullptr)
            {
                *skill = candidate;
            }
            return true;
        }
    }
    return false;
}

std::string describe_codepilot_skills()
{
    std::ostringstream oss;
    oss << "CodePilot skills:\n";
    for (const codepilot_skill_metadata &skill : codepilot_skills())
    {
        oss << "- " << skill.name << ": " << skill.description << "\n";
    }
    return oss.str();
}

} // namespace rasn
} // namespace dsn
