// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2023 Maciej Baczmanski, Michal Kawiak, Jakub Mazur

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/types.h>
#include <string.h>
#include <stdlib.h>
#include "return_codes.h"
#include "driver.h"

#include "calculations.h"

/// Unit conversion defines
#define MIN_TO_MS 60000
#define RPM_TO_MRPM 1000

#define PINS_PER_CHANNEL 2

#define MINIMUM_SPEED (CONFIG_SPEED_MAX_MRPM / CONFIG_MIN_SPEED_MODIFIER)

static const struct DriverVersion driver_ver = {
	.major = 2,
	.minor = 2,
};

/// temporary debug only variables: - To be deleted after developemnt is finished!
static uint64_t count_timer;
static int32_t ret_debug = 100;// DEBUG ONLY

#pragma region InternalFunctions// declarations of internal functions
static void enc_callback_wrapper(const struct device *dev, struct gpio_callback *cb, uint32_t pins);
static int speed_pwm_set(uint32_t value, enum ChannelNumber chnl);
static void update_speed_and_position_continuous(struct k_work *work);
static void continuous_calculation_timer_handler(struct k_timer *dummy);
static void enc_callback(enum ChannelNumber chnl, enum PinNumber pin);
static int set_direction_raw(enum MotorDirection direction, enum ChannelNumber chnl);
#pragma endregion InternalFunctions

/// CONTROL MODE - whether speed or position is controlled
static enum ControlModes control_mode;
/// encoder timer - timer is common for both channels
struct k_timer continuous_calculation_timer;
/// driver initialized (Was init function called)?
static bool drv_initialised;

// Struct representing singular channel, including channel - dependent variables
struct DriverChannel {
	/// PINS definitions
	/// motor out
	const struct pwm_dt_spec pwm_motor_driver;
	const struct gpio_dt_spec set_dir_pins[PINS_PER_CHANNEL];
	/// enc in
	const struct gpio_dt_spec enc_pins[PINS_PER_CHANNEL];
	struct gpio_callback enc_cb[PINS_PER_CHANNEL];

	/// ENCODER - variables used for actual speed calculation based on encoder pin and
	// timer interrupts
	// TODO - mutex locking instead of volatile?
	int64_t count_cycles;
	int64_t old_count_cycles;

	/// SPEED control
	uint32_t target_speed_mrpm;// Target set by user
	uint32_t actual_mrpm; // actual speed calculated from encoder pins
	uint32_t speed_control; // PID output -> pwm calculations input

	// TODO - separe target_speed_mrpm and speed_control for position control
	// from speed control!

	/// POSITION control
	uint32_t curr_pos;
	uint32_t target_position;
	uint32_t target_speed_for_pos_cntrl;
	bool target_achieved;
	// bool target_achieved_prev_debug;

	int32_t position_delta; //TEMP
	// I think it can be turned into local variable only in method that uses it?

	bool prev_val_enc[PINS_PER_CHANNEL];
	enum MotorDirection actual_dir;

	/// BOOLS - is motor on?
	bool is_motor_on; // was motor_on function called?
};

static struct DriverChannel drv_chnls[CONFIG_SUPPORTED_CHANNEL_NUMBER] = {
	{
		.pwm_motor_driver =  PWM_DT_SPEC_GET_BY_IDX(DT_ALIAS(pwm_drv), 0),
		.set_dir_pins[P0] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(set_dir_p1), gpios, 0),
		.set_dir_pins[P1] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(set_dir_p2), gpios, 0),
		.enc_pins[P0] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(get_enc_p1), gpios, 0),
		.enc_pins[P1] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(get_enc_p2), gpios, 0),
		.actual_dir = STOPPED
	},
	#if (CONFIG_SUPPORTED_CHANNEL_NUMBER > 1)
	{
		.pwm_motor_driver =  PWM_DT_SPEC_GET_BY_IDX(DT_ALIAS(pwm_drv), 1),
		.set_dir_pins[P0] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(set_dir_p1), gpios, 1),
		.set_dir_pins[P1] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(set_dir_p2), gpios, 1),
		.enc_pins[P0] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(get_enc_p1), gpios, 1),
		.enc_pins[P1] = GPIO_DT_SPEC_GET_BY_IDX(DT_ALIAS(get_enc_p2), gpios, 1),
		.actual_dir = STOPPED
	}
	#endif
};

