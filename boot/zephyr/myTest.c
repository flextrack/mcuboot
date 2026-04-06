#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "fsl_clock.h"

#include "bootutil/bootutil.h"
#include "bootutil/image.h"

/*
 * Adjust these to your board/layout.
 */
#ifndef MYTEST_SDRAM_BASE
#define MYTEST_SDRAM_BASE 0x80000000u
#endif

#ifndef MYTEST_SDRAM_TEST_OFFSET
#define MYTEST_SDRAM_TEST_OFFSET 0x00100000u
#endif

#ifndef MYTEST_SDRAM_TEST_WORDS
#define MYTEST_SDRAM_TEST_WORDS 256u
#endif

#ifndef MYTEST_VALID_RAM_START
#define MYTEST_VALID_RAM_START 0x80000000u
#endif

#ifndef MYTEST_VALID_RAM_END
#define MYTEST_VALID_RAM_END 0x81000000u
#endif

#ifndef MYTEST_ENABLE_EXEC_TEST
#define MYTEST_ENABLE_EXEC_TEST 1
#endif

typedef uint32_t (*mytest_exec_fn_t)(void);

enum mytest_fail_bits
{
    MYTEST_FAIL_SDRAM_PATTERN = (1u << 0),
    MYTEST_FAIL_SDRAM_STRIDE = (1u << 1),
    MYTEST_FAIL_VECTOR_MSP_RANGE = (1u << 2),
    MYTEST_FAIL_VECTOR_RESET_THUMB = (1u << 3),
    MYTEST_FAIL_VECTOR_RESET_RANGE = (1u << 4),
    MYTEST_FAIL_EXEC_FROM_SDRAM = (1u << 5),
};

struct mytest_fail_desc
{
    uint32_t bit;
    const char *name;
};

static const struct mytest_fail_desc g_mytest_fail_desc[] =
    {
        {MYTEST_FAIL_SDRAM_PATTERN, "sdram pattern"},
        {MYTEST_FAIL_SDRAM_STRIDE, "sdram stride"},
        {MYTEST_FAIL_VECTOR_MSP_RANGE, "vector msp range"},
        {MYTEST_FAIL_VECTOR_RESET_THUMB, "vector reset thumb"},
        {MYTEST_FAIL_VECTOR_RESET_RANGE, "vector reset range"},
        {MYTEST_FAIL_EXEC_FROM_SDRAM, "exec from sdram"},
};

static bool addr_in_range(uint32_t addr, uint32_t start, uint32_t end)
{
    return (addr >= start) && (addr < end);
}

static int sdram_pattern_test(uint32_t base_addr, size_t words)
{
    volatile uint32_t *p = (volatile uint32_t *)base_addr;
    size_t i;

    for (i = 0; i < words; ++i)
    {
        p[i] = 0xAAAAAAAAu ^ (uint32_t)i;
    }

    for (i = 0; i < words; ++i)
    {
        uint32_t expected = 0xAAAAAAAAu ^ (uint32_t)i;
        uint32_t got = p[i];
        if (got != expected)
        {
            return -1;
        }
    }

    for (i = 0; i < words; ++i)
    {
        p[i] = 0x55555555u ^ ((uint32_t)i * 0x01010101u);
    }

    for (i = 0; i < words; ++i)
    {
        uint32_t expected = 0x55555555u ^ ((uint32_t)i * 0x01010101u);
        uint32_t got = p[i];
        if (got != expected)
        {
            return -2;
        }
    }

    return 0;
}

static int sdram_stride_test(uint32_t base_addr, size_t bytes, size_t stride)
{
    volatile uint32_t *p;
    size_t i;
    size_t words = bytes / sizeof(uint32_t);
    size_t step_words;

    if (stride < sizeof(uint32_t))
    {
        stride = sizeof(uint32_t);
    }

    step_words = stride / sizeof(uint32_t);
    if (step_words == 0u)
    {
        step_words = 1u;
    }

    p = (volatile uint32_t *)base_addr;

    for (i = 0; i < words; i += step_words)
    {
        p[i] = 0x12340000u | (uint32_t)i;
    }

    for (i = 0; i < words; i += step_words)
    {
        uint32_t expected = 0x12340000u | (uint32_t)i;
        uint32_t got = p[i];
        if (got != expected)
        {
            return -3;
        }
    }

    return 0;
}

