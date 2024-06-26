/* SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2023 Maciej Baczmanski, Michal Kawiak, Jakub Mazur
 */
#pragma once

#include <zephyr/kernel.h>
#include "return_codes.h"


/// @brief motor direction definitions
enum MotorDirection {
	STOPPED,
	FORWARD,
	BACKWARD
};

/// @brief Control Mode - whether excact speed or position is controlled
enum ControlModes {
#if defined(CONFIG_SPEED_CONTROL_ENABLE)
	SPEED,
#endif
#if defined(CONFIG_POS_CONTROL_ENABLE)
	POSITION
#endif
};

static const char * const mode_names[] = {
#if defined(CONFIG_SPEED_CONTROL_ENABLE)
	[SPEED] = "Speed",
#endif
#if defined(CONFIG_POS_CONTROL_ENABLE)
	[POSITION] = "Position"
#endif
};

/// @brief struct representing current software version
struct DriverVersion {
	uint8_t major;
	uint8_t minor;
};

enum ChannelNumber {
	CH0,
	CH1
};

// TODO add preprocessor check for max supported channels (currently only CH0 and CH1)

enum PinNumber {
	P0,
	P1
};

/// @brief Function initalising PWMs (drivers) and GPIOs.
/// @param speed_max_mrpm - max speed defined in mili RPM
void init_pwm_motor_driver(void);

/// @brief Set new desired (targeted) speed AND set the pwm
/// @param value - value in mili RPM
/// @return error defined in return_codes.h
return_codes_t target_speed_set(uint32_t value, enum ChannelNumber chnl);

/// @brief Get current actual speed (from encoders)  - TODO - implement with encoders
/// @param value - variable to save speed
/// @return error defined in return_codes.h
return_codes_t speed_get(enum ChannelNumber chnl, uint32_t *value);

/// @brief Turn motor on in proper direction, without modyfing target speed or set pwm output.
/// @param direction - FORWARD or BACKWARD
/// @return error defined in return_codes.h
return_codes_t motor_on(enum MotorDirection direction, enum ChannelNumber chnl);

/// @brief Turn motor off, without modyfing pwm output
/// @return error defined in return_codes.h
return_codes_t motor_off(enum ChannelNumber chnl);

/// @brief Simple getter for max speed (set by init_pwm_motor_driver)
/// @return max speed in mili RPM
uint32_t get_current_max_speed(void);

/// @brief Simple getter for current motor state (off or on)
/// @return true - motor is on; false - motor is off
bool get_motor_off_on(enum ChannelNumber chnl);

/// @brief getter for spinning direction calculated based on encoder feedback
/// @param  channel number for direction
/// @param  output - spinning direction
/// @return code defined in return_codes.h
int get_motor_actual_direction(enum ChannelNumber chnl, enum MotorDirection *out_dir);

/// @brief Simple getter for firmware software version
/// @return DriverVersion struct of major and minor version
struct DriverVersion get_driver_version(void);

/// @brief Simple getter for target speed set with target_speed_set function
/// @return uint32_t representing target speed.
uint32_t speed_target_get(enum ChannelNumber chnl);

/// @brief utility function for converting control mode as string to control mode as proper enum
/// @param str_control_mode control mode as string
/// @param ret_value output - control mode as control mode enum
/// @return error defined in return_codes.h
return_codes_t get_control_mode_from_string(char *str_control_mode, enum ControlModes *ret_value);

/// @brief utility function for converting control mode from enum to string
/// @param control_mode control mode as enum
/// @param ret_value output - control mode as string
/// @return error defined in return_codes.h
return_codes_t get_control_mode_as_string(enum ControlModes control_mode, char **ret_value);

return_codes_t target_position_set(int32_t new_target_position, enum ChannelNumber chnl);

#if defined(CONFIG_POS_CONTROL_ENABLE)
bool is_target_achieved(enum ChannelNumber chnl);
#endif

return_codes_t position_get(uint32_t *value, enum ChannelNumber chnl);

return_codes_t mode_set(enum ControlModes new_mode);

return_codes_t mode_get(enum ControlModes *value);

/// @brief reset zero position to current position.
/// @param chnl - channel, for which should it be reset
/// @return error defined in return_codes.h
return_codes_t position_reset_zero(enum ChannelNumber chnl);

#if defined(CONFIG_BOARD_NRF52840DONGLE_NRF52840)
/// @brief Enter bootloader mode (in order to flash new software via nRF connect programmer)
void enter_boot(void);
#endif

uint64_t get_cycles_count_DEBUG(void);
uint64_t get_time_cycles_count_DEBUG(void);
int32_t get_ret_DEBUG(void);
uint32_t get_calc_speed_DEBUG(void);
uint32_t get_target_pos_DEBUG(void);
int32_t get_pos_d_DEBUG(void);