#if defined(CONFIG_BOARD_NRF52840DONGLE_NRF52840) // BOOT functionality
static const struct gpio_dt_spec out_boot = GPIO_DT_SPEC_GET(DT_ALIAS(enter_boot_p), gpios);
void enter_boot(void)
{
	gpio_pin_configure_dt(&out_boot, GPIO_OUTPUT);
}
#endif

// Difference from target position and actual position
static inline void calculate_pos_delta(enum ChannelNumber chnl, bool *is_target_behind)
{
	drv_chnls[chnl].position_delta = drv_chnls[chnl].target_position -
					 drv_chnls[chnl].curr_pos;
	// Check if motor should spin backward to get to the target
	// (if target is smalller number than current position)
	if (drv_chnls[chnl].position_delta < 0) {
		drv_chnls[chnl].position_delta =
		-drv_chnls[chnl].position_delta;
		if (is_target_behind != NULL)
			*is_target_behind = true;
	}
}

// function implementations:
#pragma region TimerWorkCallback // timer interrupt internal functions
static void update_speed_and_position_continuous(struct k_work *work)
{
	int64_t diff = 0;

	for (enum ChannelNumber chnl = 0; chnl < CONFIG_SUPPORTED_CHANNEL_NUMBER; ++chnl) {
		count_timer += 1; // debug only

		// count encoder interrupts between timer interrupts:
		diff = drv_chnls[chnl].count_cycles - drv_chnls[chnl].old_count_cycles;
		// if diff is positive, motor is going FWD, else, BCK

		// TODO - add some possibility to "wrap" around int64 limit!

		drv_chnls[chnl].old_count_cycles = drv_chnls[chnl].count_cycles;


		if (diff > 0) {
			drv_chnls[chnl].actual_dir = FORWARD;
		} else if (diff < 0) {
			drv_chnls[chnl].actual_dir = BACKWARD;
		} else {
			drv_chnls[chnl].actual_dir = STOPPED;
		}

		// calculate actual position
		drv_chnls[chnl].curr_pos = WRAP_POS_TO_RANGE(
						CALC_NEW_POS(
							INTERRUPT_COUNT_TO_DEG_DIFF(diff),
							chnl)
						);

		// calculate actual speed
		drv_chnls[chnl].actual_mrpm = RPM_TO_MRPM * MIN_TO_MS * ((uint64_t)llabs(diff)) /
			(CONFIG_ENC_STEPS_PER_ROTATION *
			CONFIG_GEARSHIFT_RATIO *
			CONFIG_ENC_TIMER_PERIOD_MS);

		// if motor is on, calculate control value
		if (get_motor_off_on(chnl)) {
#if defined(CONFIG_SPEED_CONTROL_ENABLE)
			int ret;
			if (control_mode == SPEED) {
				// increase or decrese speed each iteration by Kp * speed_delta
				drv_chnls[chnl].speed_control =
					(uint32_t)(drv_chnls[chnl].speed_control +
						   calculate_pid(
							drv_chnls[chnl].target_speed_mrpm,
							drv_chnls[chnl].actual_mrpm,
							&PID_spd
						   )
						  );

	// TODO - currently, this p(id) is more aggresive if CONFIG_ENC_TIMER_PERIOD_MS is
	// lower. Correct it, make P(ID) only time dependent, not "refresh rate" dependent!

				// Cap control at max rpm speed,
				// to avoid cumulation of too high speeds.
				if (drv_chnls[chnl].speed_control > CONFIG_SPEED_MAX_MRPM) {
					drv_chnls[chnl].speed_control = CONFIG_SPEED_MAX_MRPM;
				}

				ret = speed_pwm_set(drv_chnls[chnl].speed_control, chnl);
			}
#endif
#if defined(CONFIG_POS_CONTROL_ENABLE)
			if (control_mode == POSITION) {
				bool is_target_behind = false;
				uint32_t delta_shortest_path = 0;

				calculate_pos_delta(chnl, &is_target_behind);

				// Set proper spinning direction
				if (drv_chnls[chnl].position_delta < DEGREES_PER_HALF_ROTATION) {
					// If target is closer that 180 degree

					if (!is_target_behind)
						set_direction_raw(FORWARD, chnl);
					else
						set_direction_raw(BACKWARD, chnl);

					delta_shortest_path = drv_chnls[chnl].position_delta;
				} else {
					if (!is_target_behind)
						set_direction_raw(BACKWARD, chnl);
					else
						set_direction_raw(FORWARD, chnl);

					delta_shortest_path = DEGREES_PER_ROTATION -
							      drv_chnls[chnl].position_delta;
				}

				drv_chnls[chnl].target_achieved = is_target_achieved(chnl);

				if (drv_chnls[chnl].target_achieved) {
					// if position_delta is small enough, or large enough that
					// it is actually small "from other side"
					drv_chnls[chnl].target_speed_mrpm = 0;
				} else {
				// Calculate target speed based on the distance from the
				// target position
					int32_t temp = calculate_pid_from_error(delta_shortest_path,
										&PID_pos);

					drv_chnls[chnl].target_speed_for_pos_cntrl =
						(uint32_t)(temp >= 0 ? temp : 0u);

					if (drv_chnls[chnl].target_speed_for_pos_cntrl <
					    MINIMUM_SPEED) {
						drv_chnls[chnl].target_speed_for_pos_cntrl =
							MINIMUM_SPEED;
					}
#if !defined(CONFIG_POS_SOFT_START)
					if (drv_chnls[chnl].target_speed_mrpm == 0) {
					// Initialise speed. If motor is starting, set speed
					// to initial speed.
						drv_chnls[chnl].target_speed_mrpm =
							drv_chnls[chnl].target_speed_for_pos_cntrl;
					} else {
					// Otherwise, if motor is already spinning, then only adjust
					// the speed
#endif

						// If motor is having trouble keeping the speed,
						// or distance has changed
						drv_chnls[chnl].target_speed_mrpm =
						    drv_chnls[chnl].target_speed_mrpm +
						    calculate_pid(
							 drv_chnls[chnl].target_speed_for_pos_cntrl,
							 drv_chnls[chnl].actual_mrpm,
							 &PID_spd_for_pos
						    );
#if !defined(CONFIG_POS_SOFT_START)
					}
#endif

					// Cap the control in range:
					if (drv_chnls[chnl].target_speed_mrpm <
						MINIMUM_SPEED) {
						drv_chnls[chnl].target_speed_mrpm =
							MINIMUM_SPEED;
					}

					if (drv_chnls[chnl].target_speed_mrpm >
						CONFIG_SPEED_MAX_MRPM) {
						drv_chnls[chnl].target_speed_mrpm =
							CONFIG_SPEED_MAX_MRPM;
					}
				}

				ret_debug =
					speed_pwm_set(drv_chnls[chnl].target_speed_mrpm, chnl);
			}
#endif
		}
	}
}
K_WORK_DEFINE(speed_and_position_update_work, update_speed_and_position_continuous);
static void continuous_calculation_timer_handler(struct k_timer *dummy)
{
	k_work_submit(&speed_and_position_update_work);
}
K_TIMER_DEFINE(continuous_calculation_timer, continuous_calculation_timer_handler, NULL);
#pragma endregion TimerWorkCallback

