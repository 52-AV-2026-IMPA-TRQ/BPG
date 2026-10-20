#pragma once

#include "driver/gpio.h"

/*
 * Pinout fijo del adaptador Start Torque.
 *
 * Esta asignacion corresponde al esquema actual en Proteus.
 * No modificar sin revisar simultaneamente el esquema electrico,
 * la placa fisica y la documentacion del banco.
 */

/* Entradas digitales con pull-up externo de 10 kohm */
#define BPG_GPIO_FC1_SECURITY       GPIO_NUM_34
#define BPG_GPIO_FC2_TORQUE         GPIO_NUM_35

/* Pulsadores con pull-up interno del ESP32 */
#define BPG_GPIO_START              GPIO_NUM_32
#define BPG_GPIO_STOP_RESET         GPIO_NUM_33
#define BPG_GPIO_UP                 GPIO_NUM_25
#define BPG_GPIO_DOWN               GPIO_NUM_26
#define BPG_GPIO_ENTER              GPIO_NUM_27

/* Bus I2C mediante adaptador bidireccional 3,3 V / 5 V */
#define BPG_GPIO_I2C_SCL            GPIO_NUM_16
#define BPG_GPIO_I2C_SDA            GPIO_NUM_17

/* Salidas hacia buffer 74LS245 / 54LS245 */
#define BPG_GPIO_PWM                GPIO_NUM_5
#define BPG_GPIO_READY_LED          GPIO_NUM_18
#define BPG_GPIO_CONTACTOR          GPIO_NUM_19
#define BPG_GPIO_TEST_LED           GPIO_NUM_21
#define BPG_GPIO_FAULT_LED          GPIO_NUM_23

/*
 * Convencion inicial de entradas.
 * Debe confirmarse posteriormente contra el circuito electrico real.
 */
#define BPG_INPUT_ACTIVE_LEVEL      0
#define BPG_INPUT_INACTIVE_LEVEL    1

/*
 * Estados logicos seguros provisorios.
 * La polaridad efectiva se validara antes de conectar potencia.
 */
#define BPG_OUTPUT_OFF_LEVEL        0
#define BPG_OUTPUT_ON_LEVEL         1
#define BPG_PWM_SAFE_DUTY           0

/*
 * Caracteristicas importantes:
 *
 * 1. GPIO34 y GPIO35 son exclusivamente entradas.
 * 2. GPIO34 y GPIO35 requieren pull-up externo.
 * 3. GPIO5 se reserva exclusivamente para PWM.
 * 4. GPIO5 debe permanecer sin energia aplicada durante el arranque.
 * 5. No existe medicion analogica directa de VBAT en esta placa.
 */
