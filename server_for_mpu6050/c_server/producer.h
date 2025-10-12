#ifndef PRODUCER_H
#define PRODUCER_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <stdint.h>
#include <sys/types.h>

#include "../../06_I2C_w_IRQ_MPU6050/mpu6050_lib.h"

#define BUFFER_SIZE 16
#define REFRESH_MS 100
#define DEVICE "/dev/mpu6050"
#define SHM_NAME "/data_buffer"
#define SEM_NAME "/data_sem"

typedef struct
{
    mpu6050_sample_float_t buffer[BUFFER_SIZE];
    mpu6050_sample_float_t average;
    size_t count;
    size_t write_index;
} shared_data_t;

#endif // PRODUCER_H
