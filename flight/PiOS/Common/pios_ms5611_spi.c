/**
 ******************************************************************************
 * @addtogroup PIOS PIOS Core hardware abstraction layer
 * @{
 * @addtogroup PIOS_MS5611 MS5611 Functions
 * @brief Hardware functions to deal with the altitude pressure sensor
 * @{
 *
 * @file       pios_ms5611_spi.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2012.
 * @author     Tau Labs, http://taulabs.org, Copyright (C) 2012-2014
 * @brief      MS5611 Pressure Sensor Routines
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
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

/* Project Includes */
#include "pios.h"

#if defined(PIOS_INCLUDE_MS5611_SPI)

#include "pios_ms5611_priv.h"
#include "pios_semaphore.h"
#include "pios_thread.h"
#include "pios_queue.h"

/* Private constants */
#define PIOS_MS5611_OVERSAMPLING oversampling
#define MS5611_TASK_PRIORITY	PIOS_THREAD_PRIO_HIGHEST
#define MS5611_TASK_STACK_BYTES	512

/* MS5611 Addresses */
#define MS5611_RESET            0x1E
#define MS5611_CALIB_ADDR       0xA2  /* First sample is factory stuff */
#define MS5611_CALIB_LEN        16
#define MS5611_ADC_READ         0x00
#define MS5611_PRES_ADDR        0x40
#define MS5611_TEMP_ADDR        0x50
#define MS5611_ADC_MSB          0xF6
#define MS5611_P0               101.3250f

/* Private methods */
static int32_t PIOS_MS5611_Read(uint8_t address, uint8_t * buffer, uint8_t len);
static int32_t PIOS_MS5611_WriteCommand(uint8_t command);
static void PIOS_MS5611_Task(void *parameters);

/* Private types */

/* Local Types */

enum pios_ms5611_dev_magic {
	PIOS_MS5611_DEV_MAGIC = 0xefba8e10,
};

enum conversion_type {
	PRESSURE_CONV,
	TEMPERATURE_CONV
};

struct ms5611_dev {
	const struct pios_ms5611_cfg *cfg;
	uint32_t spi_id;
	uint32_t slave_num;
	struct pios_thread *task;
	struct pios_queue *queue;

	int64_t pressure_unscaled;
	int64_t temperature_unscaled;
	uint16_t calibration[6];
	enum conversion_type current_conversion_type;
	enum pios_ms5611_dev_magic magic;

	struct pios_semaphore *busy;
};

static struct ms5611_dev *dev;

/**
 * @brief Allocate a new device
 */
static struct ms5611_dev * PIOS_MS5611_alloc(void)
{
	struct ms5611_dev *ms5611_dev;

	ms5611_dev = (struct ms5611_dev *)PIOS_malloc(sizeof(*ms5611_dev));
	if (!ms5611_dev)
		return (NULL);

	memset(ms5611_dev, 0, sizeof(*ms5611_dev));

	ms5611_dev->queue = PIOS_Queue_Create(1, sizeof(struct pios_sensor_baro_data));
	if (ms5611_dev->queue == NULL) {
		PIOS_free(ms5611_dev);
		return NULL;
	}

	ms5611_dev->magic = PIOS_MS5611_DEV_MAGIC;

	ms5611_dev->busy = PIOS_Semaphore_Create();
	PIOS_Assert(ms5611_dev->busy != NULL);

	return ms5611_dev;
}

/**
 * @brief Validate the handle to the spi device
 * @returns 0 for valid device or <0 otherwise
 */
static int32_t PIOS_MS5611_Validate(struct ms5611_dev *dev)
{
	if (dev == NULL)
		return -1;
	if (dev->magic != PIOS_MS5611_DEV_MAGIC)
		return -2;
	if (dev->spi_id == 0)
		return -3;
	return 0;
}

/**
 * Initialise the MS5611 sensor
 */
int32_t PIOS_MS5611_SPI_Init(uint32_t spi_id, uint32_t slave_num, const struct pios_ms5611_cfg *cfg)
{
	dev = (struct ms5611_dev *)PIOS_MS5611_alloc();
	if (dev == NULL)
		return -1;

	dev->spi_id = spi_id;
	dev->slave_num = slave_num;

	dev->cfg = cfg;

	if (PIOS_MS5611_WriteCommand(MS5611_RESET) != 0)
		return -2;

	PIOS_DELAY_WaitmS(20);

	uint8_t data[2];

	/* Calibration parameters */
	for (int i = 0; i < NELEMENTS(dev->calibration); i++) {
		PIOS_MS5611_Read(MS5611_CALIB_ADDR + i * 2, data, 2);
		dev->calibration[i] = (data[0] << 8) | data[1];
	}

	PIOS_SENSORS_Register(PIOS_SENSOR_BARO, dev->queue);

	dev->task = PIOS_Thread_Create(
			PIOS_MS5611_Task, "pios_ms5611", MS5611_TASK_STACK_BYTES, NULL, MS5611_TASK_PRIORITY);
	PIOS_Assert(dev->task != NULL);

	return 0;
}

