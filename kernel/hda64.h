// hda64.h - Intel HDA（High Definition Audio）声卡驱动对外接口（最小可用、全程轮询）
//
// 定位与边界（先读，避免误期待；实现细节见 hda64.cpp 顶部）：
//   * 单控制器 / 单码器 / 单播放流（SD0）/ 单一格式 **48 kHz / 16-bit / 2ch**；
//     全程轮询（INTCTL 全屏蔽，不开中断）——与 e1000/nvme/ahci 同款做法，
//     不需要在 ISR 上下文里加锁。
//   * 通路：AFG ->（可选中转 Mixer）-> DAC（输出转换器）-> Pin Complex，
//     最多 3 跳，用 Get Connection List 真的把连接表走一遍；走不通就如实报错。
//   * 音量/静音 = DAC（必要时连 Pin）的输出放大器 Set Amp Gain/Mute；每次写完都
//     用 Get Amp Gain 回读，回读值进打点（验收要对比），绝不"假装生效"。
//   * 不做：24-bit/192k、多流、插孔检测（unsolicited）、HDMI/ELD、SPDIF 编码、
//     中断驱动、挂起/恢复。这些路径一律如实打点并返回错误。
//
// 对外 API 的错误口径：返回 0 / true = 成功；负值 / false = 失败（已打点，含阶段名）。
// 没有控制器时全部 API 都可用但会返回"不可用"，系统照常启动（优雅降级）。
#pragma once
#include <stdint.h>
#include <stddef.h>

#define HDA64_MAX_OUT      4      // 最多登记几个输出源（Pin）
#define HDA64_PERIOD_BYTES 4096   // BDL 每条缓冲 = 4 KiB（= 1024 帧 16-bit 立体声）
#define HDA64_PERIODS      2      // BDL 条数（至少 2 个周期，见任务口径）

// 控制器/码器/通路摘要（终端 `audio info`、设置页与诊断用；未初始化时 found=0）
struct Hda64Info {
    int      found;                 // 1 = PCI 找到 class 0x0403 控制器
    int      ready;                 // 1 = 复位 + 码器 + DAC/Pin 通路 + 流都就绪
    int      codecs;                // STATESTS 位图里的码器个数
    int      codec;                 // 使用的码器地址（0..14）
    uint8_t  bus, dev, fn;          // PCI 位置
    uint64_t bar0;                  // BAR0（64 位 MMIO 基址）
    uint16_t statests;              // 复位后读到的 STATESTS
    uint32_t vid_did;               // 码器 F0000 原样（vendor<<16 | device）
    uint32_t vendor, device, step;  // 拆出来的字段
    uint32_t afg;                   // AFG 节点号
    uint32_t dac, pin;              // 活动通路的 DAC / Pin 节点号
    uint32_t dac_caps;              // DAC 的 Audio Widget Capabilities
    uint32_t amp_cap;               // DAC 输出放大器能力（steps/offset/静音位）
    uint32_t fmt_set, fmt_get;      // Set Converter Format 写入值 / 回读（0x11 = 48k/16/2）
    uint32_t pin_ctl;               // Pin Widget Control 回读（bit6 = OUT_EN）
    uint32_t pin_cfg;               // CONFIG DEFAULT（判断设备类型的依据）
    int      amp_steps;             // 放大器**步数**（能力字 bits[22:16]；0 = 只支持静音位）
    int      amp_step_qdb;          // 每步衰减（0.25dB 单位；能力字 bits[14:8]）
    int      amp_offset;            // 0dB 对应的增益索引（能力字 bits[6:0]；输出放大器多为 0）
    int      amp_mute_cap;          // 1 = 支持静音位（能力字 bit31）
    int      amp_gain_max;          // ★ 修复（①）：0 dB 对应的增益索引（= 100% 写入值；见 hda64.cpp 标定）
    int      volume;                // 当前音量 0..100（最后一次写入值）
    int      muted;                 // 1 = 静音位已置
    uint32_t amp_rb;                // 最后一次 Get Amp Gain 回读（bit7 = mute）
    int      outs;                  // 检测到的输出源个数（0 个时下面 API 会如实返回 1 项说明）
    int      sel;                   // 当前选中的输出源下标
    char     out_name[HDA64_MAX_OUT][16];   // 输出源名字（按 CONFIG DEFAULT 归类）
    uint8_t  out_digital[HDA64_MAX_OUT];    // 1 = 数字/HDMI（检测到但本批不播放）
    uint64_t cmds, cmd_timeouts;    // CORB 命令数 / 超时数
    uint64_t runs, bytes, bcis;     // 流启动次数 / 累计写入字节 / 周期完成计数
    uint32_t lpib_max;              // 最近一次流的 LPIB 峰值（字节）
    uint32_t cbl_last;              // 最近一次流的 CBL
    uint32_t conv_get;              // Get Converter Stream/Channel 回读（bit7:4 = 流号，应 = 0x10）
    int      pin_amp_steps;         // Pin 自身输出放大器步数（bits[22:16]；0 = 只写静音位/不动增益）
    int      pin_amp_step_qdb;      // Pin 放大器每步衰减（0.25dB；bits[14:8]）
    int      pin_amp_offset;        // Pin 放大器 0dB 索引（bits[6:0]）
    int      pin_amp_gain_max;      // ★ 修复（①）：Pin 放大器 0dB 增益索引（0 = 不写 Pin 增益）
    uint32_t pin_caps;              // Pin Capabilities（bit4 输出 / bit16 EAPD / bit7 HDMI）
    uint32_t eapd;                  // 写进 Set EAPD 的值（0 = 没写/引脚不支持）
    int      selftest;              // -1 = 跳过/没跑；0 = PASS；>0 = FAIL 位掩码
    const char* last_err;           // 最近失败阶段名（"none" = 没失败过）
};

