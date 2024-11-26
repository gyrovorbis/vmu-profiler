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

#include <kos.h>
#include <stdbool.h>
#include <stdint.h>

#define VMU_PROFILER_MAX_MEASURES   5

struct vmu_profiler;
struct vmu_profiler_measurement;

/** Configuration Parameters

    Optional configuration parameters which can be passed to
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
    unsigned maple_port;          /**< Maple port of the VMU to display the profiler on. */
} vmu_profiler_config_t;

enum measure_type {
    use_float,
    use_unsigned,
    use_string,
    INVALID
};

typedef struct vmu_profiler_measurement {
    // 4 chars or less
    char *disp_name;
    enum measure_type m;
    //
    union {
        float fstorage;
        unsigned ustorage;
    };
    char sstorage[16];
    // callback to produce value and store in *storage
    void (*generate_value)(struct vmu_profiler_measurement *m, struct vmu_profiler *p); 
} vmu_profiler_measurement_t;

typedef struct vmu_profiler
{
    vmu_profiler_config_t config;

    kthread_t *thread;
    rw_semaphore_t rwsem;
    atomic_bool done;

    int measure_count;
    struct vmu_profiler_measurement *measures[VMU_PROFILER_MAX_MEASURES];

    unsigned fps_frame;
    float fps_frames[];
} vmu_profiler_t;

int vmu_profiler_start(const vmu_profiler_config_t *config, void (*setup_func)(struct vmu_profiler *p));
int vmu_profiler_stop(void);
int vmu_profiler_update(void);
int vmu_profiler_running(void);

vmu_profiler_measurement_t *init_measurement(char *name, enum measure_type m, void (*callback)(vmu_profiler_measurement_t *m, vmu_profiler_t *p));
void vmu_profiler_add_measure(vmu_profiler_t *prof, vmu_profiler_measurement_t *measure);

#ifdef __cplusplus
}
#endif

#endif