#if defined(CONFIG_POS_CONTROL_ENABLE)
bool is_target_achieved(enum ChannelNumber chnl)
{
	uint32_t delta_shortest_path;

	calculate_pos_delta(chnl, NULL);

	if (drv_chnls[chnl].position_delta < DEGREES_PER_HALF_ROTATION) {
		delta_shortest_path = drv_chnls[chnl].position_delta;
	} else {
		delta_shortest_path = ((int32_t)DEGREES_PER_ROTATION -
					drv_chnls[chnl].position_delta);
	}

	// Range with histeresis is narrower by some percentage, so it can be checked first
	if (delta_shortest_path <=
		((DEGREES_PER_ROTATION * CONFIG_POS_CONTROL_HISTERESIS_PERCENTAGE) /
		(CONFIG_POS_CONTROL_PRECISION_MODIFIER * 100))) {

		return true;
	}

	// Ring between range with histeresis (narrower) and regular range (wider) is accepted only
	// if target was already previously achieved!
	if (drv_chnls[chnl].target_achieved &&
	    (delta_shortest_path <=
			(DEGREES_PER_ROTATION) / (CONFIG_POS_CONTROL_PRECISION_MODIFIER))) {
		return true;
	}

	return false;
}
#endif

// encoder functions
static void enc_callback(enum ChannelNumber chnl, enum PinNumber pin)
{
	if (pin == P0) {
		if (drv_chnls[chnl].prev_val_enc[P0] == drv_chnls[chnl].prev_val_enc[P1]) {
			drv_chnls[chnl].count_cycles--;
		} else {
			drv_chnls[chnl].count_cycles++;
		}
	} else if (pin == P1) {
		if (drv_chnls[chnl].prev_val_enc[P0] == drv_chnls[chnl].prev_val_enc[P1]) {
			drv_chnls[chnl].count_cycles++;
		} else {
			drv_chnls[chnl].count_cycles--;
		}
	}
}

