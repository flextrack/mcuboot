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

static bool addr_in_range(uint32_t addr, uint32_t start, uint32_t end)
{
    return (addr >= start) && (addr < end);
}

static int sdram_pattern_test(uint32_t base_addr, size_t words)
{
    volatile uint32_t *p = (volatile uint32_t *)base_addr;
    size_t i;

    /* Pattern 1 */
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
            printk("SDRAM test fail P1 at 0x%08x: got=0x%08x exp=0x%08x\n",
                   (unsigned int)(base_addr + i * 4u),
                   (unsigned int)got,
                   (unsigned int)expected);
            return -1;
        }
    }

    /* Pattern 2 */
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
            printk("SDRAM test fail P2 at 0x%08x: got=0x%08x exp=0x%08x\n",
                   (unsigned int)(base_addr + i * 4u),
                   (unsigned int)got,
                   (unsigned int)expected);
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

    if (stride < sizeof(uint32_t))
    {
        stride = sizeof(uint32_t);
    }

    p = (volatile uint32_t *)base_addr;

    for (i = 0; i < words; i += (stride / sizeof(uint32_t)))
    {
        p[i] = 0x12340000u | (uint32_t)i;
    }

    for (i = 0; i < words; i += (stride / sizeof(uint32_t)))
    {
        uint32_t expected = 0x12340000u | (uint32_t)i;
        uint32_t got = p[i];
        if (got != expected)
        {
            printk("SDRAM stride fail at 0x%08x: got=0x%08x exp=0x%08x\n",
                   (unsigned int)(base_addr + i * 4u),
                   (unsigned int)got,
                   (unsigned int)expected);
            return -3;
        }
    }

    return 0;
}

int mytest_sdram_basic(void)
{
    uint32_t test_addr = MYTEST_SDRAM_BASE + MYTEST_SDRAM_TEST_OFFSET;
    int rc;

    printk("mytest: SDRAM basic test at 0x%08x\n", (unsigned int)test_addr);

    rc = sdram_pattern_test(test_addr, MYTEST_SDRAM_TEST_WORDS);
    if (rc != 0)
    {
        printk("mytest: sdram_pattern_test rc=%d\n", rc);
        return rc;
    }

    rc = sdram_stride_test(test_addr, 4096u, 64u);
    if (rc != 0)
    {
        printk("mytest: sdram_stride_test rc=%d\n", rc);
        return rc;
    }

    printk("mytest: SDRAM basic test PASS\n");
    return 0;
}

int mytest_check_vector_table(uint32_t image_base)
{
    uint32_t msp = *(volatile uint32_t *)(image_base + 0u);
    uint32_t reset = *(volatile uint32_t *)(image_base + 4u);

    printk("mytest: image_base=0x%08x MSP=0x%08x RESET=0x%08x\n",
           (unsigned int)image_base,
           (unsigned int)msp,
           (unsigned int)reset);

    if (!addr_in_range(msp, MYTEST_VALID_RAM_START, MYTEST_VALID_RAM_END))
    {
        printk("mytest: MSP out of expected RAM range\n");
        return -10;
    }

    if ((reset & 1u) == 0u)
    {
        printk("mytest: RESET handler is not Thumb address\n");
        return -11;
    }

    if (!addr_in_range(reset & ~1u, MYTEST_VALID_RAM_START, MYTEST_VALID_RAM_END))
    {
        printk("mytest: RESET handler out of expected RAM range\n");
        return -12;
    }

    printk("mytest: vector table looks sane\n");
    return 0;
}

/*
 * Optional very small execute test.
 * WARNING:
 * - enable only if you know the SDRAM region is executable
 * - this test writes 2 Thumb instructions into test area and jumps there
 */
typedef uint32_t (*mytest_exec_fn_t)(void);

int mytest_exec_from_sdram(void)
{
    uint32_t test_addr = MYTEST_SDRAM_BASE + MYTEST_SDRAM_TEST_OFFSET + 0x1000u;
    volatile uint16_t *code16 = (volatile uint16_t *)test_addr;
    volatile uint32_t *literal = (volatile uint32_t *)(test_addr + 4u);
    mytest_exec_fn_t fn;
    uint32_t ret;

    /*
     * Thumb code:
     *  ldr r0, [pc, #0]
     *  bx  lr
     *  .word 0xCAFEBABE
     */
    code16[0] = 0x4800u; /* ldr r0, [pc, #0] */
    code16[1] = 0x4770u; /* bx lr */
    *literal = 0xCAFEBABEu;

    __DSB();
    __ISB();

    fn = (mytest_exec_fn_t)(test_addr | 1u);
    ret = fn();

    printk("mytest: exec from SDRAM returned 0x%08x\n", (unsigned int)ret);

    if (ret != 0xCAFEBABEu)
    {
        printk("mytest: execute-from-SDRAM FAIL\n");
        return -20;
    }

    printk("mytest: execute-from-SDRAM PASS\n");
    return 0;
}

void mytest_show_clocks(void)
{
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