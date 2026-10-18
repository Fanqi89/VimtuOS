/* calc.h - 办公套件里的表格公式引擎（纯用户态，零依赖，整数运算）
 *
 * 为什么是整数而不是 double：本内核/用户态一律 `-mno-sse`（不保存 FP 上下文），
 * 表格里的金额/计数/百分比用**定点整数**（以 1 为单位，除法向下取整）足够，
 * 也避免把浮点格式化拖进来。需要小数的场景下一批再加"千分位/两位小数"的显示层。
 *
 * 支持的语法（就是这四类，别期待 Excel）：
 *   =1+2*3              四则 + 括号（* / 优先于 + -，同优先级左结合）
 *   =A1+B2*2            单元格引用（列 A..H、行 1..20，大小写不敏感）
 *   =SUM(A1:A3)         区域求和（只支持 SUM，矩形区域）
 *   =-(A1+A2)           一元负号
 * 边界（如实）：没有函数（除 SUM）、没有字符串/日期、没有绝对引用 `$A$1`、
 *   没有跨表引用；除法是**整数除法**（7/2=3），除零返回 CALC_EDIV0。
 *
 * 依赖/环检测由调用方驱动：calc_eval() 递归求单元格时通过 `get` 回调回来，
 * 调用方在回调用里用 `depth`/`visiting` 自己判环（见 office.c 的 sheet_value()）。
 */
#ifndef VIMTUOS_OFFICE_CALC_H
#define VIMTUOS_OFFICE_CALC_H

#include <stdint.h>

/* 网格尺寸（office.c 的单元格存储按这个来；改这里要同步它） */
#define CALC_COLS 8          /* A..H */
#define CALC_ROWS 20         /* 1..20 */

/* 错误码（负数） */
#define CALC_OK       0
#define CALC_ESYNTAX  (-1)   /* 语法错 / 表达式为空 / 不是 '=' 开头 */
#define CALC_ECYCLE   (-2)   /* 循环引用（由调用方在回调里返回） */
#define CALC_EDIV0    (-3)   /* 除零 */
#define CALC_EREF     (-4)   /* 引用非法（列/行越界、名字不认识） */
#define CALC_EOVER    (-5)   /* 溢出/太深（嵌套超过 16 层或数值溢出） */

/* 单元格取值回调：row/col 都是 0 基。返回 CALC_OK 时写 *out；
 * 返回 CALC_ECYCLE / CALC_EREF 时 calc_eval 原样上报（不吞错）。
 * user 是调用方自己的上下文（office.c 用它传"正在求值"标记）。 */
typedef int (*calc_cell_fn)(int row, int col, void* user, int64_t* out);

/* 求值（expr 必须以 '=' 开头；调用方也可以先去掉 '=' 再传，两者都接受）。
 *   返回 CALC_OK 时写 *out；否则返回上面的错误码（负数）。 */
int calc_eval(const char* expr, calc_cell_fn get, void* user, int64_t* out);

/* 把一个"单元格引用"文本解析成 0 基 row/col（"A1" -> row=0,col=0）；
 * 成功返回 1，失败返回 0（越界/不是引用）。给 office.c 判"这格是公式还是数字"用。 */
int calc_parse_ref(const char* s, int len, int* row, int* col);

/* 把 0 基 row/col 写成 "A1" 形式（cap >= 8）。返回写入长度。 */
int calc_ref_name(int row, int col, char* out, int cap);

#endif /* VIMTUOS_OFFICE_CALC_H */
