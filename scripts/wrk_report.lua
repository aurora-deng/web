-- Compatibility entrypoint; implementation: scripts/reports/wrk_report.lua
local script_path = debug.getinfo(1, "S").source:sub(2)
local script_dir = script_path:match("^(.*[/\\])") or "./"
dofile(script_dir .. "reports/wrk_report.lua")
