#include "producer.h"

static volatile sig_atomic_t running = 1;

void handle_signal(int sig)
{
    printf("Producer received signal %d\n", sig);
    running = 0;
}

int main()
{
    printf("Starting producer process...\n");

    // Set up signal handling
    struct sigaction sa = {0};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Create/open shared memory
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1)
    {
        perror("shm_open");
        return 1;
    }

    // Set the size of the shared memory object
    if (ftruncate(shm_fd, sizeof(shared_data_t)) == -1)
    {
        perror("ftruncate");
        close(shm_fd);
        return 1;
    }

    // Map the shared memory object
    shared_data_t *shared_data = mmap(NULL, sizeof(shared_data_t),
                                      PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shared_data == MAP_FAILED)
    {
        perror("mmap");
        close(shm_fd);
        return 1;
    }

    // Initialize shared data
    memset(shared_data, 0, sizeof(shared_data_t));

    // Create/open semaphore for synchronization
    sem_t *sem = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (sem == SEM_FAILED)
    {
        perror("sem_open");
        munmap(shared_data, sizeof(shared_data_t));
        close(shm_fd);
        return 1;
    }

    printf("Producer initialized, starting data generation...\n");

    while (running)
    {
        int fd = open(DEVICE, O_RDONLY);
        if (fd < 0)
        {
            perror("open " DEVICE);
            usleep(REFRESH_MS * 1000);
            continue;
        }

        mpu6050_sample_float_t sample;

        FILE *f = fdopen(fd, "r");
        if (!f)
        {
            perror("fdopen");
            close(fd);
            usleep(REFRESH_MS * 1000);
            continue;
        }

        char line[128];
        int irq = 0;

        // 1) IRQ line
        if (!fgets(line, sizeof(line), f))
        {
            perror("fgets irq");
            fclose(f);
            usleep(REFRESH_MS * 1000);
            continue;
        }
        (void)sscanf(line, "IRQ count: %d", &irq);

        // 2) Accelerometer: " ax, ay, az, [g]"
        if (!fgets(line, sizeof(line), f) ||
            sscanf(line, " %f , %f , %f",
                   &sample.ax, &sample.ay, &sample.az) != 3)
        {
            fprintf(stderr, "parse accel failed: %s", line);
            fclose(f);
            usleep(REFRESH_MS * 1000);
            continue;
        }

        // 3) Gyroscope: " gx, gy, gz, [dps]"
        if (!fgets(line, sizeof(line), f) ||
            sscanf(line, " %f , %f , %f",
                   &sample.gx, &sample.gy, &sample.gz) != 3)
        {
            fprintf(stderr, "parse gyro failed: %s", line);
            fclose(f);
            usleep(REFRESH_MS * 1000);
            continue;
        }

        // 4) Temperature: " temp, [°C]"
        if (!fgets(line, sizeof(line), f) ||
            sscanf(line, " %f", &sample.temp) != 1)
        {
            fprintf(stderr, "parse temp failed: %s", line);
            fclose(f);
            usleep(REFRESH_MS * 1000);
            continue;
        }

        fclose(f);

        // Process the byte and update shared memory
        sem_wait(sem);

        shared_data->buffer[shared_data->write_index] = sample;
        shared_data->write_index = (shared_data->write_index + 1) % BUFFER_SIZE;
        if (shared_data->count < BUFFER_SIZE)
            shared_data->count++;

        // Compute average over currently filled samples
        // The average of all six axes and temperature
        mpu6050_sample_float_t avg = {0};
        for (size_t i = 0; i < shared_data->count; i++)
        {
            avg.ax += shared_data->buffer[i].ax;
            avg.ay += shared_data->buffer[i].ay;
            avg.az += shared_data->buffer[i].az;
            avg.gx += shared_data->buffer[i].gx;
            avg.gy += shared_data->buffer[i].gy;
            avg.gz += shared_data->buffer[i].gz;
            avg.temp += shared_data->buffer[i].temp;
        }
        if (shared_data->count > 0)
        {
            avg.ax /= shared_data->count;
            avg.ay /= shared_data->count;
            avg.az /= shared_data->count;
            avg.gx /= shared_data->count;
            avg.gy /= shared_data->count;
            avg.gz /= shared_data->count;
            avg.temp /= shared_data->count;
        }
        shared_data->average = avg;

        sem_post(sem);

        // printf("Producer:\nlatest=(%f,%f,%f || %f,%f,%f || %f) count=%zu\n",
        //        sample.ax, sample.ay, sample.az,
        //        sample.gx, sample.gy, sample.gz,
        //        sample.temp, shared_data->count);

        usleep(REFRESH_MS * 1000);
    }

    printf("Producer shutting down...\n");

    sem_close(sem);
    munmap(shared_data, sizeof(shared_data_t));
    close(shm_fd);

    return 0;
}
