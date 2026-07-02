#pragma once

#include <dsn/cpp/zlocks.h>

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace dsn {
namespace rasn {

struct agent_contract
{
    std::string contract_id;
    bool require_input_non_empty = false;
    bool require_output_non_empty = true;
    size_t max_output_bytes = 0;
    std::vector<std::string> required_input_fragments;
    std::vector<std::string> required_output_fragments;
    std::vector<std::string> forbidden_output_fragments;
    std::vector<std::string> required_policy_labels;
};

struct contract_evaluation
{
    bool ok = true;
    std::string contract_id;
    std::vector<std::string> violations;
    std::vector<std::string> warnings;
};

class contract_verifier
{
public:
    bool register_contract(const agent_contract &contract, std::string *error);
    bool remove_contract(const std::string &contract_id, std::string *error);
    bool find_contract(const std::string &contract_id, agent_contract *contract) const;
    contract_evaluation evaluate_input(const std::string &contract_id, const std::string &input) const;
    contract_evaluation evaluate_output(const std::string &contract_id,
                                        const std::string &output,
                                        const std::vector<std::string> &policy_labels) const;
    contract_evaluation evaluate(const std::string &contract_id,
                                 const std::string &input,
                                 const std::string &output,
                                 const std::vector<std::string> &policy_labels) const;
    std::vector<agent_contract> list_contracts() const;
    std::string describe() const;

private:
    contract_evaluation evaluate_contract(const agent_contract &contract,
                                          const std::string *input,
                                          const std::string *output,
                                          const std::vector<std::string> *policy_labels) const;
    bool contains_all(const std::string &text, const std::vector<std::string> &fragments, std::vector<std::string> *missing) const;

    mutable ::dsn::service::zlock _lock;
    std::map<std::string, agent_contract> _contracts;
};

} // namespace rasn
} // namespace dsn