int mytest_perform(void)
{
    uint32_t fail_mask = 0u;
    uint32_t test_addr = MYTEST_SDRAM_BASE + MYTEST_SDRAM_TEST_OFFSET;
    int rc;
    size_t i;

    rc = sdram_pattern_test(test_addr, MYTEST_SDRAM_TEST_WORDS);
    if (rc != 0)
    {
        fail_mask |= MYTEST_FAIL_SDRAM_PATTERN;
    }

    rc = sdram_stride_test(test_addr, 4096u, 64u);
    if (rc != 0)
    {
        fail_mask |= MYTEST_FAIL_SDRAM_STRIDE;
    }

#if defined(MYTEST_ENABLE_EXEC_TEST) && (MYTEST_ENABLE_EXEC_TEST != 0)
    {
        uint32_t exec_addr = MYTEST_SDRAM_BASE + MYTEST_SDRAM_TEST_OFFSET + 0x1000u;
        volatile uint16_t *code16 = (volatile uint16_t *)exec_addr;
        volatile uint32_t *literal = (volatile uint32_t *)(exec_addr + 4u);
        mytest_exec_fn_t fn;
        uint32_t ret;

        /*
         * Thumb code:
         *  ldr r0, [pc, #0]
         *  bx  lr
         *  .word 0xCAFEBABE
         */
        code16[0] = 0x4800u;
        code16[1] = 0x4770u;
        *literal = 0xCAFEBABEu;

        __DSB();
        __ISB();

        fn = (mytest_exec_fn_t)(exec_addr | 1u);
        ret = fn();

        if (ret != 0xCAFEBABEu)
        {
            fail_mask |= MYTEST_FAIL_EXEC_FROM_SDRAM;
        }
    }
#endif

    for (i = 0; i < ARRAY_SIZE(g_mytest_fail_desc); ++i)
    {
        if ((fail_mask & g_mytest_fail_desc[i].bit) != 0u)
        {
            printk("E: - %s : fail\n", g_mytest_fail_desc[i].name);
        }
        else
        {
            printk("I: - %s : ok\n", g_mytest_fail_desc[i].name);
        }
    }

    if (fail_mask == 0u)
    {
        return 0;
    }

    return -(int)fail_mask;
}

int mytest_validate_loaded_image(uint32_t image_base)
{
    uint32_t fail_mask = 0u;
    uint32_t msp;
    uint32_t reset;

    msp = *(volatile uint32_t *)(image_base + 0u);
    reset = *(volatile uint32_t *)(image_base + 4u);

    if (!addr_in_range(msp, MYTEST_VALID_RAM_START, MYTEST_VALID_RAM_END))
    {
        fail_mask |= MYTEST_FAIL_VECTOR_MSP_RANGE;
    }

    if ((reset & 1u) == 0u)
    {
        fail_mask |= MYTEST_FAIL_VECTOR_RESET_THUMB;
    }

    if (!addr_in_range(reset & ~1u, MYTEST_VALID_RAM_START, MYTEST_VALID_RAM_END))
    {
        fail_mask |= MYTEST_FAIL_VECTOR_RESET_RANGE;
    }

    printk((fail_mask & MYTEST_FAIL_VECTOR_MSP_RANGE) ? "E: - vector msp range : fail\n"
                                                      : "I: - vector msp range : ok\n");
    printk((fail_mask & MYTEST_FAIL_VECTOR_RESET_THUMB) ? "E: - vector reset thumb : fail\n"
                                                        : "I: - vector reset thumb : ok\n");
    printk((fail_mask & MYTEST_FAIL_VECTOR_RESET_RANGE) ? "E: - vector reset range : fail\n"
                                                        : "I: - vector reset range : ok\n");

    if (fail_mask == 0u)
    {
        return 0;
    }

    return -(int)fail_mask;
}

