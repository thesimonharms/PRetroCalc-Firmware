/* ESP32-S3 PIE memset/memcpy — 16 bytes per ee.vst/ee.vld.
 * Windowed ABI: a2=dst, a3=val/src, a4=n. */

    .text
    .align  4

    .global s3_pie_memset
    .type   s3_pie_memset, @function
s3_pie_memset:
    entry   a1, 32
    beqz    a4, .Lms_done

.Lms_align:
    beqz    a4, .Lms_done
    movi    a5, 15
    and     a6, a2, a5
    beqz    a6, .Lms_aligned
    s8i     a3, a2, 0
    addi    a2, a2, 1
    addi    a4, a4, -1
    j       .Lms_align

.Lms_aligned:
    srli    a5, a4, 6              /* n / 64 */
    beqz    a5, .Lms_16
    s8i     a3, a1, 0
    ee.vldbc.8 q0, a1
    loopnez a5, .Lms_64_end
        ee.vst.128.ip q0, a2, 16
        ee.vst.128.ip q0, a2, 16
        ee.vst.128.ip q0, a2, 16
        ee.vst.128.ip q0, a2, 16
.Lms_64_end:
    movi    a5, 63
    and     a4, a4, a5

.Lms_16:
    srli    a5, a4, 4              /* remaining / 16 */
    beqz    a5, .Lms_tail
    s8i     a3, a1, 0
    ee.vldbc.8 q0, a1
    loopnez a5, .Lms_16_end
        ee.vst.128.ip q0, a2, 16
.Lms_16_end:
    movi    a5, 15
    and     a4, a4, a5

.Lms_tail:
    beqz    a4, .Lms_done
.Lms_tail_loop:
    s8i     a3, a2, 0
    addi    a2, a2, 1
    addi    a4, a4, -1
    bnez    a4, .Lms_tail_loop

.Lms_done:
    retw.n
    .size   s3_pie_memset, .-s3_pie_memset

    .align  4
    .global s3_pie_memcpy
    .type   s3_pie_memcpy, @function
s3_pie_memcpy:
    entry   a1, 32
    beqz    a4, .Lmc_done

    /* Different 16-byte alignment → byte copy (avoids unaligned l32i). */
    xor     a5, a2, a3
    movi    a6, 15
    and     a5, a5, a6
    bnez    a5, .Lmc_scalar

.Lmc_align:
    beqz    a4, .Lmc_done
    movi    a5, 15
    and     a6, a2, a5
    beqz    a6, .Lmc_aligned
    l8ui    a7, a3, 0
    s8i     a7, a2, 0
    addi    a2, a2, 1
    addi    a3, a3, 1
    addi    a4, a4, -1
    j       .Lmc_align

.Lmc_aligned:
    srli    a5, a4, 6              /* n / 64 */
    beqz    a5, .Lmc_16
    loopnez a5, .Lmc_64_end
        ee.vld.128.ip q0, a3, 16
        ee.vld.128.ip q1, a3, 16
        ee.vld.128.ip q2, a3, 16
        ee.vld.128.ip q3, a3, 16
        ee.vst.128.ip q0, a2, 16
        ee.vst.128.ip q1, a2, 16
        ee.vst.128.ip q2, a2, 16
        ee.vst.128.ip q3, a2, 16
.Lmc_64_end:
    movi    a5, 63
    and     a4, a4, a5

.Lmc_16:
    srli    a5, a4, 4
    beqz    a5, .Lmc_tail
    loopnez a5, .Lmc_16_end
        ee.vld.128.ip q0, a3, 16
        ee.vst.128.ip q0, a2, 16
.Lmc_16_end:
    movi    a5, 15
    and     a4, a4, a5

.Lmc_tail:
    beqz    a4, .Lmc_done
.Lmc_tail_loop:
    l8ui    a7, a3, 0
    s8i     a7, a2, 0
    addi    a2, a2, 1
    addi    a3, a3, 1
    addi    a4, a4, -1
    bnez    a4, .Lmc_tail_loop
    j       .Lmc_done

.Lmc_scalar:
    beqz    a4, .Lmc_done
.Lmc_scalar_byte:
    l8ui    a7, a3, 0
    s8i     a7, a2, 0
    addi    a2, a2, 1
    addi    a3, a3, 1
    addi    a4, a4, -1
    bnez    a4, .Lmc_scalar_byte

.Lmc_done:
    retw.n
    .size   s3_pie_memcpy, .-s3_pie_memcpy
