// uefi_gop.h - UEFI GOP图形初始化
#pragma once
#include <stdint.h>
#include <efi.h>
#include <efilib.h>

// 显示模式信息
struct DisplayMode {
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_size;
    int is_linear;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
    uint8_t red_position;
    uint8_t green_position;
    uint8_t blue_position;
    uint8_t reserved_position;
};

// UEFI GOP初始化结果
struct GOPInitResult {
    int success;
    struct DisplayMode mode;
    char error_msg[256];
    EFI_HANDLE handle;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop;
};

// 预设的显示模式
struct DisplayModePreference {
    uint32_t min_width;
    uint32_t min_height;
    uint32_t preferred_width;
    uint32_t preferred_height;
    uint32_t max_bpp;
};

// 默认偏好设置
#define DEFAULT_MODE_PREFERENCE { \
    .min_width = 1024, \
    .min_height = 768, \
    .preferred_width = 1920, \
    .preferred_height = 1080, \
    .max_bpp = 32 \
}

// GOP初始化函数
struct GOPInitResult efi_gop_init(EFI_HANDLE image_handle, 
                                 EFI_SYSTEM_TABLE* system_table,
                                 const struct DisplayModePreference* pref);

// 显示模式查询
int efi_gop_query_modes(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                       struct DisplayMode* modes,
                       uint32_t* mode_count);

// 显示模式选择
int efi_gop_select_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                       struct DisplayMode* selected_mode,
                       const struct DisplayModePreference* pref);

// 帧缓冲区操作
void efi_gop_clear_screen(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, uint32_t color);
void efi_gop_draw_pixel(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, 
                       uint32_t x, uint32_t y, uint32_t color);
uint32_t efi_gop_get_pixel(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, 
                          uint32_t x, uint32_t y);
void efi_gop_draw_rect(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                      uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                      uint32_t color);
void efi_gop_draw_text(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                      uint32_t x, uint32_t y, const char* text,
                      uint32_t fg_color, uint32_t bg_color);

// 颜色转换函数
uint32_t efi_gop_rgb_to_pixel(uint32_t r, uint32_t g, uint32_t b,
                              const struct DisplayMode* mode);
void efi_gop_pixel_to_rgb(uint32_t pixel, const struct DisplayMode* mode,
                         uint32_t* r, uint32_t* g, uint32_t* b);

// 显示模式枚举
enum DisplayModeFilter {
    MODE_FILTER_ALL = 0,
    MODE_FILTER_LINEAR = 1,
    MODE_FILTER_BEST_RESOLUTION = 2,
    MODE_FILTER_MINIMAL = 3
};

// 模式比较函数
int efi_gop_compare_modes(const struct DisplayMode* a, const struct DisplayMode* b);
int efi_gop_mode_matches_preference(const struct DisplayMode* mode,
                                   const struct DisplayModePreference* pref,
                                   enum DisplayModeFilter filter);

// EDID信息处理
struct EDIDInfo {
    uint8_t manufacturer_id[3];
    uint16_t product_code;
    uint32_t serial_number;
    uint8_t week_of_manufacture;
    uint8_t year_of_manufacture;
    uint8_t version;
    uint8_t revision;
    uint32_t max_horizontal_size;
    uint32_t max_vertical_size;
    uint32_t gamma;
    uint8_t features;
    uint8_t color_characteristics[10];
    uint8_t established_timings[3];
    uint8_t standard_timings[8];
    uint8_t detailed_timings[6][18];
    uint8_t extensions[128];
};

int efi_gop_read_edid(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, struct EDIDInfo* edid);
const char* efi_gop_get_monitor_name(const struct EDIDInfo* edid);

// 刷新率支持
struct RefreshRate {
    uint32_t horizontal;
    uint32_t vertical;
};

int efi_gop_get_refresh_rates(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                             uint32_t mode_index,
                             struct RefreshRate* rates,
                             uint32_t* rate_count);

// 色彩空间支持
enum ColorSpace {
    COLOR_SPACE_SRGB = 0,
    COLOR_SPACE_ADOBE_RGB = 1,
    COLOR_SPACE_DCIP3 = 2,
    COLOR_SPACE_REC_709 = 3,
    COLOR_SPACE_REC_2020 = 4
};