static void enc_callback_wrapper(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	for (int channel = 0; channel < CONFIG_SUPPORTED_CHANNEL_NUMBER; ++channel) {
		for (int pin = 0; pin < PINS_PER_CHANNEL; ++pin) {
			if ((pins & BIT(drv_chnls[channel].enc_pins[pin].pin)) &&
			    dev == drv_chnls[channel].enc_pins[pin].port) {

				drv_chnls[channel].prev_val_enc[pin] =
					gpio_pin_get(dev, drv_chnls[channel].enc_pins[pin].pin);
				enc_callback(channel, pin);
			}
		}
	}
}

// init motor
void init_pwm_motor_driver(void)
{
	for (unsigned int channel = 0; channel < CONFIG_SUPPORTED_CHANNEL_NUMBER; channel++) {

		__ASSERT(device_is_ready(drv_chnls[channel].pwm_motor_driver.dev),
			 "PWM Driver not ready!");

		__ASSERT_EVAL((void) pwm_set_pulse_dt(&(drv_chnls[channel].pwm_motor_driver), 0),
			int r = pwm_set_pulse_dt(&(drv_chnls[channel].pwm_motor_driver), 0),
			!r, "Unable to set PWM!");

		for (uint8_t pin = 0; pin < 2; ++pin) {
			__ASSERT(gpio_is_ready_dt(&(drv_chnls[channel].set_dir_pins[pin])),
				 "Direction Pin %d on channel %d not ready!",
				 pin, channel);

			__ASSERT_EVAL((void) gpio_pin_configure_dt(
							&(drv_chnls[channel].set_dir_pins[pin]),
							GPIO_OUTPUT_LOW),
				int r = gpio_pin_configure_dt(
							&(drv_chnls[channel].set_dir_pins[pin]),
							GPIO_OUTPUT_LOW),
				!r, "Unable to configure direction Pin %d on channel %d!",
				pin, channel);
		}

#if defined(CONFIG_BOARD_NRF52840DONGLE_NRF52840)
		__ASSERT(gpio_is_ready_dt(&out_boot),
			 "GPIO Boot pin not ready!");
#endif

		for (int pin = 0; pin < 2; ++pin) {
			__ASSERT(gpio_is_ready_dt(&(drv_chnls[channel].enc_pins[pin])),
				 "Encoder Pin %d on channel %d not ready!",
				 pin, channel);

			__ASSERT_EVAL((void) gpio_pin_configure_dt(
							&(drv_chnls[channel].enc_pins[pin]),
							GPIO_INPUT),
				int r = gpio_pin_configure_dt(
							&(drv_chnls[channel].enc_pins[pin]),
							GPIO_INPUT),
				!r, "Unable to configure Encoder pin %d on channel %d!",
				pin, channel);

			__ASSERT_EVAL((void) gpio_pin_interrupt_configure_dt(
							&(drv_chnls[channel].enc_pins[pin]),
							GPIO_INT_EDGE_BOTH),
				int r = gpio_pin_interrupt_configure_dt(
							&(drv_chnls[channel].enc_pins[pin]),
							GPIO_INT_EDGE_BOTH),
				!r, "Unable to configure Encoder pin %d on channel %d!",
				pin, channel);

			drv_chnls[channel].prev_val_enc[pin] =
				gpio_pin_get(drv_chnls[channel].enc_pins[pin].port,
							 drv_chnls[channel].enc_pins[pin].pin);

			gpio_init_callback(&(drv_chnls[channel].enc_cb[pin]),
						enc_callback_wrapper,
						BIT(drv_chnls[channel].enc_pins[pin].pin));
			gpio_add_callback(drv_chnls[channel].enc_pins[pin].port,
						&(drv_chnls[channel].enc_cb[pin]));
		}
	}

	k_timer_start(&continuous_calculation_timer, K_MSEC(CONFIG_ENC_TIMER_PERIOD_MS),
		      K_MSEC(CONFIG_ENC_TIMER_PERIOD_MS));

	drv_initialised = true;
}

