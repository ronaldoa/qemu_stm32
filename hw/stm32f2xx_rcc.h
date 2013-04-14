#include "sysbus.h"
#include "clktree.h"
#include "stm32f2xx.h"
#include "stm32_rcc.h"

typedef struct Stm32f2xxRcc {
    /* Inherited */
    union {
        Stm32Rcc inherited;
        struct {
            /* Inherited */
            SysBusDevice busdev;

            /* Properties */
            uint32_t osc_freq;
            uint32_t osc32_freq;

            /* Private */
            MemoryRegion iomem;
            qemu_irq irq;
        };
    };
    
    /* Peripheral clocks */
    Clk PERIPHCLK[STM32F2XX_PERIPH_COUNT], // MUST be first field after `inherited`, because Stm32Rcc's last field aliases this array
    HSICLK,
    HSECLK,
    LSECLK,
    LSICLK,
    SYSCLK,
    PLLXTPRECLK,
    PLLCLK,
    HCLK, /* Output from AHB Prescaler */
    PCLK1, /* Output from APB1 Prescaler */
    PCLK2; /* Output from APB2 Prescaler */

    /* Register Values */
    uint32_t
    RCC_APB1ENR,
    RCC_APB2ENR;

    /* Register Field Values */
    uint32_t
    RCC_CFGR_PLLMUL,
    RCC_CFGR_PLLXTPRE,
    RCC_CFGR_PLLSRC,
    RCC_CFGR_PPRE1,
    RCC_CFGR_PPRE2,
    RCC_CFGR_HPRE,
    RCC_CFGR_SW;
} Stm32f2xxRcc;
