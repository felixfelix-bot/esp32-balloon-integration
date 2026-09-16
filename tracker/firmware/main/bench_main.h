/**
 * @file    bench_main.h
 * @brief   Entry point of the ESP32-C3 BENCH console application (HARM-T4).
 *
 * The implementation is `bench_main.cpp`, compiled into the tracker firmware
 * target but active only when `CONFIG_BENCH_CONSOLE=y` (Kconfig, default n).
 * `app_main()` calls bench_console_task_start() and never returns: the bench
 * console takes over the board (it is not a flight application).
 */

#ifndef ESP32_BENCH_MAIN_H
#define ESP32_BENCH_MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up the LR2021 + USB-CDC BENCH console and start its tasks.
 *
 * Runs the console state machine from `components/bench` (spec
 * BENCH-CONSOLE-SPEC v1.0) with the RadioLib LR2021 glue in bench_main.cpp.
 * Callable from app_main(); blocks the caller forever (the console owns the
 * board — no tracker tasks are started).
 */
void bench_console_task_start(void);

#ifdef __cplusplus
}
#endif

#endif /* ESP32_BENCH_MAIN_H */
