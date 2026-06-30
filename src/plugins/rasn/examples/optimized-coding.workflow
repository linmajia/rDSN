# Optimizer-aware rASN coding workflow.
# The compiler uses latency/cost/reliability hints to order ready nodes while
# preserving dependencies and to explain the critical path in the compiled plan.

task inspect tool "list src\plugins\rasn" capability tool.run policy read_only budget_ms 5000 latency_ms 50 cost_hint 1 reliability 99 state examples/inspect artifact examples/rasn-files.txt
task design plan "Design the smallest robust change after inspecting rASN files" after inspect capability model.complete policy read_only budget_ms 10000 latency_ms 400 cost_hint 3 reliability 94 state examples/design
task risk ask "Identify likely failure modes and validation steps" after inspect capability model.complete policy read_only budget_ms 10000 latency_ms 350 cost_hint 2 reliability 92 state examples/risk
task summarize ask "Merge the design and risk findings into a concise implementation note" after design,risk capability model.complete policy read_only budget_ms 10000 latency_ms 300 cost_hint 2 reliability 95 state examples/summary