int efi_gop_get_color_space(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, enum ColorSpace* space);

// 缩放和适配
struct DisplayBounds {
    uint32_t source_x;
    uint32_t source_y;
    uint32_t source_width;
    uint32_t source_height;
    uint32_t dest_x;
    uint32_t dest_y;
    uint32_t dest_width;
    uint32_t dest_height;
};

void efi_gop_calculate_fit(const struct DisplayMode* source,
                          const struct DisplayMode* target,
                          struct DisplayBounds* bounds,
                          int maintain_aspect_ratio);

// 双缓冲支持
struct DoubleBuffer {
    void* front_buffer;
    void* back_buffer;
    uint32_t buffer_size;
    int flip_pending;
};

int efi_gop_create_double_buffer(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                                struct DisplayMode* mode,
                                struct DoubleBuffer* buffer);
void efi_gop_destroy_double_buffer(struct DoubleBuffer* buffer);
void efi_gop_flip_buffers(struct DoubleBuffer* buffer);

// 性能监控
struct GOPPerformance {
    uint32_t frame_count;
    uint64_t total_render_time;
    uint64_t max_render_time;
    uint64_t min_render_time;
    uint32_t flip_count;
    uint64_t total_flip_time;
};

void efi_gop_performance_start(struct GOPPerformance* perf);
void efi_gop_performance_end(struct GOPPerformance* perf);
void efi_gop_performance_flip(struct GOPPerformance* perf);
void efi_gop_performance_stats(struct GOPPerformance* perf);

// 调试支持
void efi_gop_debug_info(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);
void efi_gop_dump_mode_info(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, uint32_t mode_index);
void efi_gop_test_pattern(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop);

// 向后兼容性 - 转换为VimtuOS BootInfo格式
int efi_gop_to_boot_info(const struct DisplayMode* gop_mode,
                        struct BootInfo* boot_info);

// 错误处理
#define EFI_GOP_ERROR_NONE 0
#define EFI_GOP_ERROR_INIT_FAILED 1
#define EFI_GOP_ERROR_NO_MODE 2
#define EFI_GOP_ERROR_INVALID_MODE 3
#define EFI_GOP_ERROR_MEMORY_FAILED 4
#define EFI_GOP_ERROR_PROTOCOL_FAILED 5

const char* efi_gop_error_string(int error_code);

// 常用颜色定义
#define EFI_GOP_COLOR_BLACK    0x00000000
#define EFI_GOP_COLOR_WHITE    0xFFFFFFFF
#define EFI_GOP_COLOR_RED      0x000000FF
#define EFI_GOP_COLOR_GREEN    0x0000FF00
#define EFI_GOP_COLOR_BLUE     0x00FF0000
#define EFI_GOP_COLOR_YELLOW   0x0000FFFF
#define EFI_GOP_COLOR_CYAN     0x00FFFFFF
#define EFI_GOP_COLOR_MAGENTA  0xFF00FFFF

// 常用字体设置
#define EFI_GOP_FONT_DEFAULT_WIDTH 8
#define EFI_GOP_FONT_DEFAULT_HEIGHT 16
#define EFI_GOP_FONT_DEFAULT_CHAR_SIZE (EFI_GOP_FONT_DEFAULT_WIDTH * EFI_GOP_FONT_DEFAULT_HEIGHT)

// 字体数据结构
struct FontGlyph {
    uint8_t width;
    uint8_t height;
    uint8_t data[];
};

struct Font {
    uint8_t char_width;
    uint8_t char_height;
    uint16_t first_char;
    uint16_t char_count;
    const struct FontGlyph* glyphs;
};

// 默认字体支持
extern const struct Font efi_gop_font_8x8;
extern const struct Font efi_gop_font_8x16;
extern const struct Font efi_gop_font_16x16;

void efi_gop_draw_char(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                      uint32_t x, uint32_t y, char c,
                      const struct Font* font,
                      uint32_t fg_color, uint32_t bg_color);
void efi_gop_draw_string(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop,
                        uint32_t x, uint32_t y, const char* str,
                        const struct Font* font,
                        uint32_t fg_color, uint32_t bg_color);
uint32_t efi_gop_measure_string(const char* str, const struct Font* font);