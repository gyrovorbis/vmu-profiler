/** \file  vmu_profiler.h
    \brief Multithreaded VMU Profiler

    This file provides a high-level API for managing a VMU profiler
    process which simply does the following in a background thread:

        - Sleep for a configurable interval
        - Wake up and check for a VMU on the given port
        - Query various RAM, VRAM, and SRAM statistics
        - Display the result to the VMU
        - Repeat

    The profiler is meant to be used like so:

        int main(int argc, char* argv[]) {
            // initialize video
            // initialize audio

            vmu_profiler_start(<optional configuration>);

            // Start the game loop
            while(!done) {
                // Update every frame
                vmu_profiler_update(<verts in current frame>)
            }

            vmu_profiler_stop();

            return 0;
        }

    This profiler was originally written in C++20 for the GTA3 project. Feel
    free to use it in your own projects, provided you don't adulterate this
    original comment!

    \author     Falco Girgis
    \copyright  2024 MIT License
 */

#ifndef VMU_PROFILER_H
#define VMU_PROFILER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/** Configuration Parameters

    Optional configuration parameters which an be passed to
    vmu_profiler_start().

    \note
    Leaving any one of these fields as `0`, without exlpicitly giving
    them a value will use the default, built-in value for the given
    field.
 */
typedef struct vmu_profiler_config {
    prio_t       thread_priority;     /**< Priority of the profiler's background thread. */
    unsigned     polling_interval_ms; /**< How long the thread sleeps between each update. */
    unsigned     fps_avg_frames;      /**< How many frames get averaged together to smooth FPS. */
    maple_addr_t maple_port;          /**< Maple port of the VMU to display the profiler on. */
} vmu_profiler_config_t;

bool vmu_profiler_start(const vmu_profiler_config_t* config);
bool vmu_profiler_stop(void);
bool vmu_profiler_update(size_t vert_count);
bool vmu_profiler_running(void);

#ifdef __cplusplus
}
#endif

#endif