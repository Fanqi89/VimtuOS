/* mp_compat.c - ★ v0.11.0：补齐 vendored musl 归档里缺的数学符号 + clang 的 half-float 内建
 *
 * 与 user/lua/lua_compat.c 完全同一份理由：third_party/musl/lib/libc.a 里
 * 没有 fabs.o/fabsf.o（上游 src/math/fabs.c 在宿主 clang 下被识别成内建，
 * 没落盘成归档成员），而 MicroPython 的 objfloat/formatfloat/modmath 都会用到。
 *
 * 必须用 -ffreestanding -fno-builtin 单独编译，否则编译器又把它当内建、不产出符号。
 */

typedef unsigned long long u64;
typedef unsigned int u32;

double fabs(double x) {
    union { double f; u64 i; } u;
    u.f = x;
    u.i &= 0x7FFFFFFFFFFFFFFFULL;
    return u.f;
}

float fabsf(float x) {
    union { float f; u32 i; } u;
    u.f = x;
    u.i &= 0x7FFFFFFFu;
    return u.f;
}

/* ---------------------------------------------------------------------------
 * 两个 clang 的 half-float 内建（compiler-rt 提供，musl 的 libc.a 里没有）：
 *   __extendhfsf2  : _Float16 -> float   （py/binary.c 的 struct 'e' 格式读）
 *   __truncsfhf2   : float -> _Float16   （同上的写）
 * 都是 IEEE-754 binary16 <-> binary32 的位级转换，含 subnormal / inf / nan 与
 * round-to-nearest-even。放在这里而不是链 compiler-rt，因为 vendored musl 只有 libc.a。
 * ------------------------------------------------------------------------- */
typedef unsigned short u16;

float __extendhfsf2(u16 h) {
    u32 sign = (u32)(h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 0x1Fu;
    u32 mant = h & 0x3FFu;
    u32 out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;                       /* +-0 */
        } else {
            int e = -1;
            do {
                e++;
                mant <<= 1;
            } while ((mant & 0x400u) == 0);
            mant &= 0x3FFu;
            out = sign | ((u32)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        out = sign | 0x7F800000u | (mant << 13);   /* inf / nan */
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    union { u32 u; float f; } v;
    v.u = out;
    return v.f;
}

u16 __truncsfhf2(float f) {
    union { float f; u32 u; } v;
    v.f = f;
    u32 u = v.u;
    u32 sign = (u >> 16) & 0x8000u;
    u32 exp = (u >> 23) & 0xFFu;
    u32 mant = u & 0x7FFFFFu;
    if (exp == 0xFFu) {
        return (u16)(sign | 0x7C00u | (mant ? 0x200u | (mant >> 13) : 0));
    }
    int e = (int)exp - 127 + 15;
    if (e >= 0x1F) {
        return (u16)(sign | 0x7C00u);          /* 溢出 -> inf */
    }
    if (e <= 0) {
        if (e < -10) {
            return (u16)sign;                  /* 太小 -> +-0 */
        }
        mant |= 0x800000u;
        u32 shift = (u32)(14 - e);
        u32 hm = mant >> shift;
        u32 rem = mant & ((1u << shift) - 1u);
        u32 half = 1u << (shift - 1);
        if (rem > half || (rem == half && (hm & 1u))) {
            hm++;
        }
        return (u16)(sign | hm);
    }
    u32 hm = mant >> 13;
    u32 rem = mant & 0x1FFFu;
    u16 hout = (u16)(sign | ((u32)e << 10) | hm);
    if (rem > 0x1000u || (rem == 0x1000u && (hm & 1u))) {
        hout++;                                /* 进位到指数也是正确结果 */
    }
    return hout;
}
