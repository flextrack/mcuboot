#ifndef MY_TEST_H
#define MY_TEST_H

#include <stdint.h>
#include "bootutil/bootutil.h"

int mytest_perform(void);
int mytest_validate_loaded_image(uint32_t image_base);
void mytest_show_config_and_clocks(void);

#endif /* MY_TEST_H */