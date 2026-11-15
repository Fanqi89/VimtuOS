/* vi_img.h - 图片解码（vendored stb_image 的对外壳）
 *
 * 分工：
 *   * `user/viewer/stb_image_impl.c` 是 **唯一** 编译 stb_image 的编译单元（含内存竞技场）；
 *   * `user/viewer/viewer.c`（窗口壳）只通过本头文件的 7 个函数用它 —— 不直接包含
 *     stb_image.h，这样窗口壳那一侧可以保持 `-mno-sse` 的严格纪律。
 *
 * 为什么要有"竞技场"：
 *   本仓库用户态的 `malloc` 只有 **4 KiB**（user/lib/stdlib.h 的 V64_HEAP_BYTES），
 *   stb_image 解一张图要几十 KB 到几 MB。所以这里用 mmap(9) 拿一块大缓冲、按 16 字节
 *   对齐做 bump 分配，并把 STBI_MALLOC/STBI_REALLOC 全部指过去（STBI_FREE 是空操作）。
 *   一次性进程（看完图就退出），不回收，也就不需要通用分配器。
 */
#ifndef VIMTU64_VI_IMG_H
#define VIMTU64_VI_IMG_H

/* 竞技场（单位字节）。上限来自内核：用户 mmap 空间 4GiB+576KiB 起 ~990 MiB，
 * 但物理内存只有 512 MB，所以这里取一个"一两张图够用"的固定值。
 * ★ 32 MiB 是**实测**出来的：第三方 stb_image 解一张 1600x1200 RGBA PNG 的峰值是
 *   20.18 MiB（滤波行数据 4 B/px + native 图 4 B/px + 转 3 通道 3 B/px ≈ 11 B/px），
 *   12 MiB 竞技场连 1.92 Mpx 都过不去（客户机只是兜底报 reason=decode，看不出原因）。
 *   宿主机复现：arena=16MiB FAIL(peak 15.4MB) / arena=32MiB OK(peak 21.2MB)。 */
#define VI_ARENA_BYTES   (32u * 1024u * 1024u)

/* 输入文件上限：竞技场要同时装"压缩字节 + 解码峰值"（峰值 ≈ 11 B/px，见上面 VI_ARENA_BYTES）。
 * 竞技场 32 MiB - 4 MiB 输入 = 28 MiB -> 28*1024*1024/11 = 2,667,000 px；取 2,500,000 px
 * （峰值 27.5 MB + 输入 4 MB = 31.5 MB <= 32 MiB，留 ~1 MB 余量）。
 * 覆盖 1600x1200（1.92 Mpx，验收图 big.png）与 1920x1080（2.07 Mpx，手机随手拍）。
 * ★ 旧值 1.6 Mpx 是凭直觉得出的，既挡掉了常见尺寸、又没算进峰值（实测连 1.92 Mpx 都解不出）。
 * 超了如实拒绝（reason=file-too-big / too-many-pixels），不截断、不糊弄。 */
#define VI_MAX_FILE      (4u * 1024u * 1024u)
#define VI_MAX_PIXELS    (2500000u)

/* 返回 1 = 竞技场就绪；0 = mmap 失败（调用方按 reason=alloc 报）。幂等。 */
int  vi_arena_init(void);
void vi_arena_destroy(void);

/* 解码一段内存里的图片（PNG / JPEG / BMP）。
 * 返回 1 = 成功：out_w / out_h = 尺寸，out_comp = 通道数（3 或 4）；
 * 失败返回 0，原因见 vi_err()（静态 ASCII，直接打点用）。 */
int  vi_decode(const unsigned char* data, int len, int* out_w, int* out_h, int* out_comp);

/* 解码出的像素（RGB 或 RGBA，按 vi_decode 的 *out_comp 解释）。只在成功后可读。 */
const unsigned char* vi_pixels(void);

/* 最后一次失败原因（静态 ASCII；成功时为空串）。 */
const char* vi_err(void);

#endif /* VIMTU64_VI_IMG_H */
