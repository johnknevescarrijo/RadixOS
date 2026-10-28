#include <radix/hal.h>
#include <radix/types.h>

#ifdef RADIX_HOST_TEST

void wdt_disable_all(void) {
    /* No hardware watchdogs on host */
}

#else

#include <esp32s3_soc.h>

void wdt_disable_all(void) {
    /* 1. Disable Timer Group 0 Watchdog (TIMG0 WDT) */
    REG_WRITE(TIMG_WDTWPROTECT_REG(DR_REG_TIMG0_BASE), TIMG_WDT_WKEY_VALUE);
    REG_WRITE(TIMG_WDTCONFIG0_REG(DR_REG_TIMG0_BASE), 0);
    REG_WRITE(TIMG_WDTWPROTECT_REG(DR_REG_TIMG0_BASE), 0);

    /* 2. Disable Timer Group 1 Watchdog (TIMG1 WDT) */
    REG_WRITE(TIMG_WDTWPROTECT_REG(DR_REG_TIMG1_BASE), TIMG_WDT_WKEY_VALUE);
    REG_WRITE(TIMG_WDTCONFIG0_REG(DR_REG_TIMG1_BASE), 0);
    REG_WRITE(TIMG_WDTWPROTECT_REG(DR_REG_TIMG1_BASE), 0);

    /* 3. Disable RTC Controller Watchdog (RTC WDT) */
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, RTC_CNTL_WDT_WKEY_VALUE);
    REG_WRITE(RTC_CNTL_WDTCONFIG0_REG, 0);
    REG_WRITE(RTC_CNTL_WDTWPROTECT_REG, 0);

    /* 4. Disable Super Watchdog (SWD) */
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0x12345678);
    REG_SET_BITS(RTC_CNTL_SWD_CONF_REG, RTC_CNTL_SWD_DISABLE);
    REG_WRITE(RTC_CNTL_SWD_WPROTECT_REG, 0);
}

#endif /* RADIX_HOST_TEST */
