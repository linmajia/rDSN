#include "component_test_runner.h"

void command_manager_module_init();

namespace {

void prepare_core_tests()
{
    command_manager_module_init();
}

} // namespace

int main(int argc, char **argv)
{
    return dsn::tests::run_component_tests(argc, argv, prepare_core_tests);
}
