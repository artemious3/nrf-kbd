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


#include "bt.h"

#define ADV_STATUS_LED DK_LED1
#define CON_STATUS_LED DK_LED2
#define ADV_LED_BLINK_INTERVAL 2000

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

#define GPIO_BUTTONS_NUM  ( sizeof(gpio_buttons) / sizeof(struct gpio_dt_spec) )

static const struct device * gpio0 =  DEVICE_DT_GET(DT_NODELABEL(gpio0));
static const struct device * gpio1 =  DEVICE_DT_GET(DT_NODELABEL(gpio1));

struct k_work poll_until_all_released_work;

static volatile uint32_t last_time_button_pressed = 0;


void poll_until_all_released(struct k_work * work){


    int8_t stable_change_cnt[GPIO_BUTTONS_NUM] = {0};
    uint32_t stable_states_mask = 0;
    uint32_t changing_states_mask = 0;

    uint8_t pressed_keys[KEYS_MAX_LEN];
    uint8_t released_keys[KEYS_MAX_LEN];

    do {

        size_t pressed_keys_cnt = 0;
        size_t released_keys_cnt = 0;

        // IMPORTANT TODO : fix potential out ot bounds index
        for(int i = 0; i < GPIO_BUTTONS_NUM; ++i){

            bool stable_pressed = stable_states_mask & (1UL << i);
            bool current_pressed = gpio_pin_get_dt(&gpio_buttons[i]);

            // detect state change
            if((!stable_pressed && current_pressed)
                ||
                (stable_pressed && !current_pressed)
            ){
                printk("%d ++\n", i);
                stable_change_cnt[i]++;
                changing_states_mask |= (1UL << i);
            } else {
                // printk("%d reset\n", i);
                stable_change_cnt[i] = 0;
                changing_states_mask &= ~(1UL << i);
            }

            if(stable_change_cnt[i] == 2){
                if(current_pressed){
                    printk("report %d pressed\n", i);
                    stable_states_mask |= (1UL << i);
                    pressed_keys[pressed_keys_cnt++] = i + 0x1E;
                } else {
                    printk("report %d released\n", i);
                    stable_states_mask &= ~(1UL << i);
                    released_keys[released_keys_cnt++] = i + 0x1E;
                }
                // probably same, should be out of loop
                // stable_states_mask ^= changing_states_mask;
                stable_change_cnt[i] = 0;
                changing_states_mask &= ~(1UL << i);
                last_time_button_pressed = k_uptime_get();
            }
        }
        // printk("stable states mask %x\n", stable_states_mask);

        if(pressed_keys_cnt>0){
            hid_buttons_press(pressed_keys, pressed_keys_cnt);
        }
        if(released_keys_cnt>0){

            hid_buttons_release(released_keys, released_keys_cnt);
        }
        k_sleep(K_MSEC(5));

    } while((changing_states_mask | stable_states_mask) != 0);

}


#define GPIOTE DT_NODELABEL(gpiote)

static void gpiote_isr(void * arg){
    // only if we are not already polling
    // printk("port event!\n");
    if(!k_work_is_pending(&poll_until_all_released_work)){
        printk("work submit\n");
        k_work_submit(&poll_until_all_released_work);
    }
    nrf_gpiote_event_clear(NRF_GPIOTE0, NRF_GPIOTE_EVENT_PORT);
    NRF_P0->LATCH=~0;
    NRF_P1->LATCH=~0;
}

static int configure_buttons(void)
{

    IRQ_CONNECT(DT_IRQN(GPIOTE), DT_IRQ(GPIOTE, priority), gpiote_isr, NULL, 0);


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

        // Generate DETECT signal on low GPIO level (that is, button press)
        // Thus, wake up from System OFF.
        nrf_gpio_cfg_sense_set(absolute_pin_number, NRF_GPIO_PIN_SENSE_LOW);
    }

    nrf_gpiote_int_enable(NRF_GPIOTE0, NRF_GPIOTE_INT_PORT_MASK);

    irq_enable(DT_IRQN(GPIOTE));

    return 0;
}

int main(void)
{
    printk("resetreas : %x\n", NRF_POWER->RESETREAS);
    printk("mainregstatus : %x\n", NRF_POWER->MAINREGSTATUS);
    int err;
    int blink_status = 0;

    err = configure_leds();
    if(err){
        printk("Failed to configure LEDs\n");
        return 0;
    }

    k_work_init(&poll_until_all_released_work, poll_until_all_released);

    err = configure_buttons();
    if(err){
        printk("Failed to configure buttons\n");
        return 0;
    }

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

    int cnt = 0;

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
            NRF_POWER->RESETREAS=0x0;
            sys_clock_disable() ;
            sys_poweroff();
        }
    }
}