/**
 * Claim the MS5611 device semaphore
 * \return 0 if no error
 * \return -1 if timeout before claiming semaphore
 */
static int32_t PIOS_MS5611_ClaimDevice(void)
{
	PIOS_Assert(PIOS_MS5611_Validate(dev) == 0);

	return PIOS_Semaphore_Take(dev->busy, PIOS_SEMAPHORE_TIMEOUT_MAX) == true ? 0 : 1;
}

/**
 * @brief Release the SPI bus for the baro communications and end the transaction
 * @return 0 if successful
 */
static int32_t PIOS_MS5611_ReleaseDevice(void)
{
	PIOS_Assert(PIOS_MS5611_Validate(dev) == 0);

	return PIOS_Semaphore_Give(dev->busy) == true ? 0 : 1;
}

/**
 * @brief Claim the SPI bus for the baro communications and select this chip
 * @return 0 if successful, -1 for invalid device, -2 if unable to claim bus
 */
static int32_t PIOS_MS5611_ClaimBus(void)
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;

	if (PIOS_SPI_ClaimBus(dev->spi_id) != 0)
		return -2;

	PIOS_SPI_RC_PinSet(dev->spi_id, dev->slave_num, 0);

	return 0;
}

/**
 * @brief Release the SPI bus for the baro communications and end the transaction
 * @return 0 if successful
 */
static int32_t PIOS_MS5611_ReleaseBus(void)
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;

	PIOS_SPI_RC_PinSet(dev->spi_id, dev->slave_num, 1);

	return PIOS_SPI_ReleaseBus(dev->spi_id);
}

/**
* Start the ADC conversion
* \param[in] PRESSURE_CONV or TEMPERATURE_CONV to select which measurement to make
* \return 0 for success, -1 for failure (conversion completed and not read)
*/
static int32_t PIOS_MS5611_StartADC(enum conversion_type type)
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;

	/* Start the conversion */
	switch (type) {
	case TEMPERATURE_CONV:
		while (PIOS_MS5611_WriteCommand(MS5611_TEMP_ADDR + dev->cfg->oversampling) != 0)
			continue;
		break;
	case PRESSURE_CONV:
		while (PIOS_MS5611_WriteCommand(MS5611_PRES_ADDR + dev->cfg->oversampling) != 0)
			continue;
		break;
	default:
		return -1;
	}

	dev->current_conversion_type = type;

	return 0;
}

/**
 * @brief Return the delay for the current osr
 */
static int32_t PIOS_MS5611_GetDelay()
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return 100;

	switch(dev->cfg->oversampling) {
	case MS5611_OSR_256:
		return 2;
	case MS5611_OSR_512:
		return 2;
	case MS5611_OSR_1024:
		return 3;
	case MS5611_OSR_2048:
		return 5;
	case MS5611_OSR_4096:
		return 10;
	default:
		break;
	}
	return 10;
}

/**
* Read the ADC conversion value (once ADC conversion has completed)
* \return 0 if successfully read the ADC, -1 if failed
*/
static int32_t PIOS_MS5611_ReadADC(void)
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;

	uint8_t data[3];

	static int64_t delta_temp;
	static int64_t temperature;

	/* Read and store the 16bit result */
	if (dev->current_conversion_type == TEMPERATURE_CONV) {
		uint32_t raw_temperature;
		/* Read the temperature conversion */
		if (PIOS_MS5611_Read(MS5611_ADC_READ, data, 3) != 0)
			return -1;

		raw_temperature = (data[0] << 16) | (data[1] << 8) | data[2];

		delta_temp = (int32_t)raw_temperature - (dev->calibration[4] << 8);
		temperature = 2000 + ((delta_temp * dev->calibration[5]) >> 23);
		dev->temperature_unscaled = temperature;

		// second order temperature compensation
		if (temperature < 2000)
			dev->temperature_unscaled -= (delta_temp * delta_temp) >> 31;

	} else {
		int64_t offset;
		int64_t sens;
		uint32_t raw_pressure;

		/* Read the pressure conversion */
		if (PIOS_MS5611_Read(MS5611_ADC_READ, data, 3) != 0)
			return -1;

		raw_pressure = (data[0] << 16) | (data[1] << 8) | (data[2] << 0);

		offset = ((int64_t)dev->calibration[1] << 16) + (((int64_t)dev->calibration[3] * delta_temp) >> 7);
		sens = (int64_t)dev->calibration[0] << 15;
		sens = sens + ((((int64_t) dev->calibration[2]) * delta_temp) >> 8);

		// second order temperature compensation
		if (temperature < 2000) {
			offset -= (5 * (temperature - 2000) * (temperature - 2000)) >> 1;
			sens -= (5 * (temperature - 2000) * (temperature - 2000)) >> 2;

			if (dev->temperature_unscaled < -1500) {
				offset -= 7 * (temperature + 1500) * (temperature + 1500);
				sens -= (11 * (temperature + 1500) * (temperature + 1500)) >> 1;
			}
		}

		dev->pressure_unscaled = ((((int64_t)raw_pressure * sens) >> 21) - offset) >> 15;
	}
	return 0;
}