void mytest_show_config_and_clocks(void)
{
    printk("=== SEMC SDRAM Controller Registers ===\n");
    printk("MCR      = 0x%08x\n", SEMC->MCR);
    printk("IOCR     = 0x%08x\n", SEMC->IOCR);
    printk("BR0      = 0x%08x\n", SEMC->BR[0]);
    printk("BMCR0    = 0x%08x\n", SEMC->BMCR0);
    printk("BMCR1    = 0x%08x\n", SEMC->BMCR1);
    printk("SDRAMCR0 = 0x%08x\n", SEMC->SDRAMCR0);
    printk("SDRAMCR1 = 0x%08x\n", SEMC->SDRAMCR1);
    printk("SDRAMCR2 = 0x%08x\n", SEMC->SDRAMCR2);
    printk("SDRAMCR3 = 0x%08x\n", SEMC->SDRAMCR3);

    printk("=== Clock Frequencies (i.MX RT1062) ===\n");
    printk("kCLOCK_CpuClk          : %u Hz\n", CLOCK_GetFreq(kCLOCK_CpuClk));
    printk("kCLOCK_AhbClk          : %u Hz\n", CLOCK_GetFreq(kCLOCK_AhbClk));
    printk("kCLOCK_SemcClk         : %u Hz\n", CLOCK_GetFreq(kCLOCK_SemcClk));
    printk("kCLOCK_IpgClk          : %u Hz\n", CLOCK_GetFreq(kCLOCK_IpgClk));
    printk("kCLOCK_PerClk          : %u Hz\n", CLOCK_GetFreq(kCLOCK_PerClk));
    printk("kCLOCK_OscClk          : %u Hz\n", CLOCK_GetFreq(kCLOCK_OscClk));
    printk("kCLOCK_RtcClk          : %u Hz\n", CLOCK_GetFreq(kCLOCK_RtcClk));

    printk("kCLOCK_ArmPllClk       : %u Hz\n", CLOCK_GetFreq(kCLOCK_ArmPllClk));
    printk("kCLOCK_Usb1PllClk      : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1PllClk));
    printk("kCLOCK_Usb1PllPfd0Clk  : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1PllPfd0Clk));
    printk("kCLOCK_Usb1PllPfd1Clk  : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1PllPfd1Clk));
    printk("kCLOCK_Usb1PllPfd2Clk  : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1PllPfd2Clk));
    printk("kCLOCK_Usb1PllPfd3Clk  : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1PllPfd3Clk));
    printk("kCLOCK_Usb1SwClk       : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1SwClk));
    printk("kCLOCK_Usb1Sw120MClk   : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1Sw120MClk));
    printk("kCLOCK_Usb1Sw60MClk    : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1Sw60MClk));
    printk("kCLOCK_Usb1Sw80MClk    : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb1Sw80MClk));

    printk("kCLOCK_Usb2PllClk      : %u Hz\n", CLOCK_GetFreq(kCLOCK_Usb2PllClk));

    printk("kCLOCK_SysPllClk       : %u Hz\n", CLOCK_GetFreq(kCLOCK_SysPllClk));
    printk("kCLOCK_SysPllPfd0Clk   : %u Hz\n", CLOCK_GetFreq(kCLOCK_SysPllPfd0Clk));
    printk("kCLOCK_SysPllPfd1Clk   : %u Hz\n", CLOCK_GetFreq(kCLOCK_SysPllPfd1Clk));
    printk("kCLOCK_SysPllPfd2Clk   : %u Hz\n", CLOCK_GetFreq(kCLOCK_SysPllPfd2Clk));
    printk("kCLOCK_SysPllPfd3Clk   : %u Hz\n", CLOCK_GetFreq(kCLOCK_SysPllPfd3Clk));

    printk("kCLOCK_EnetPll0Clk     : %u Hz\n", CLOCK_GetFreq(kCLOCK_EnetPll0Clk));
    printk("kCLOCK_EnetPll1Clk     : %u Hz\n", CLOCK_GetFreq(kCLOCK_EnetPll1Clk));
    printk("kCLOCK_EnetPll2Clk     : %u Hz\n", CLOCK_GetFreq(kCLOCK_EnetPll2Clk));

    printk("kCLOCK_AudioPllClk     : %u Hz\n", CLOCK_GetFreq(kCLOCK_AudioPllClk));
    printk("kCLOCK_VideoPllClk     : %u Hz\n", CLOCK_GetFreq(kCLOCK_VideoPllClk));

    printk("Root Clocks:\n");
    printk("kCLOCK_FlexspiClkRoot : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_FlexspiClkRoot));
    printk("kCLOCK_LpspiClkRoot   : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_LpspiClkRoot));
    printk("kCLOCK_TraceClkRoot   : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_TraceClkRoot));
    printk("kCLOCK_Sai1ClkRoot    : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_Sai1ClkRoot));
    printk("kCLOCK_Sai3ClkRoot    : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_Sai3ClkRoot));
    printk("kCLOCK_Lpi2cClkRoot   : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_Lpi2cClkRoot));
    printk("kCLOCK_UartClkRoot    : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_UartClkRoot));
    printk("kCLOCK_SpdifClkRoot   : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_SpdifClkRoot));
    printk("kCLOCK_Flexio1ClkRoot : %u Hz\n", CLOCK_GetClockRootFreq(kCLOCK_Flexio1ClkRoot));
}