return_codes_t motor_on(enum MotorDirection direction, enum ChannelNumber chnl)
{
	int ret;

	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	// TODO - change direction during spinning

	if (drv_chnls[chnl].is_motor_on) {
		// Motor is already on, no need for starting it again
		return SUCCESS;
	}

	drv_chnls[chnl].speed_control = 0;

	drv_chnls[chnl].count_cycles = 0;
	drv_chnls[chnl].old_count_cycles = 0;

	ret = set_direction_raw(direction, chnl);

	if (ret != SUCCESS) {
		return ret;
	}

	drv_chnls[chnl].is_motor_on = true;

	return SUCCESS;
}
return_codes_t motor_off(enum ChannelNumber chnl)
{
	int ret;

	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	if (!drv_chnls[chnl].is_motor_on) {
		// Motor is already off, no need for stopping it again
		return SUCCESS;
	}

	if (drv_initialised) {
		ret = gpio_pin_set_dt(&(drv_chnls[chnl].set_dir_pins[P0]), 0);

		if (ret != 0) {
			return ERR_UNABLE_TO_SET_GPIO;
		}
		ret = gpio_pin_set_dt(&(drv_chnls[chnl].set_dir_pins[P1]), 0);
		if (ret != 0) {
			return ERR_UNABLE_TO_SET_GPIO;
		}
		drv_chnls[chnl].is_motor_on = false;
		return SUCCESS;
	}

	return ERR_NOT_INITIALISED;
}

bool get_motor_off_on(enum ChannelNumber chnl)
{
	return drv_chnls[chnl].is_motor_on;
}

static int set_direction_raw(enum MotorDirection direction, enum ChannelNumber chnl)
{
	int ret;

	if (direction == STOPPED) {
		return ERR_INVALID_ARGUMENT;
	}

	ret = gpio_pin_set_dt(&(drv_chnls[chnl].set_dir_pins[P0]), direction == FORWARD);
	if (ret != 0) {
		return ERR_UNABLE_TO_SET_GPIO;
	}

	ret = gpio_pin_set_dt(&(drv_chnls[chnl].set_dir_pins[P1]), direction != FORWARD);
	if (ret != 0) {
		return ERR_UNABLE_TO_SET_GPIO;
	}
	return SUCCESS;
}

int get_motor_actual_direction(enum ChannelNumber chnl, enum MotorDirection *out_dir)
{
	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	*out_dir = drv_chnls[chnl].actual_dir;

	return SUCCESS;
}

