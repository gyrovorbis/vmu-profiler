/*  vmu_profiler.c

    Copyright (C) 2024 Falco Girgis
 */

#include "vmu_profiler.h"

#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

#include <sys/malloc.h>

#include <kos/thread.h>
#include <kos/rwsem.h>

#include <arch/arch.h>

#include <dc/sound/sound.h>
#include <dc/maple.h>
#include <dc/pvr.h>
#include <dc/vmu_fb.h>

#define VMU_PROFILER_THREAD_PRIO_DEFAULT_	PRIO_DEFAULT
#define VMU_PROFILER_POLL_INT_DEFAULT_		200
#define VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_ 	20
#define VMU_PROFILER_MAPLE_PORT_DEFAULT_	0

#define VMU_PROFILER_THD_STACK_SIZE_		1024
#define VMU_PROFILER_THD_LABEL_                 "VmuProfiler"

#define MB_(b)                                  ((b) * 1024 * 1024)

typedef struct vmu_profiler {
    vmu_profiler_config_t config;

    kthread_t* thread;
    rw_semaphore_t rwsem;
    atomic_bool done;

    volatile size_t verts;

    unsigned fps_frame;
    float fps_frames[];
} vmu_profiler_t;

static vmu_profiler_t* profiler_ = NULL;

static size_t ram_used_(void) {
    size_t total_ram = 0;

    // Heap
    total_ram += mallinfo().uordblks;
    // Main Stack

    return total_ram;
}

static void *vmu_profiler_run_(void *arg) {
    vmu_profiler_t* self    = arg;
    maple_device_t* dev     = NULL;
    bool            success = true;

    while(!self->done) {
        thd_sleep(self->config.polling_interval_ms);

        if(!(dev = maple_enum_type(0, MAPLE_FUNC_MEMCARD))
            continue;

        struct mallinfo ram_stats  = ram_used_();
        size_t          vram_stats = pvr_mem_available();
        uint32_t        sram_stats = snd_mem_available();

        float fps = 0.0f;
        for(unsigned f = 0; f < self->config.fps_avg_frames; ++f)
            fps += self->fps_frames[f];

        float sh4_mem = (HW_MEMSIZE - ram_stats)  / (float)HW_MEMSIZE * 100.0f;
        float pvr_mem = (MB_(8)     - vram_stats) / (float)MB_(8)     * 100.0f;
        float arm_mem = (MB_(2)     - sram_stats) / (float)MB_(2)     * 100.0f;

        if(rwsem_read_lock(&profiler_->rwsem) < 0) {
            fprintf(stderr, "vmu_profiler_run(): RWSEM lock failed: [%s]!\n",
                    strerror(errno));
            success = false;
            continue;
        }

        vmu_printf("FPS : %.2f\n"
                   "SH4 : %.2f%%\n"
                   "PVR : %.2f%%\n"
                   "ARM : %.2f%%\n"
                   "VTX : %lu",
                   fps, sh4_mem, pvr_mem,
                   arm_mem, self->verts);

        if(rwsem_read_unlock(&self->rwsem) < 0) {
            fprintf(stderr, "vmu_profiler_run(): RWSEM unlock failed: [%s]!\n",
                    strerror(errno));
            success = false;
            continue;
        }
    }

    return (void*)success;
}

bool vmu_profiler_start(const vmu_profiler_config_t* config) {
    static const vmu_profiler_config_t default_config = {
        .thread_priority     = VMU_PROFILER_THREAD_PRIO_DEFAULT_,
        .polling_interval_ms = VMU_PROFILER_POLL_INT_DEFAULT_,
        .fps_avg_frames      = VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_,
        .maple_port 	     = VMU_PROFILER_MAPLE_PORT_DEFAULT_
    };

    bool            success  = true;
    vmu_profiler_t* profiler = NULL;

    if(vmu_profiler_running()) {
        fprintf(stderr, "vmu_profiler_start(): Profiler already running!\n");
        success = false;
        goto done;
    } else
        printf("vmu_profiler_start(): Launching profiler thread.\n");

    unsigned fps_frames =
        (config && config->fps_avg_frames)?
            config->fps_avg_frames : default_config.fps_avg_frames;

    profiler = calloc(sizeof(vmu_profiler_t) + sizeof(float) * fps_avg_frames);

    if(!profiler) {
        fprintf(stderr, "\tVMU Profiler failed to allocate!\n");
        success = false;
        goto done;
    }

    memcpy(&profiler->config, &default_config, sizeof(vmu_profiler_config_t));

    if(config) {
        if(config->thread_priority)
            profiler->config.thread_priority = config->thread_priority;

        if(config->polling_interval_ms)
            profiler->config.polling_interval_ms = config->polling_interval_ms;

        if(config->fps_avg_frames)
            profiler->config.fps_avg_frames = config->fps_avg_frames;

        if(config->maple_port)
            profiler->config.maple_port = config->maple_port;
    }

    if(rwsem_init(&profiler->rwsem) < 0) {
        fprintf(stderr, "\tCould not initialize RW semaphore!");
        success = false;
        goto dealloc;
    }

    const kthread_attr_t attr = {
        .create_detached = false,
        .stack_size      = VMU_PROFILER_THD_STACK_SIZE_,
        .prio 	         = profiler_->config.priority,
        .label           = VMU_PROFILER_THD_LABEL_,
    };

    profiler->thread = thd_create_ex(&attr, vmu_profiler_run_, profiler);

    if(!profiler->thread) {
        fprintf(stderr, "\tFailed to spawn profiler thread!\n");
        success = false;
        goto deinit_sem;
    }

    goto done;

deinit_sem:
    rwsem_destroy(&profiler->rwsem);
dealloc:
    free(profiler);
    profiler = NULL;
done:
    profiler_ = profiler;
    return success;
}

bool vmu_profiler_stop(void) {
    bool success = true;

    if(!vmu_profiler_running()) {
        fprintf(stderr, "vmu_profiler_stop(): Profiler isn't running!\n");
        return false;
    }

    printf("vmu_profiler_stop(): Stopping profiler!\n");

    profiler_->done = true;

    bool join_value = true;
    if(thd_join(profiler_->thread, (void**)&join_value) < 0) {
        fprintf(stderr, "\tFailed to join thread!\n");
        return false;
    }

    if(!join_value) {
        fprintf(stderr, "\tThread had some issues!\n");
        susccess = false;
    }

    if(rwsem_destroy(&profiler_->rwsem) < 0) {
        fprintf(stderr, "\tFailed to destroy rwsem: [%s]\n", strerror(errno));
        success = false;
    }

    free(_profiler);
    _profiler = NULL;
 
    return success;
}

bool vmu_profiler_running(void) {
    return !!profiler_;
}

bool vmu_profiler_update(size_t vert_count) {
    if(!vmu_profiler_running())
        return false;

    if(!rwsem_write_lock(&profile_->rwsem) < 0) {
        fprintf(stderr, "vmu_profiler_update(): Failed to write rwlock: [%s]\n",
                strerror(errno));
        return false;
    }

    pvr_stats_t pvr_stats;
    pvr_get_stats(&pvr_stats);
    
    profiler_->fps_frames[profiler_->fps_frame++] = pvr_stats.frame_rate;

    if(profiler_->fps_frame >= profiler_->config.fps_avg_frames)
        profiler_->fps_frame = 0;

    profiler_->verts = vert_count;

    if(!rwsem_write_unlock(&profile_->rwsem) < 0) {
        fprintf(stderr, "vmu_profiler_update(): Failed to unlock rwlock: [%s]\n",
                strerror(errno));
        return false;
    }

    return true;
}
