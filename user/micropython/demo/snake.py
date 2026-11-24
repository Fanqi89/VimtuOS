# /mpy/snake.py - VimtuOS 上的 MicroPython 小游戏：终端贪吃蛇
#
# 用法（在系统终端里）：
#   mp /mpy/snake.py
#   然后每回合输入一个方向：w 上 / a 左 / s 下 / d 右，q 退出（回车确认）
#
# 为什么是"逐回合"而不是实时：
#   内核终端的按键是**行缓冲**的（shell 自己管行编辑、Backspace、历史），ring3 程序拿到的是
#   整行输入 —— 所以这里用"一回合一步"的回合制规则（与终端模型一致，不假装有 termios/select）。
#   要做到实时，得先给终端加原始模式（下一轮的事，如实写在这里）。
#
# 规则：吃 @ 加分 +1 并变长；撞墙/撞自己 = 结束（保留最高分）。

W, H = 20, 10
APPLE = "@"
BODY = "o"
HEAD = "O"

# ★ v0.11.7 修：方向表是 (dy, dx) —— 之前写成 (0,-1)/(-1,0)… 相当于整体转了 90°，
#   按 w 实际是往左走，第一步就撞到自己（实测"咬到自己了。最终分数 0（1 回合）"）。
#   w 上 = dy -1；a 左 = dx -1；s 下 = dy +1；d 右 = dx +1。
DIRS = {"w": (-1, 0), "a": (0, -1), "s": (1, 0), "d": (0, 1)}


def draw(snake, apple, score, round_no):
    """把棋盘一行行打出来（终端是等宽 + GNU 半角，所以用 ASCII 边框）。"""
    grid = [[" " for _ in range(W)] for _ in range(H)]
    ay, ax = apple
    grid[ay][ax] = APPLE
    for i, (y, x) in enumerate(snake):
        grid[y][x] = HEAD if i == 0 else BODY
    print("+" + "-" * W + "+   分数 %d  回合 %d" % (score, round_no))
    for row in grid:
        print("|" + "".join(row) + "|")
    print("+" + "-" * W + "+   w/a/s/d 移动，q 退出")


def new_apple(snake):
    """固定顺序找第一个空格：MicroPython 的 random 依赖 urandom，这里不赌它。"""
    for y in range(H):
        for x in range(W):
            if (y, x) not in snake:
                return (y, x)
    return None


def main():
    snake = [(H // 2, 3), (H // 2, 2), (H // 2, 1)]
    apple = new_apple(snake)
    score = 0
    round_no = 0
    print("VimtuOS 贪吃蛇 (MicroPython) —— 吃 %s 变长，撞墙/撞自己结束" % APPLE)
    draw(snake, apple, score, round_no)
    while True:
        try:
            key = input("方向> ").strip()[:1]
        except EOFError:
            print("")
            return 0
        if key in ("q", "quit"):
            print("bye（分数 %d）" % score)
            return 0
        if key not in DIRS:
            print("只认 w/a/s/d（或 q 退出）")
            continue
        round_no += 1
        dy, dx = DIRS[key]
        hy, hx = snake[0]
        ny, nx = hy + dy, hx + dx
        if ny < 0 or ny >= H or nx < 0 or nx >= W:
            print("撞墙了。最终分数 %d（%d 回合）" % (score, round_no))
            return 0
        if (ny, nx) in snake[:-1]:
            print("咬到自己了。最终分数 %d（%d 回合）" % (score, round_no))
            return 0
        snake.insert(0, (ny, nx))
        if (ny, nx) == apple:
            score += 1
            apple = new_apple(snake)
            print("吃到 @ ！分数 %d" % score)
            if apple is None:
                print("满屏了，你赢了！")
                return 0
        else:
            snake.pop()
        draw(snake, apple, score, round_no)


main()
