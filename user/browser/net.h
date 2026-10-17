/* net.h - 浏览器的最小 HTTP 客户端（ring3，走内核 net_raw 自有 ABI 号 53）
 *
 * 它是什么：把 `user/net/netd.c` 里那套**已经验收过**的 ARP / DNS(A) / TCP 客户端搬过来，
 * 只保留"抓一个 http:// URL 的正文"这一条路径：
 *
 *     net_http_get("http://10.0.2.2:18080/index.html", body, cap, &status, &got)
 *
 * 边界（与 netd 完全一致，别当成完整 TCP/IP）：
 *   * IPv4 不分片/不重组；TCP 不做重排序、不做拥塞控制（固定 RTO、固定窗口）、无 TIME_WAIT；
 *   * DNS 只做 A 记录、一问一答；
 *   * **只支持 http 协议**（没有 TLS，也就不假装支持 https）；
 *   * 同一时刻只有一条连接（浏览器一次只抓一页）。
 *
 * 缓冲约定：本模块**不 malloc、不持有大缓冲**；body 由调用方给（浏览器走 mmap），
 * 内部收发帧缓冲用小块静态区（构建带 -fno-zero-initialized-in-bss，所以**必须是非零初始化**
 * 或者放在 mmap 区里 —— 见 net.c 顶部说明）。
 *
 * 打点（tests/browser_http64_test.py grep；格式别改）：
 *   [BNET] url host=.. port=.. path=..
 *   [BNET] dns name=.. addr=<a.b.c.d> ms=..
 *   [BNET] tcp connect ip=..:<port> ok=.. ms=..
 *   [BNET] http get status=.. bytes=.. ms=..
 *   [BNET] FAIL reason=<parse|dns|connect|send|timeout|status-line|no-body> step=..
 */
#ifndef VIMTUOS_BROWSER_NET_H
#define VIMTUOS_BROWSER_NET_H

#include <stdint.h>

/* 诊断信息（只读；最近一次请求） */
struct BNetInfo {
    char     host[64];      /* URL 里的主机名（或点分 IP 原文） */
    uint32_t ip;            /* a<<24|b<<16|c<<8|d；0 = 没解析出来 */
    uint32_t port;
    uint32_t dns_ms;
    uint32_t tcp_ms;
    uint32_t http_ms;       /* 从发出 GET 到收完正文 */
    int      step;          /* 失败时停在哪一步（与打点的 step= 同义） */
};

/* 抓一个 http:// URL 的正文。
 *   url     : "http://host[:port][/path]"（大小写不敏感；没有 path 就当 "/"）
 *   body/cap: 输出缓冲；正文超过 cap 时**截断并如实把 got 写成 cap**（不谎报完整）
 *   status  : HTTP 状态码（200/404…）；got: 实际写进 body 的字节数
 * 返回：0 = 拿到响应（status/got 有效，status 可能是 404 等）；负数见 net.c 的错误码表。 */
int net_http_get(const char* url, char* body, int cap, int* status, int* got);

const struct BNetInfo* net_last_info(void);

#endif /* VIMTUOS_BROWSER_NET_H */
