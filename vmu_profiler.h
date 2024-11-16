#ifndef VMU_PROFILER_H
#define VMU_PROFILER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

typedef struct vmu_profiler_config {
	prio_t 	 	 thread_priority;
    unsigned 	 polling_interval_ms;
    unsigned 	 fps_avg_frames;
    maple_addr_t maple_port;
} vmu_profiler_config_t;

bool vmu_profiler_start(const vmu_profiler_config_t *config);
bool vmu_profiler_stop(void);
bool vmu_profiler_update(size_t vert_count);

bool vmu_profiler_running(void);

float vmu_profiler_ram (void);
float vmu_profiler_vram(void);
float vmu_profiler_sram(void);
size_t vmu_profiler_verts(void);

#ifdef __cplusplus
}
#endif

#endif