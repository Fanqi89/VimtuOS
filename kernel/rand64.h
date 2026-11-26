// kernel/rand64.h - ★ v0.12.3：**密码学级**随机数（CSPRNG）
//
// 为什么必须单独一件东西（这是 TLS 的前置硬条件）：
//   v0.9.x 起内核里的 `getrandom(318)` 只是"xorshift + TSC 混一混"，文件头自己都写着
//   **非密码学安全**。这在 TLS 里等于没加密：会话密钥、client random、ECDHE 私钥全从它来，
//   一旦可预测，握手就是明文。所以先把熵源与 DRBG 做对，再谈 mbedTLS。
//
// 熵源（按可用性降级，全部打点）：
//   ① RDRAND（CPUID.01h:ECX bit30）—— 硬件数字随机数发生器，直接当输出用
//   ② RDSEED（CPUID.07h:EBX bit18）—— 只用于**播种**（真熵）
//   ③ 无硬件源时：TSC 抖动采样（连续 rdtsc 差值的最低有效位，受中断/预取/缓存扰动）+
//      PIT tick + 中断计数，喂进 SHA-256 DRBG 播种
//
// 输出：SHA-256 计数器 DRBG（NIST SP800-90A 的 Hash_DRBG 简化形态）：
//   block_i = SHA256(state || ctr_be64)；每次出块后用 block_i 重新 rekey（state = SHA256(state||block_i))
//   这样即使某次 state 泄漏，也拿不到之前的输出（后向安全），且不会出现短周期。
#pragma once
#include <stdint.h>

void     rand64_init64(void);                     // 幂等：CPUID 探测 + 播种（启动期调用一次）
void     rand64_bytes64(void* out, uint32_t n);   // 密码学随机字节（永不返回全 0；内部自愈）
uint32_t rand64_u32_64(void);
uint64_t rand64_u64_64(void);

int      rand64_rdrand64(void);                   // 1 = CPU 支持 RDRAND
int      rand64_rdseed64(void);                   // 1 = CPU 支持 RDSEED
int      rand64_seed_bytes64(void);               // 本次播种用了多少字节熵

// 自检：非全 0 / 字节分布均衡 / 16 字节块不重复 / 两次调用不同。串口打 [RAND64] selftest …
int      rand64_selftest64(void);

// 供其它内核模块（口令加盐等）直接拿一个好随机数；不要再用 sha256_64.h 里那个 xorshift。
uint32_t rand64_salt32_64(void);
