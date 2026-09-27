/* config.h - ★ A4-2b（VimtuOS）：TinyCC 的**目标平台配置**（手写，不再由 configure 生成）
 *
 * 上游的 configure 生成这个文件（里面是 CONFIG_TCCDIR / TCC_VERSION）。VimtuOS 的构建
 * **不走 configure**（理由见 third_party/tcc/README.vimtu64-a4b.md：configure 只认 host/
 * 已知 targetos，且它给 libtcc1.a 定的规则要求"有一个能跑的 tcc"，而给 VimtuOS 编出来的
 * 4GiB 静态 ELF 在 Windows 上跑不了），所以这一份是手写的、**目标平台就是 VimtuOS**。
 *
 * {B} = tcc_lib_path：默认 = CONFIG_TCCDIR，可用命令行 `-B <dir>` 覆盖（tcc 会在
 * libtcc.c 的 tcc_split_path 里把 {B} 展开成 tcc_lib_path）。四个路径的归属：
 *   CONFIG_TCCDIR            {B}           -> libtcc1.a 就在 {B}/libtcc1.a（tccelf.c 的 tcc_add_support）
 *   CONFIG_TCC_SYSINCLUDEPATHS {B}/include -> 系统头（本平台放 musl 头，见 tcc_pack_win.py）
 *   CONFIG_TCC_LIBPATHS      {B}/lib       -> libc.a（tcc_add_runtime -> tcc_add_library("c")）
 *   CONFIG_TCC_CRTPREFIX     {B}/lib       -> crt1.o / crti.o / crtn.o（tcc_add_crt）
 *
 * 这四个目录**都在系统卷（VimtuFS2）里**，不在内核镜像里 —— VimtuOS 的内核区只剩 ~187 KB，
 * tcc 与它的头/库一个字节都不许进去。
 */
#ifndef CONFIG_TCCDIR
# define CONFIG_TCCDIR "/tcc"
#endif
#ifndef CONFIG_TCC_SYSINCLUDEPATHS
# define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"
#endif
#ifndef CONFIG_TCC_LIBPATHS
# define CONFIG_TCC_LIBPATHS "{B}/lib"
#endif
#ifndef CONFIG_TCC_CRTPREFIX
# define CONFIG_TCC_CRTPREFIX "{B}/lib"
#endif
#ifndef TCC_VERSION
# define TCC_VERSION "0.9.27"
#endif
