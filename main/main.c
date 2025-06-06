#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include <sys/time.h>
#include "driver/i2c.h"
#include "esp_task_wdt.h"
#include "esp_task.h"
#include <string.h>
#include "esp_timer.h"

#include "pwm.h"
#include "i2c.h"
#include "ms5837.h"
#include "get_time.h"
#include "uart.h"
#include "stepper.h"

#define target 2         // 目标深度
#define data_size 10240  // 数据大小
#define stepper_max 1000 // 丝杆极限

float depth_data[data_size] = {0}; // 深度数据
double unix_time[data_size] = {0}; // 对应的时间
int reached_time = 0;              // 到达目标深度的时间
extern TaskHandle_t uart_task_handle;
int data_index = 0;
int steps_moved = 0;
float atmosphere = 0; // 大气压

// 状态机
typedef enum
{
    init,
    dowm,
    keep,
    up,
    report,
    wait
} State;

void user_init()
{
    // 初始化
    i2c_master_init();
    ms5837_reset();
    uart_init();
    stepper_init();

    // 等待开始指令

    printf("all ready\n");
    uart_write_bytes(UART_NUM_1, "all ready", 9);
    stepper_move(100);
    stepper_move(-100);

    while (cmd.unix_time == 0)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    time_sync(cmd.unix_time); // 同步时间
}

void user_code()
{
    ms5837_get_data(&atmosphere, NULL); // 校准大气压
    printf("atmosphere: %f\n", atmosphere);
    char depth_str[32];
    snprintf(depth_str, sizeof(depth_str), "%.2f,%.2f\n", unix_time[0], atmosphere);
    uart_write_bytes(UART_NUM_1, depth_str, strlen(depth_str));

    State state = wait;

    while (1)
    {
        switch (state)
        {
        case init:
            time_sync(cmd.unix_time);
            state = keep;
            break;

        // 下潜
        case dowm:
            ms5837_get_data(&depth_data[data_index], NULL);
            unix_time[data_index] = time(NULL);
            printf("depth: %f\n", depth_data[data_index]);
            printf("time: %f\n", unix_time[data_index]);
            if (steps_moved == 0)
            {
                stepper_move(-200);
                steps_moved -= 200;
            }
            data_index++;
            break;
        // 定深
        case keep:
            if (depth_data[data_index] - atmosphere < target && reached_time == 0)
            {
                reached_time = time(NULL);
                printf("down finished\n");
                // state = keep;
            }
            ms5837_get_data(&depth_data[data_index], NULL);
            unix_time[data_index] = time(NULL);
            printf("depth: %f\n", depth_data[data_index]);
            printf("time: %f\n", unix_time[data_index]);
            if (depth_data[data_index] - atmosphere < target)
            {
                // 下沉限幅
                if (steps_moved <= -300)
                {
                    printf("%d\n", steps_moved);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                else
                {
                    stepper_move(-50);
                    steps_moved -= 50;
                }
            }
            else if (depth_data[data_index] - atmosphere > target)
            {
                // 上浮限幅
                if (steps_moved >= 300)
                {
                    printf("%d\n", steps_moved);
                    vTaskDelay(pdMS_TO_TICKS(250));
                }
                else
                {
                    stepper_move(100);
                    steps_moved += 100;
                }
            }
            // 定深时间
            if (unix_time[data_index] - reached_time > 30)
            {
                reached_time = 0;
                printf("keep,finished\n");
                uart_write_bytes(UART_NUM_1, "keep,finished", 14);
                state = up;
            }
            data_index++;
            vTaskDelay(pdMS_TO_TICKS(10));
            break;

        // 上浮
        case up:
            printf("now,up");
            uart_write_bytes(UART_NUM_1, "now,up", 7);
            printf("depth: %f\n", depth_data[data_index]);
            uart_write_bytes(UART_NUM_1, "depth: ", 7);
            snprintf(depth_str, sizeof(depth_str), "%.2f\n", depth_data[data_index]);
            uart_write_bytes(UART_NUM_1, depth_str, strlen(depth_str));
            printf("time: %f\n", unix_time[data_index]);
            while (steps_moved < 0)
            {
                ms5837_get_data(&depth_data[data_index], NULL);
                unix_time[data_index] = time(NULL);
                ;
                stepper_move(50);
                steps_moved += 50;
                data_index++;
            }
            printf("up,finished");
            uart_write_bytes(UART_NUM_1, "up,finished", 12);
            state = report;
            break;
        // 回传
        case report:
            for (int i = 0; i < data_index; i++)
            {
                printf("Depth: %.2f\n", depth_data[i]);
                printf("time: %f\n", unix_time[i]);
            }

            int last_unixtime = -5;
            // 5s一个数据
            for (int i = 0; i < data_index; i++)
            {
                if (unix_time[i] - last_unixtime >= 5)
                {
                    char depth_str[64] = {0};
                    snprintf(depth_str, sizeof(depth_str), "%.2f,%.2f ", unix_time[i], depth_data[i]);
                    uart_write_bytes(UART_NUM_1, depth_str, strlen(depth_str));
                    last_unixtime = unix_time[i];
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
            data_index = 0;
            cmd.start = 0;
            state = wait;
            break;
        // 待机
        case wait:
            if (cmd.start == 1)
            {
                state = keep;
                printf("start working\n");
                uart_write_bytes(UART_NUM_1, "start working", 13);
            }
            stepper_move(cmd.steps);
            cmd.steps = 0;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main()
{
    stepper_move(-1000);
    user_init();
    // user_code();
}
