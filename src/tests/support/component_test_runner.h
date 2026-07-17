#pragma once

namespace dsn {
namespace tests {

using prepare_test_environment = void (*)();

int run_component_tests(int argc,
                        char **argv,
                        prepare_test_environment prepare_environment = nullptr);

} // namespace tests
} // namespace dsn
