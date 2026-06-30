# Generic rASN workflow: read-only tool node feeds a model node.
task inspect tool list src\plugins\rasn capability tool.run policy read_only budget_ms 5000 state examples/inspect artifact examples/rasn-listing.txt
task summarize ask Summarize the rASN service roles from the inspect result after inspect capability model.complete policy read_only budget_ms 10000 state examples/summary
