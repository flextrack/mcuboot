#ifndef MY_TEST_H
#define MY_TEST_H

#include <stdint.h>
#include "bootutil/bootutil.h"

int mytest_sdram_basic(void);
int mytest_check_vector_table(uint32_t image_base);
int mytest_exec_from_sdram(void);
int mytest_show_clocks(void);

#endif /* MY_TEST_H */