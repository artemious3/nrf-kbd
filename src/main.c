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
#include <hal/nrf_gpio.h>
#include <hal/nrf_power.h>
#include <nrfx_power.h>
#include <nrfx.h>


#include "bt.h"

#define ADV_STATUS_LED DK_LED1
#define CON_STATUS_LED DK_LED2
#define ADV_LED_BLINK_INTERVAL 2000
#define UPTIME_BEFORE_SLEEP_MS 30000

static int configure_leds(void)
{
    int err;

    err = dk_leds_init();
    if (err)
    {
        printk("Cannot init LEDs (err: %d)\n", err);
        return err;
    }
    return 0;
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

static const struct gpio_dt_spec gpio_buttons[] = {
    GPIO_DT_SPEC_GET(DT_NODELABEL(button0), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button1), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button2), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button3), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button4), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button5), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button6), gpios),
    GPIO_DT_SPEC_GET(DT_NODELABEL(button7), gpios),
};
#define GPIO_BUTTONS_NUM (sizeof(gpio_buttons) / sizeof(struct gpio_dt_spec))
BUILD_ASSERT(GPIO_BUTTONS_NUM <= 8, "There are only 8 event lines in GPIOTE on NRF52840");
BUILD_ASSERT(GPIO_BUTTONS_NUM <= KEY_PRESS_MAX, "Function `button_submit_handler` requires that GPIO_BUTTONS_NUM <= KEYS_PRESS_MAX");

static const struct device * gpio0 =  DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device * gpio1 =  DEVICE_DT_GET(DT_NODELABEL(gpio1));

struct  button_submit_work_t {
    struct k_work_delayable work;
    uint32_t codes;
} button_submit_work;

static volatile int64_t last_time_button_pressed = 0;

//  Mask of button states, used to fast check that all are released
static uint8_t btn_states_mask = 0;

/* GPIO buttons handler, invoked by kernel shortly after ISR*/
static void buttons_submit_handler(struct k_work * work)
{
    // mask of pressed codes
    // key with code i is pressed only if bit i is set in mask
    uint32_t codes_mask = button_submit_work.codes;
    uint8_t pressed_codes[KEY_PRESS_MAX];
    uint8_t released_codes[KEY_PRESS_MAX];
    size_t pressed_keys_num = 0;
    size_t released_keys_num = 0;

    printk("mask %d\n", codes_mask);

    for(uint8_t i = 0; i < GPIO_BUTTONS_NUM; ++i){
        if(codes_mask & (1UL << i)){
            if(gpio_pin_get_dt(&gpio_buttons[i]) > 0){
                printk("btn %d pressed\n", i);
                // safe: pressd_keys_num++ < GPIO_BUTTONS_NUM <= KEYS_PRESS_MAX
                pressed_codes[pressed_keys_num++] = i + 0x1E;
                btn_states_mask |= (1U << i);
            } else {
                printk("btn %d released\n", i);
                // safe: released_keys_num++ < GPIO_BUTTONS_NUM <= KEYS_PRESS_MAX
                released_codes[released_keys_num++] = i + 0x1E;
                btn_states_mask &= ~(1U << i);
            }
        }
    }

    hid_buttons_press(pressed_codes, pressed_keys_num);
    hid_buttons_release(released_codes, released_keys_num);

    last_time_button_pressed = k_uptime_get();

    button_submit_work.codes = 0x0;
}



#define GPIOTE DT_NODELABEL(gpiote)

static void gpiote_isr(void * arg){
    ARG_UNUSED(arg);
    uint32_t codes_mask = 0;
    for (int i = 0; i < GPIO_BUTTONS_NUM; ++i)
    {
        if(NRF_GPIOTE0->EVENTS_IN[i]){
            codes_mask |= (1U << i);
            NRF_GPIOTE0->EVENTS_IN[i] = 0x0;
        }
    }
    button_submit_work.codes |= codes_mask;
    // If multiple interrupts occur in short time because of bouncing,
    // `button_submit_work` is invoked only once.
    k_work_reschedule(&button_submit_work.work, K_MSEC(10));
}


static volatile bool low_power_warning_received = false;

static void powwarn_handler(){
    low_power_warning_received = true;
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
    for(int i = 0; i < GPIO_BUTTONS_NUM; ++i){
        int err;

        err = gpio_pin_configure_dt(&gpio_buttons[i], GPIO_INPUT);
        if(err){
            printk("Failed to configure GPIO pin %d, error code : %d", i, err);
            return err;
        }

        // check if pin is on either of 2 ports : gpio 0 or gpio1
        __ASSERT(gpio_buttons[i].port == gpio0 || gpio_buttons[i].port == gpio1, "Button is not on gpio0 or gpio1");

        uint32_t port_number = (gpio_buttons[i].port == gpio0 ? (0U) : (1U));
        uint32_t absolute_pin_number  = NRF_PIN_PORT_TO_PIN_NUMBER(gpio_buttons[i].pin, port_number);

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


#define POWER DT_NODELABEL(power)
static void configure_pofcon(){

    nrfx_power_config_t power_config  = {
        .dcdcen = 0,
        .dcdcenhv = 0
    };
    int err = nrfx_power_init(&power_config);
    if (err) {
        printk("Power init failed: %d\n", err);
    }

    // 2. Configure Power Failure Warning (POFWARN)
    // Example: Trigger warning if VDD drops below 2.8V
    nrfx_power_pofwarn_config_t pof_config = {
        .handler = powwarn_handler,
        .thrvddh  = NRF_POWER_POFTHRVDDH_V28,
    };

    nrfx_power_pof_init(&pof_config);
    nrfx_power_pof_enable(&pof_config);
    printk("POFWARN Event Handler Enabled\n");

}

int main(void)
{
    printk("resetreas : %x\n", NRF_POWER->RESETREAS);
    printk("mainregstatus : %x\n", NRF_POWER->MAINREGSTATUS);
    int err;
    int blink_status = 0;

    // Buttons (i.e wake up sources) must be configured before POFCON
    err = configure_buttons();
    if(err){
        printk("Failed to configure buttons\n");
        return 0;
    }

    configure_pofcon();
    __DSB();
    if(low_power_warning_received){
        printk("Early power warning received, entering System OFF\n");
        sys_clock_disable();
        sys_poweroff();
    }

    err = configure_leds();
    if(err){
        printk("Failed to configure LEDs\n");
        return 0;
    }

    button_submit_work.codes = 0;
    k_work_init_delayable(&button_submit_work.work, buttons_submit_handler);


    err = configure_bt();
    if (err)
    {
        printk("Failed to configure Bluetooth\n");
        return 0;
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

        // Check if no button is held constantly and no action was performed in last UPTIME_BEFORE_SLEEP_MS
        if((k_uptime_get() - last_time_button_pressed >= UPTIME_BEFORE_SLEEP_MS && btn_states_mask == 0x0) || low_power_warning_received){
            if(low_power_warning_received){
                printk("Low power!\n");
            }
            dk_set_led(0, 0);
            NRF_POWER->RESETREAS=0x0;
            sys_clock_disable() ;
            sys_poweroff();
        }

    }
}
