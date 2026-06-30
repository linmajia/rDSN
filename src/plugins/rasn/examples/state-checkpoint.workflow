# State/checkpoint smoke workflow: stores workflow progress through rasn.state.
task files tool list src\plugins\rasn\examples capability tool.run policy read_only budget_ms 5000 state examples/files artifact examples/workflow-examples.txt
task explain plan Explain how these rASN examples can validate direct and service modes after files capability model.complete policy read_only budget_ms 10000 state examples/explain
