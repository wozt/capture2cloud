#!/usr/bin/env python3
from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch-ffmpeg-old3ds.py <ffmpeg-source-dir>")

root = Path(sys.argv[1])

asm = root / "libavcodec/arm/hpeldsp_armv6.S"
init = root / "libavcodec/arm/hpeldsp_init_armv6.c"

s = asm.read_text()

anchor = """call_2x_pixels          put, _y2_no_rnd

function ff_put_pixels16_armv6, export=1
"""

replacement = r"""call_2x_pixels          put, _y2_no_rnd
call_2x_pixels          put, _xy2
call_2x_pixels          put, _xy2_no_rnd

/*
 * ARMv6 diagonal half-pixel interpolation for MPEG-1/2/4.
 *
 * For each output pixel:
 *
 *   rounded: (a + b + c + d + 2) >> 2
 *   no-rnd:  (a + b + c + d + 1) >> 2
 *
 * Split each byte into its upper six bits and lower two bits so UADD8
 * can sum four pixels in parallel without cross-byte carry.  This is
 * mathematically identical to the generic ARM implementation.
 */
.macro old3ds_xy2_four offset
        ldr             r4,  [r1, #\offset]
        ldr             r5,  [r1, #(\offset + 4)]
        ldr             r6,  [r14, #\offset]
        ldr             r7,  [r14, #(\offset + 4)]

        lsr             r10, r4, #8
        orr             r10, r10, r5, lsl #24
        lsr             r11, r6, #8
        orr             r11, r11, r7, lsl #24

        /* Sum the low two bits of all four source pixels. */
        and             r12, r4,  r8
        and             r5,  r10, r8
        uadd8           r12, r12, r5
        and             r5,  r6,  r8
        uadd8           r12, r12, r5
        and             r5,  r11, r8
        uadd8           r12, r12, r5
        uadd8           r12, r12, r9

        /* Sum the upper six bits; maximum is 4*63 = 252. */
        bic             r4,  r4,  r8
        bic             r10, r10, r8
        bic             r6,  r6,  r8
        bic             r11, r11, r8

        lsr             r4,  r4,  #2
        lsr             r10, r10, #2
        lsr             r6,  r6,  #2
        lsr             r11, r11, #2

        uadd8           r4, r4, r10
        uadd8           r6, r6, r11
        uadd8           r4, r4, r6

        /* Convert the low-bit sum back to a per-byte contribution. */
        lsr             r12, r12, #2
        and             r12, r12, r8
        uadd8           r4, r4, r12

        str             r4, [r0, #\offset]
.endm

.macro old3ds_xy2_func name, bias
function ff_\name\()_armv6, export=1
        push            {r4-r11, lr}

        mov             r8, #3
        orr             r8, r8, r8, lsl #8
        orr             r8, r8, r8, lsl #16

        mov             r9, #\bias
        orr             r9, r9, r9, lsl #8
        orr             r9, r9, r9, lsl #16

1:
        add             r14, r1, r2

        old3ds_xy2_four 0
        old3ds_xy2_four 4

        add             r1, r1, r2
        add             r0, r0, r2
        subs            r3, r3, #1
        bne             1b

        pop             {r4-r11, pc}
endfunc
.endm

old3ds_xy2_func put_pixels8_xy2,        2
old3ds_xy2_func put_pixels8_xy2_no_rnd, 1

function ff_put_pixels16_armv6, export=1
"""

if "old3ds_xy2_func put_pixels8_xy2" not in s:
    if anchor not in s:
        raise SystemExit("hpeldsp_armv6.S anchor not found")
    s = s.replace(anchor, replacement, 1)
    asm.write_text(s)

s = init.read_text()

old = """void ff_put_pixels16_y2_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_put_pixels16_x2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);
"""

new = """void ff_put_pixels16_y2_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);
void ff_put_pixels16_xy2_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_put_pixels16_x2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);
"""

if old in s:
    s = s.replace(old, new, 1)

old = """void ff_put_pixels16_y2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_avg_pixels16_armv6"""

new = """void ff_put_pixels16_y2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);
void ff_put_pixels16_xy2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_avg_pixels16_armv6"""

if old in s:
    s = s.replace(old, new, 1)

old = """void ff_put_pixels8_y2_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_put_pixels8_x2_no_rnd_armv6"""

new = """void ff_put_pixels8_y2_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);
void ff_put_pixels8_xy2_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_put_pixels8_x2_no_rnd_armv6"""

if old in s:
    s = s.replace(old, new, 1)

old = """void ff_put_pixels8_y2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_avg_pixels8_armv6"""

new = """void ff_put_pixels8_y2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);
void ff_put_pixels8_xy2_no_rnd_armv6(uint8_t *, const uint8_t *, ptrdiff_t, int);

void ff_avg_pixels8_armv6"""

if old in s:
    s = s.replace(old, new, 1)

s = s.replace(
    "/*     c->put_pixels_tab[0][3] = ff_put_pixels16_xy2_armv6; */",
    "    c->put_pixels_tab[0][3] = ff_put_pixels16_xy2_armv6;"
)
s = s.replace(
    "/*     c->put_pixels_tab[1][3] = ff_put_pixels8_xy2_armv6; */",
    "    c->put_pixels_tab[1][3] = ff_put_pixels8_xy2_armv6;"
)
s = s.replace(
    "/*     c->put_no_rnd_pixels_tab[0][3] = ff_put_pixels16_xy2_no_rnd_armv6; */",
    "    c->put_no_rnd_pixels_tab[0][3] = ff_put_pixels16_xy2_no_rnd_armv6;"
)
s = s.replace(
    "/*     c->put_no_rnd_pixels_tab[1][3] = ff_put_pixels8_xy2_no_rnd_armv6; */",
    "    c->put_no_rnd_pixels_tab[1][3] = ff_put_pixels8_xy2_no_rnd_armv6;"
)

init.write_text(s)

print("Applied Old3DS ARMv6 MPEG xy2 DSP patch.")
