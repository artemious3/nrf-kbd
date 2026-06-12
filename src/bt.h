#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/** @brief Release the button and send report
 *
 *  @note Functions to manipulate hid state are not reentrant
 *  @param keys
 *  @param cnt
 *
 *  @return 0 on success or negative error code.
 */
int hid_buttons_release(const uint8_t* keys, size_t cnt);

/** @brief Press a button and send report
 *
 *  @note Functions to manipulate hid state are not reentrant
 *  @param keys
 *  @param cnt
 *
 *  @return 0 on success or negative error code.
 */
int hid_buttons_press(const uint8_t* keys, size_t cnt);

/*
 * Reply on Pin comparison request
 */
void num_comp_reply(bool accept);

/*
 * Configure bluetooth
 */
int configure_bt(void);

/*
 * Start advertising
 */
void advertising_start(void);


/*
 * Is currently advertising
 */
extern volatile bool is_adv;