/**
* Reads one or more bytes into a buffer
* \param[in] the command indicating the address to read
* \param[out] buffer destination buffer
* \param[in] len number of bytes which should be read
* \return 0 if operation was successful
* \return -1 if dev is invalid
* \return -2 if failed to claim SPI bus
* \return -3 if error during SPI transfer
*/
static int32_t PIOS_MS5611_Read(uint8_t address, uint8_t *buffer, uint8_t len)
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;

	if (PIOS_MS5611_ClaimBus() != 0)
		return -2;

	int32_t rc;

	PIOS_SPI_TransferByte(dev->spi_id, address);

	if (PIOS_SPI_TransferBlock(dev->spi_id, NULL, buffer, len) < 0) {
		rc = -3;
		goto out;
	}

	rc = 0;

out:
	PIOS_MS5611_ReleaseBus();

	return rc;
}

/**
* Writes one or more bytes to the MS5611
* \param[in] address Register address
* \param[in] buffer source buffer
* \return 0 if operation was successful
* \return -1 if dev is invalid
* \return -2 if failed to claim SPI bus
* \return -3 if error during SPI transfer
*/
static int32_t PIOS_MS5611_WriteCommand(uint8_t command)
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;

	if (PIOS_MS5611_ClaimBus() != 0)
		return -2;

	PIOS_SPI_TransferByte(dev->spi_id, command);

	PIOS_MS5611_ReleaseBus();

	return 0;
}

/**
* @brief Run self-test operation.
* \return 0 if self-test succeed, -1 if failed
*/
int32_t PIOS_MS5611_SPI_Test()
{
	if (PIOS_MS5611_Validate(dev) != 0)
		return -1;


	PIOS_MS5611_ClaimDevice();
	PIOS_MS5611_StartADC(TEMPERATURE_CONV);
	PIOS_DELAY_WaitmS(PIOS_MS5611_GetDelay());
	PIOS_MS5611_ReadADC();
	PIOS_MS5611_ReleaseDevice();

	PIOS_MS5611_ClaimDevice();
	PIOS_MS5611_StartADC(PRESSURE_CONV);
	PIOS_DELAY_WaitmS(PIOS_MS5611_GetDelay());
	PIOS_MS5611_ReadADC();
	PIOS_MS5611_ReleaseDevice();


	// check range for sanity according to datasheet
	if (dev->temperature_unscaled < -4000 ||
		dev->temperature_unscaled > 8500 ||
		dev->pressure_unscaled < 1000 ||
		dev->pressure_unscaled > 120000)
		return -1;

	return 0;
}

static void PIOS_MS5611_Task(void *parameters)
{
	// init this to 1 in order to force a temperature read on the first run
	uint32_t temp_press_interleave_count = 1;
	int32_t  read_adc_result = 0;

	while (1) {

		--temp_press_interleave_count;

		if (temp_press_interleave_count == 0)
		{
			// Update the temperature data
			PIOS_MS5611_ClaimDevice();
			PIOS_MS5611_StartADC(TEMPERATURE_CONV);
			PIOS_Thread_Sleep(PIOS_MS5611_GetDelay());
			PIOS_MS5611_ReadADC();
			PIOS_MS5611_ReleaseDevice();

			temp_press_interleave_count = dev->cfg->temperature_interleaving;
			if (temp_press_interleave_count == 0)
				temp_press_interleave_count = 1;
		}

		// Update the pressure data
		PIOS_MS5611_ClaimDevice();
		PIOS_MS5611_StartADC(PRESSURE_CONV);
		PIOS_Thread_Sleep(PIOS_MS5611_GetDelay());
		read_adc_result = PIOS_MS5611_ReadADC();
		PIOS_MS5611_ReleaseDevice();

		// Compute the altitude from the pressure and temperature and send it out
		struct pios_sensor_baro_data data;
		data.temperature = ((float) dev->temperature_unscaled) / 100.0f;
		data.pressure = ((float) dev->pressure_unscaled) / 1000.0f;
		data.altitude = 44330.0f * (1.0f - powf(data.pressure / MS5611_P0, (1.0f / 5.255f)));


		if (read_adc_result == 0) {
			PIOS_Queue_Send(dev->queue, &data, 0);
		}
	}
}


#endif

/**
 * @}
 * @}
 */
