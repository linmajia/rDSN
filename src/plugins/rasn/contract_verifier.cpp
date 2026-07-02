#include <rasn/contract_verifier.h>

#include <algorithm>
#include <sstream>

namespace dsn {
namespace rasn {

bool contract_verifier::register_contract(const agent_contract &contract, std::string *error)
{
    if (contract.contract_id.empty())
    {
        if (error != nullptr)
        {
            *error = "agent contract missing id";
        }
        return false;
    }
    ::dsn::service::zauto_lock guard(_lock);
    _contracts[contract.contract_id] = contract;
    return true;
}

bool contract_verifier::remove_contract(const std::string &contract_id, std::string *error)
{
    ::dsn::service::zauto_lock guard(_lock);
    if (_contracts.erase(contract_id) == 0)
    {
        if (error != nullptr)
        {
            *error = "unknown contract: " + contract_id;
        }
        return false;
    }
    return true;
}

bool contract_verifier::find_contract(const std::string &contract_id, agent_contract *contract) const
{
    ::dsn::service::zauto_lock guard(_lock);
    const std::map<std::string, agent_contract>::const_iterator it = _contracts.find(contract_id);
    if (it == _contracts.end())
    {
        return false;
    }
    if (contract != nullptr)
    {
        *contract = it->second;
    }
    return true;
}

contract_evaluation contract_verifier::evaluate_input(const std::string &contract_id, const std::string &input) const
{
    agent_contract contract;
    if (!find_contract(contract_id, &contract))
    {
        contract_evaluation result;
        result.ok = false;
        result.contract_id = contract_id;
        result.violations.push_back("unknown contract: " + contract_id);
        return result;
    }
    return evaluate_contract(contract, &input, nullptr, nullptr);
}

contract_evaluation contract_verifier::evaluate_output(const std::string &contract_id,
                                                       const std::string &output,
                                                       const std::vector<std::string> &policy_labels) const
{
    agent_contract contract;
    if (!find_contract(contract_id, &contract))
    {
        contract_evaluation result;
        result.ok = false;
        result.contract_id = contract_id;
        result.violations.push_back("unknown contract: " + contract_id);
        return result;
    }
    return evaluate_contract(contract, nullptr, &output, &policy_labels);
}

contract_evaluation contract_verifier::evaluate(const std::string &contract_id,
                                                const std::string &input,
                                                const std::string &output,
                                                const std::vector<std::string> &policy_labels) const
{
    agent_contract contract;
    if (!find_contract(contract_id, &contract))
    {
        contract_evaluation result;
        result.ok = false;
        result.contract_id = contract_id;
        result.violations.push_back("unknown contract: " + contract_id);
        return result;
    }
    return evaluate_contract(contract, &input, &output, &policy_labels);
}

std::vector<agent_contract> contract_verifier::list_contracts() const
{
    ::dsn::service::zauto_lock guard(_lock);
    std::vector<agent_contract> contracts;
    contracts.reserve(_contracts.size());
    for (const std::map<std::string, agent_contract>::value_type &entry : _contracts)
    {
        contracts.push_back(entry.second);
    }
    return contracts;
}

std::string contract_verifier::describe() const
{
    const std::vector<agent_contract> contracts = list_contracts();
    std::ostringstream output;
    output << "contracts=" << contracts.size();
    for (const agent_contract &contract : contracts)
    {
        output << "\n" << contract.contract_id
               << " require_input=" << (contract.require_input_non_empty ? "yes" : "no")
               << " require_output=" << (contract.require_output_non_empty ? "yes" : "no");
    }
    return output.str();
}

contract_evaluation contract_verifier::evaluate_contract(const agent_contract &contract,
                                                         const std::string *input,
                                                         const std::string *output,
                                                         const std::vector<std::string> *policy_labels) const
{
    contract_evaluation result;
    result.contract_id = contract.contract_id;
    if (input != nullptr)
    {
        if (contract.require_input_non_empty && input->empty())
        {
            result.violations.push_back("input must not be empty");
        }
        std::vector<std::string> missing;
        if (!contains_all(*input, contract.required_input_fragments, &missing))
        {
            for (const std::string &fragment : missing)
            {
                result.violations.push_back("input missing required fragment: " + fragment);
            }
        }
    }
    if (output != nullptr)
    {
        if (contract.require_output_non_empty && output->empty())
        {
            result.violations.push_back("output must not be empty");
        }
        if (contract.max_output_bytes != 0 && output->size() > contract.max_output_bytes)
        {
            result.violations.push_back("output exceeds max bytes");
        }
        std::vector<std::string> missing;
        if (!contains_all(*output, contract.required_output_fragments, &missing))
        {
            for (const std::string &fragment : missing)
            {
                result.violations.push_back("output missing required fragment: " + fragment);
            }
        }
        for (const std::string &fragment : contract.forbidden_output_fragments)
        {
            if (!fragment.empty() && output->find(fragment) != std::string::npos)
            {
                result.violations.push_back("output contains forbidden fragment: " + fragment);
            }
        }
    }
    if (policy_labels != nullptr)
    {
        for (const std::string &label : contract.required_policy_labels)
        {
            if (std::find(policy_labels->begin(), policy_labels->end(), label) == policy_labels->end())
            {
                result.violations.push_back("missing policy label: " + label);
            }
        }
    }
    result.ok = result.violations.empty();
    return result;
}

bool contract_verifier::contains_all(const std::string &text,
                                     const std::vector<std::string> &fragments,
                                     std::vector<std::string> *missing) const
{
    bool ok = true;
    for (const std::string &fragment : fragments)
    {
        if (!fragment.empty() && text.find(fragment) == std::string::npos)
        {
            ok = false;
            if (missing != nullptr)
            {
                missing->push_back(fragment);
            }
        }
    }
    return ok;
}

} // namespace rasn
} // namespace dsn

