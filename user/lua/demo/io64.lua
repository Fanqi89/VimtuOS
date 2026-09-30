-- io64.lua - ★ A4-4a：Lua 从**系统卷**里读文件（io.open / io.read）
-- 目标文件 /etc/sh64hello.txt 由 tools/make_shellvol.py 系列离线写入，内容逐字节已知：
--     VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n
-- 证据（tests/lua64_test.py 断言）：前两行 + 字节数 + 逐行读回来的第一行。

local path = "/etc/sh64hello.txt"
local f, err = io.open(path, "r")
if not f then
  print("io-open-FAIL: " .. tostring(err))
  os.exit(1)
end

local first = f:read("l")
local rest = f:read("a")
f:close()

print("io-open-ok " .. path)
print("io-line1 " .. first)
print("io-reab " .. #rest)
print("io-lines " .. (select(2, string.gsub(first .. rest, "\n", "")) + 0))
print("io64-done")
