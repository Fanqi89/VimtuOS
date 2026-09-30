-- hello.lua - ★ A4-4a：Lua 演示（算术 + 字符串 + for 循环 + 表/排序）
-- 由 tools/lua_pack_win.py 装进系统卷 /tcc/demo/hello.lua；终端里：
--     run /bin/lua /tcc/demo/hello.lua
-- 输出全部逐行可断言的确定文本（tests/lua64_test.py 会逐字节比对）。

local sum = 0
for i = 1, 10 do
  sum = sum + i
end
print("arith: 1+2*3=" .. (1 + 2 * 3) .. " sum(1..10)=" .. sum)
print("str:   " .. ("vim" .. "tu") .. " len=" .. #("vimtuos") .. " upper=" .. string.upper("lua"))

local t = { 3, 1, 2 }
table.sort(t)
print("table: " .. t[1] .. "," .. t[2] .. "," .. t[3] .. " max=" .. math.max(7, 5, 9))
print("fmt:   " .. string.format("%d/%s/%.0f", 42, "ok", 2.5))
print("hello-lua-done")
