/* qstrdefsport.h - 端口私有的 qstr 定义（本端口目前**没有**，但机制必须留着）
 *
 * 为什么这个文件存在（与 upstream 的 QSTR_DEFS 接口一致）：
 *   tools/micropython_build_win.sh 的 qstr 流水线里，这个文件紧跟 py/qstrdefs.h 一起进
 *   `cat py/qstrdefs.h user/micropython/port/qstrdefsport.h <collected> | sed ... | clang -E`
 *   那一步，所以写在这里的 `Q(name)` 一定会进最终的 genhdr/qstrdefs.generated.h。
 *
 * 为什么现在一条都不写：本端口的 .c（main.c / mphalport.c / readline_port.c）**也参与
 * qstr 抽取**（脚本把它们列进了 makeqstrdefs.py 的 sources），所以端口里用到的
 * `MP_QSTR_xxx` 会被自动扫出来，不需要在这里重复声明。
 *
 * 什么情况下**必须**在这里手写 Q(...)（这是这个文件存在的唯一理由）：
 *   1) 某个 qstr 只被"字符串形式"使用，源码里没有 `MP_QSTR_xxx` 这个记号 —— 例如
 *      `mp_obj_new_str("__main__")` 后再按名字查字典、或者宏在预处理阶段把记号藏起来了，
 *      makeqstrdefs 的 `MP_QSTR_[_a-zA-Z0-9]+` 正则扫不到它；
 *   2) 端口要"预留"一个名字给冻结字节码 / mpy 文件用，而本工程不编 frozen 模块
 *      （MICROPY_MODULE_FROZEN_MPY=0），那种名字不会由任何 .c 产生；
 *   3) 端口想让某个名字**成为静态 qstr 池的第一批**（QDEF0）以便按常量比较（性能取巧）。
 * 写法就是一行一个 `Q(name)`（见 py/qstrdefs.h 的格式），`name` 里不能有引号/括号。
 */
