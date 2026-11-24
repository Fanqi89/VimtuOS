# /mpy/calc.py - VimtuOS 上的 MicroPython 小工具：命令行计算器
#
# 用法（在系统终端里）：
#   mp /mpy/calc.py         进入交互模式：一行一个表达式，回车求值，q 退出
#
# 为什么只做交互模式（**不**做 `mp /mpy/calc.py 7*6` 这种单次模式）：
#   本端口的 mpconfigport.h 把 `MICROPY_PY_SYS_ARGV` 关着（没有 sys.argv）——
#   脚本拿不到命令行参数，硬写单次模式只会让用户以为"参数被吃了"。
#   v0.11.6 实测教训：第一版写 `sys.exit(main(sys.argv))`，脚本连第一行都没跑完就
#   `AttributeError: 'module' object has no attribute 'exit'`（SYS_EXIT 当时也是关的）。
#   现在 SYS_EXIT 已打开（脚本可以用 sys.exit），argv 仍关着 —— 如实写在这里。
#
# 边界：只做 + - * / // % ** 和括号；不做变量/函数/科学计算。

print("VimtuOS 计算器 (MicroPython)")
print("输入表达式，回车求值；q 退出。例如： (3+4)*5 - 2**3")


def evaluate(src):
    """安全求值：只允许数字与 + - * / // % ( ) 空白；其它字符一律拒绝。"""
    allowed = "0123456789.+-*/%() \t"
    for ch in src:
        if ch not in allowed:
            return None, "含有不支持的字符：%r" % ch
    try:
        return eval(src), ""
    except Exception as e:              # noqa: BLE001  （MicroPython 的异常层次比 CPython 窄）
        return None, "算不了：%s" % e


def once(src):
    val, err = evaluate(src)
    if err:
        print("错误：" + err)
        return
    print("%s = %s" % (src, val))


while True:
    try:
        line = input("calc> ")
    except EOFError:
        print("")
        break
    line = line.strip()
    if not line:
        continue
    if line in ("q", "quit", "exit"):
        print("bye")
        break
    once(line)
