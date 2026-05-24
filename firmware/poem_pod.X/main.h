/* 
 * File:   main.h
 * Author: e.togashi
 *
 * Created on April 27, 2026, 3:18 PM
 */

#ifndef MAIN_H
#define	MAIN_H

#ifdef	__cplusplus
extern "C" {
#endif
    
static void runHttpServerMode(void);
static void runMqttClientMode(void);
static void runI2cClockDummy(uint16_t freq_khz);

//以下はW5500コールバック関数**************************
static void spi_cris_enter(void);

static void spi_cris_exit(void);

static void spi_cs_select(void);

static void spi_cs_deselect(void);

static uint8_t spi_read_byte(void);

static void spi_write_byte(uint8_t data);

static void spi_read_burst(uint8_t* pBuf, uint16_t len);

static void spi_write_burst(uint8_t* pBuf, uint16_t len);


#ifdef	__cplusplus
}
#endif

#endif	/* MAIN_H */

