-- err.lua - ★ A4-4a：**故意出错**的脚本（Lua 要给出 文件名:行号 + 非 0 退出码，而不是 PANIC）
-- 断言（tests/lua64_test.py）：
--   * 串口上有 "lua: /tcc/demo/err.lua:2:" 前缀（行号 2 = 下面那行）
--   * 串口上有 "/tcc/demo/err.lua:6:" 前缀（行号 6 = 下面那行；Lua 的行号从文件头数）
local t = nil
t.x = 1                 -- 第 2 行：attempt to index a nil value (local 't')
print("unreachable")
