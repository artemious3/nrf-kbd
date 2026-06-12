/**
 * @file main.c
 * @author Dmitry Vdovenko
 * @brief BLE hid kbd
 * @date 2026-05-22
 *
 * @copyright Copyright (c) 2026
 *
 */

#include "hal/nrf_gpiote.h"
#include "zephyr/bluetooth/gap.h"
#include "zephyr/drivers/gpio.h"
#include "zephyr/drivers/timer/system_timer.h"
#include "zephyr/irq.h"

#include <assert.h>
#include <bluetooth/services/hids.h>
#include <dk_buttons_and_leds.h>
#include <hal/nrf_gpio.h>
#include <soc.h>
#include <stddef.h>
#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/bluetooth/services/dis.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/printk.h>
#include <zephyr/types.h>
// #include "app_nfc.h"


#include "hal/nrf_gpiote.h"
#include <nrfx_gpiote.h>
#include <gpiote_nrfx.h>
#include <hal/nrf_gpio.h>

#include "bt.h"

#define ADV_STATUS_LED DK_LED1
#define CON_STATUS_LED DK_LED2
#define ADV_LED_BLINK_INTERVAL 2000

static void configure_leds(void)
{
    int err;

    err = dk_leds_init();
    if (err)
    {
        printk("Cannot init LEDs (err: %d)\n", err);
    }
}

static void bas_notify(void)
{
    uint8_t battery_level = bt_bas_get_battery_level();

    battery_level--;

    if (!battery_level)
    {
        battery_level = 100U;
    }
    printk("battery_level %d\n", battery_level);
    bt_bas_set_battery_level(battery_level);
}

// assume they are on the same port
static const struct gpio_dt_spec gpio_buttons[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(button0), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button4), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button5), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button6), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button7), gpios),
    // DO NOT ADD MORE!!!
};
static const size_t gpio_buttons_num = sizeof(gpio_buttons) / sizeof(struct gpio_dt_spec);
BUILD_ASSERT(sizeof(gpio_buttons) / sizeof(struct gpio_dt_spec) <= 8, "There are only 8 event lines in GPIOTE on NRF52840");

struct  button_submit_work_t {
    struct k_work_delayable work;
    uint32_t codes;
} button_submit_work;

static volatile int64_t last_time_button_pressed = 0;

/* GPIO buttons handler, invoked by kernel shortly after ISR*/
void buttons_submit_handler(struct k_work * work)
{
    // mask of pressed codes
    // key with code i is pressed only if bit i is set in mask
    uint32_t codes_mask = button_submit_work.codes;
    uint8_t pressed_codes[gpio_buttons_num];
    uint8_t released_codes[gpio_buttons_num];
    printk("mask %d\n", codes_mask);
    size_t pressed_keys_num = 0;
    size_t released_keys_num = 0;

    for(uint8_t i = 0; i < gpio_buttons_num; ++i){
        if(codes_mask & (1UL << i)){
            if(gpio_pin_get_dt(&gpio_buttons[i]) > 0){
                printk("btn %d pressed", i);
                pressed_codes[pressed_keys_num++] = i + 0x1E;
            } else {
                printk("btn %d released", i);
                released_codes[released_keys_num++] = i + 0x1E;
            }
        }
    }

    hid_buttons_press(pressed_codes, pressed_keys_num);
    hid_buttons_release(released_codes, released_keys_num);

    last_time_button_pressed = k_uptime_get();
}

// K_THREAD_DEFINE(gpio_button0_submit_thread, 1024, gpio_button0_submit_handler, NULL, NULL, NULL, 7, 0, 0);


#define GPIOTE DT_NODELABEL(gpiote)

void gpiote_isr(void * arg){
    ARG_UNUSED(arg);
    uint32_t codes_mask = 0;
    for (int i = 0; i < gpio_buttons_num; ++i)
    {
        if(NRF_GPIOTE0->EVENTS_IN[i]){
            codes_mask |= (1U << i);
            NRF_GPIOTE0->EVENTS_IN[i] = 0x0;
        }
    }
    button_submit_work.codes = codes_mask;
    // If multiple interrupts occur in short time because of bouncing,
    // `button_submit_work` is invoked only once.
    k_work_reschedule(&button_submit_work.work, K_MSEC(10));
}

static int configure_buttons(void)
{

    // Default IRQ handler from GPIOTE driver seems to always handle PORT event.
    // According to NRF52840 datasheed, PORT event is *ALWAYS* enabled.
    // Even if the cause of interrupt is not port event, but PORT event condition occurs,
    // it is nevertheless handled, so we get 2 interrupts instead of 1.
    // Hence, we set up our custom interrupt handler that does not have this flaw.
    IRQ_CONNECT(DT_IRQN(GPIOTE), DT_IRQ(GPIOTE, priority), gpiote_isr, NULL, 0);

    uint32_t gpio_int_mask = 0;

    // Each button corresponds to a GPIOTE event channel (maximum 8).
    for(int i = 0; i < gpio_buttons_num; ++i){
        int err;

        err = gpio_pin_configure_dt(&gpio_buttons[i], GPIO_INPUT);
        if(err){
            printk("Failed to configure GPIO pin %d, error code : %d", i, err);
        }


        uint32_t absolute_pin_number  = NRF_PIN_PORT_TO_PIN_NUMBER(gpio_buttons[i].pin, 0);

        // NOTE : asuming port number 0
        nrf_gpiote_event_configure(NRF_GPIOTE0, i, absolute_pin_number, NRF_GPIOTE_POLARITY_TOGGLE);
        nrf_gpiote_event_enable(NRF_GPIOTE, i);

        // Generate DETECT signal on low GPIO level (that is, button press)
        // Thus, wake up from System OFF.
        nrf_gpio_cfg_sense_set(absolute_pin_number, NRF_GPIO_PIN_SENSE_LOW);
        gpio_int_mask |= (1U << i);
    }

    nrf_gpiote_int_enable(NRF_GPIOTE0, gpio_int_mask);

    irq_enable(DT_IRQN(GPIOTE));

    return 0;
}

int main(void)
{
    int err;
    int blink_status = 0;

    configure_leds();

    button_submit_work.codes = 0;
    k_work_init_delayable(&button_submit_work.work, buttons_submit_handler);
    configure_buttons();

    err = configure_bt();
    if (err)
    {
        printk("Failed to configure Bluetooth\n");
        return 0;
    }

    if (IS_ENABLED(CONFIG_SETTINGS))
    {
        settings_load();
    }

#if CONFIG_SAMPLE_NFC_OOB_PAIRING
    k_work_init(&adv_work, delayed_advertising_start);
    app_nfc_init();
#else
    advertising_start();
#endif /* CONFIG_SAMPLE_NFC_OOB_PAIRING */


    for (;;)
    {
        if (is_adv)
        {
            dk_set_led(ADV_STATUS_LED, (++blink_status) % 2);
        }
        else
        {
            dk_set_led(ADV_STATUS_LED, 0);
        }
        k_sleep(K_MSEC(ADV_LED_BLINK_INTERVAL));
        /* Battery level simulation */
        bas_notify();

        if(k_uptime_get() - last_time_button_pressed > 10000)
        {
            dk_set_led(0, 0);
            sys_clock_disable() ;
            sys_poweroff();
        }
    }
}