static int speed_pwm_set(uint32_t value, enum ChannelNumber chnl)
{
	int ret;

	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	if (value > CONFIG_SPEED_MAX_MRPM) {
		return ERR_DESIRED_VALUE_TO_HIGH;
	}

	if (drv_chnls[chnl].target_speed_mrpm < MINIMUM_SPEED) {
		value = 0;
		drv_chnls[chnl].speed_control = 0;
	}

	uint64_t w_1 = drv_chnls[chnl].pwm_motor_driver.period * (uint64_t)value;
	uint32_t w = value != 0 ? (uint32_t)(w_1 / CONFIG_SPEED_MAX_MRPM) : 0;

	ret = pwm_set_pulse_dt(&(drv_chnls[chnl].pwm_motor_driver), w);
	if (ret != 0) {
		return ERR_UNABLE_TO_SET_PWM;
	}

	return SUCCESS;
}
#if defined(CONFIG_SPEED_CONTROL_ENABLE)
return_codes_t target_speed_set(uint32_t value, enum ChannelNumber chnl)
{
	if (control_mode != SPEED) {
		return ERR_UNSUPPORTED_FUNCTION_IN_CURRENT_MODE;
	}
	drv_chnls[chnl].target_speed_mrpm = value;

	drv_chnls[chnl].count_cycles = 0;
	drv_chnls[chnl].old_count_cycles = 0;

	return SUCCESS;
}
#endif
return_codes_t speed_get(enum ChannelNumber chnl, uint32_t *value)
{
	if (drv_initialised) {
		*value = drv_chnls[chnl].actual_mrpm;
		return SUCCESS;
	}

	return ERR_NOT_INITIALISED;
}
uint32_t speed_target_get(enum ChannelNumber chnl)
{
	return drv_chnls[chnl].target_speed_mrpm;
}
uint32_t get_current_max_speed(void)
{
	return CONFIG_SPEED_MAX_MRPM;
}

#if defined(CONFIG_POS_CONTROL_ENABLE)
return_codes_t target_position_set(int32_t new_target_position, enum ChannelNumber chnl)
{
	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	if (control_mode != POSITION) {
		return ERR_UNSUPPORTED_FUNCTION_IN_CURRENT_MODE;
	}

	drv_chnls[chnl].target_position = WRAP_POS_TO_RANGE(new_target_position);

	drv_chnls[chnl].target_achieved = is_target_achieved(chnl);

	drv_chnls[chnl].target_speed_for_pos_cntrl = 0;
	drv_chnls[chnl].target_speed_mrpm = 0;


	return SUCCESS;
}
#endif

return_codes_t position_get(uint32_t *value, enum ChannelNumber chnl)
{
	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	if (drv_chnls[chnl].curr_pos == DEGREES_PER_ROTATION) {
		// TODO - remove this when continuus update will treat 360 as 0 as it should!
		*value = 0u;
		return SUCCESS;
	}

	*value = drv_chnls[chnl].curr_pos;
	return SUCCESS;
}

return_codes_t mode_set(enum ControlModes new_mode)
{
	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	control_mode = new_mode;
	return SUCCESS;
}
return_codes_t mode_get(enum ControlModes *value)
{
	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	*value = control_mode;
	return SUCCESS;
}

#if defined(CONFIG_POS_CONTROL_ENABLE)
return_codes_t position_reset_zero(enum ChannelNumber chnl)
{
	if (!drv_initialised) {
		return ERR_NOT_INITIALISED;
	}

	if (control_mode != POSITION) {
		return ERR_UNSUPPORTED_FUNCTION_IN_CURRENT_MODE;
	}

	if (drv_chnls[chnl].actual_dir != STOPPED) {
		return ERR_MOTOR_SHOULD_BE_STATIONARY;
	}

	drv_chnls[chnl].curr_pos = 0;
	drv_chnls[chnl].target_position = 0;

	return SUCCESS;
}
#endif

#pragma region DebugFunctions
uint64_t get_cycles_count_DEBUG(void)
{
	return drv_chnls[CH0].target_speed_for_pos_cntrl;
}

uint64_t get_time_cycles_count_DEBUG(void)
{
	return drv_chnls[CH0].target_speed_mrpm;
}

int32_t get_ret_DEBUG(void)
{
	return ret_debug;
}

uint32_t get_calc_speed_DEBUG(void)
{
	return drv_chnls[CH0].speed_control;
}

uint32_t get_target_pos_DEBUG(void)
{
	return drv_chnls[CH0].target_position;
}

int32_t get_pos_d_DEBUG(void)
{
	return drv_chnls[CH0].position_delta;
}
#pragma endregion DebugFunctions

struct DriverVersion get_driver_version(void)
{
	return driver_ver;
}