// 初始化（幂等）：PCI 扫描 -> BAR0 -> GCTL.CRST 复位 -> STATESTS -> CORB/RIRB ->
//   AFG->DAC->Pin 通路 -> 格式 48k/16/2 -> BDL（2×4KiB，来自页池）-> 按持久化音量落硬件。
// 没控制器只打一行 "[HDA64] not found"（优雅降级）；所有等待都有界，绝不挂死。
void hda64_init64();

// 1 = 可用（能真的播放/改音量）；0 = 不可用（没控制器 / 通路没建立）
int  hda64_ready64();
const Hda64Info* hda64_info64();          // 永远非空（未初始化时 found=0）

// ---- PCM 播放（16-bit 立体声交织；frames = 每声道采样数 = 字节数/4）----
// 同步播放：内部按 <= 2 个周期（8 KiB）分块，每块 BDL -> RUN -> 轮询完成 -> 停。
// 返回 0 = 全部送完（控制器侧确认；LPIB/BCIS 计数进打点）；-1 = 驱动不可用/参数错；
//        -2 = 流超时（如实失败，不谎报成功）。
int  hda64_play64(const int16_t* pcm, size_t frames);
// 异步：把 PCM 追加进软件环（非阻塞）。返回接受的**帧数**（0 = 环满 / 驱动不可用；-1 = 参数错）。
int  hda64_queue64(const int16_t* pcm, size_t frames);
// 把软件环推进硬件（最多 max_periods 个 8 KiB 块；有界）。返回推进的块数，负值 = 失败。
int  hda64_pump64(int max_chunks);
int  hda64_queued64();                    // 环里还有多少帧
// 合成方波（440 Hz 近似）播放 ms 毫秒（10..2000，超过按上限截断）。终端 `audio playtone` 用它。
int  hda64_tone64(int ms);

// ---- 音量 / 静音 / 输出源 ----
int  hda64_set_volume64(int percent);     // 0..100（越界夹取）；0 = 静音位；返回实际写入的百分比
int  hda64_get_volume64();                // 0..100（缓存值，来自写入；回读在 amp_rb / 打点里）
int  hda64_mute64(int on);                // 1 = 静音，0 = 取消；返回 0 / 负值
int  hda64_get_mute64();
int  hda64_outputs64();                   // 检测到的输出源个数（0 个时返回 1，名字里如实说明）
const char* hda64_output_name64(int i);   // 名字（越界返回 "?"）
int  hda64_get_output64();                // 当前选中的输出源下标
int  hda64_select_output64(int i);        // 切换输出源（真写 Pin Widget Control + 通路；失败返回负值）

// 未实现/不支持的路径：如实打点并返回负值（调用方不必猜）。
int  hda64_set_format64(uint32_t fmt);    // 只支持 0x11（48k/16/2）；别的值如实拒绝

// 启动期自检：PCI/码器/通路/格式/音量回读 + **送一段已知 PCM 并断言 LPIB/BCIS 前进**。
// 返回 true = PASS 或 skipped（没控制器）；false = FAIL（已打 [HDA64] selftest FAIL mask=..）。
bool hda64_selftest64();

// ---- ★ 系统音效（4 段内置素材）：从系统卷 /usr/share/sounds/<name>.wav 读，走同一条播放流 ----
// name ∈ "startup" | "notify" | "click" | "error"（与 /bin/sounder 的素材同名同源）。
// 返回 0 = 播完；-1 = 驱动不可用/素材缺失/非法；-2 = 流超时。why = 触发点（打点用）。
// 说明：外壳仍在 Ring 3 之上（内核里）时由内核 GUI 直接调用；外壳搬进 Ring 3 之后，
//   应改由用户态会话/通知管理器调用 /bin/sounder（这套音效是用户态资产）。
int  hda64_play_named64(const char* name, const char* why);
