/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup   PIOS_SERVO Servo Functions
 * @brief PIOS interface to read and write from servo PWM ports
 * @{
 *
 * @file       pios_servo_priv.h
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Servo private structures.
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>
 */

#ifndef PIOS_PWM_PRIV_H
#define PIOS_PWM_PRIV_H

#include <pios.h>
#include <pios_stm32.h>

#include <pios_tim_priv.h>

struct pios_pwm_cfg {
	TIM_ICInitTypeDef tim_ic_init;
	const struct pios_tim_channel * channels;
	uint8_t num_channels;
};

extern const struct pios_rcvr_driver pios_pwm_rcvr_driver;

extern int32_t PIOS_PWM_Init(uintptr_t * pwm_id, const struct pios_pwm_cfg * cfg);

#endif /* PIOS_PWM_PRIV_H */

/**
 * @}
 * @}
 */
