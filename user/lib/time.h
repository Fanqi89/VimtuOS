/* time.h - 时间（Vimtu64 用户态 C 运行时）
 *
 * ★ 语义如实标注：本内核只有 **PIT tick（250Hz，启动以来计数）**，没有 RTC/epoch ——
 *   所以 time() 返回的是"启动以来的秒数"，不是 Unix 时间戳。内核也没有这之外的时钟号段
 *   （Linux 号段里的 clock_gettime(228) 内核有实现，但本轮 A2 不做 time() 之外的时间 API）。 */
#ifndef VIMTU64_TIME_H
#define VIMTU64_TIME_H

#include <sys/types.h>

#define VIMTU64_TICKS_HZ 250

/* 启动以来的秒数（整除 PIT tick / 250）。tloc 非空时同时写回。 */
time_t time(time_t* tloc);

#endif /* VIMTU64_TIME_H */
