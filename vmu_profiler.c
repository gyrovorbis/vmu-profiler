/*  vmu_profiler.c

    Copyright (C) 2024 Falco Girgis
 */

#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

#include <errno.h>
// #include <sys/malloc.h>

#include <kos/thread.h>
#include <kos/rwsem.h>

#include <arch/arch.h>

#include <dc/sound/sound.h>
#include <dc/maple.h>
#include <dc/pvr.h>
#include <dc/vmu_fb.h>

#include "vmu_profiler.h"

#define VMU_PROFILER_THREAD_PRIO_DEFAULT_ PRIO_DEFAULT
#define VMU_PROFILER_POLL_INT_DEFAULT_ 200
#define VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_ 20
#define VMU_PROFILER_MAPLE_PORT_DEFAULT_ 0

#define VMU_PROFILER_THD_STACK_SIZE_ 8192
#define VMU_PROFILER_THD_LABEL_ "VmuProfiler"

#define MB_(b) ((b) * 1024 * 1024)

extern int xform_verts;
extern int subd_verts;
extern int xform_polys;

int copied_xv;
int copied_sv;
int copied_xp;

static vmu_profiler_t *profiler_ = NULL;

void vmu_profiler_add_measure(vmu_profiler_t *prof, vmu_profiler_measurement_t *measure)
{
    if (prof->measure_count < 5) {
        prof->measures[prof->measure_count++] = measure;
    }
}

vmu_profiler_measurement_t *init_measurement(char *name, enum measure_type m, void (*callback)(vmu_profiler_measurement_t *m, vmu_profiler_t *p))
{
    vmu_profiler_measurement_t *measure = (vmu_profiler_measurement_t *)malloc(sizeof(vmu_profiler_measurement_t));

    measure->disp_name = name;
    measure->m = m;
    measure->generate_value = callback;

    return measure;
}

void update_fps(vmu_profiler_measurement_t *m, vmu_profiler_t *p)
{
    float fps = 0.0f;

    for (unsigned f = 0; f < p->config.fps_avg_frames; ++f)
        fps += p->fps_frames[f];

    fps /= (float)p->config.fps_avg_frames;

    m->fstorage = fps;
}

void update_transformed_verts(vmu_profiler_measurement_t *m, vmu_profiler_t *p)
{
    m->ustorage = (size_t)xform_verts;
}

void update_submitted_verts(vmu_profiler_measurement_t *m, vmu_profiler_t *p)
{
    m->ustorage = (size_t)subd_verts;
}

void update_transformed_polys(vmu_profiler_measurement_t *m, vmu_profiler_t *p)
{
    m->ustorage = (size_t)xform_polys;
}

void update_pvr_ram(vmu_profiler_measurement_t *m, vmu_profiler_t *p)
{
    size_t vram_stats = pvr_mem_available();

    float pvr_mem = (MB_(8) - vram_stats) / (float)MB_(8) * 100.0f;

    m->fstorage = pvr_mem;
}

void update_some_string(vmu_profiler_measurement_t *m, vmu_profiler_t *p)
{
    sprintf(m->sstorage, "!TESTSTRING?");
}

void setup_my_measures(void)
{
    vmu_profiler_measurement_t *fps_msr = init_measurement("FPS", use_float, update_fps);
    vmu_profiler_measurement_t *pvr_msr = init_measurement("PVR", use_float, update_pvr_ram);
    vmu_profiler_measurement_t *xp_msr = init_measurement("POLY", use_unsigned, update_transformed_polys);
    vmu_profiler_measurement_t *xv_msr = init_measurement("TVRT", use_unsigned, update_transformed_verts);
    vmu_profiler_measurement_t *sv_msr = init_measurement("SVRT", use_unsigned, update_submitted_verts);
    // vmu_profiler_measurement_t *str_msr = init_measurement("ADDR", use_string, update_some_string);

    // vmu_profiler_add_measure(str_msr);
    vmu_profiler_add_measure(profiler_, xp_msr);
    vmu_profiler_add_measure(profiler_, xv_msr);
    vmu_profiler_add_measure(profiler_, sv_msr);
    vmu_profiler_add_measure(profiler_, pvr_msr);
    vmu_profiler_add_measure(profiler_, fps_msr);
}

//char tmp_vstr[32];
// generic to_string for all measure types
// for number types
//  display name is right-padded / left-justified 4 characters long
//  separator is always 5th character followed by a space
//  values are left-padded / right-justified 5 characters long
// for string types
//  return raw sstring as-is
char *to_string(vmu_profiler_measurement_t *measure, int i, int call)
{
    // int value_len = 0;
    switch (measure->m)
    {
    case use_float:
        // sprintf(tmp_vstr, "%5.2f", measure->fstorage);
        // value_len = strlen(tmp_vstr);
        if (!i)
            sprintf(measure->sstorage, "%-4s: %5.2f", measure->disp_name, measure->fstorage);
        else
            sprintf(measure->sstorage, "\n%-4s: %5.2f", measure->disp_name, measure->fstorage);
        break;
    case use_unsigned:
        // sprintf(tmp_vstr, "%u", measure->ustorage);
        // value_len = strlen(tmp_vstr);
        if (!i)
            sprintf(measure->sstorage, "%-4s: %5u", measure->disp_name, measure->ustorage);
        else
            sprintf(measure->sstorage, "\n%-4s: %5u", measure->disp_name, measure->ustorage);
        break;
    case use_string:
    default:
        break;
    }
    return measure->sstorage;
}

char pfstr[1024];

static void *vmu_profiler_run_(void *arg)
{
    vmu_profiler_t *self = arg;
    int success = true;
    int runs = 0;

    while (!self->done)
    {
        thd_sleep(self->config.polling_interval_ms);
        pfstr[0] = 0;
        for (int i = 0; i < profiler_->measure_count; i++)
        {
            strcat(pfstr, to_string(profiler_->measures[i], i, runs++));
        }

        if (rwsem_read_lock(&profiler_->rwsem) < 0)
        {
            dbgio_printf("vmu_profiler_run(): RWSEM lock failed: [%s]!\n",
                         strerror(errno));
            success = false;
            continue;
        }

        vmu_printf(pfstr);

        if (rwsem_read_unlock(&self->rwsem) < 0)
        {
            dbgio_printf("vmu_profiler_run(): RWSEM unlock failed: [%s]!\n",
                         strerror(errno));
            success = false;
            continue;
        }
    }

    return (void *)success;
}

int vmu_profiler_start(const vmu_profiler_config_t *config)
{
    static const vmu_profiler_config_t default_config = {
        .thread_priority = VMU_PROFILER_THREAD_PRIO_DEFAULT_,
        .polling_interval_ms = VMU_PROFILER_POLL_INT_DEFAULT_,
        .fps_avg_frames = VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_,
        .maple_port = VMU_PROFILER_MAPLE_PORT_DEFAULT_};

    int success = true;
    vmu_profiler_t *profiler = NULL;

    if (vmu_profiler_running())
    {
        dbgio_printf("vmu_profiler_start(): Profiler already running!\n");
        success = false;
        goto done;
    }
    else
        dbgio_printf("vmu_profiler_start(): Launching profiler thread.\n");

    unsigned fps_frames =
        (config && config->fps_avg_frames) ? config->fps_avg_frames : default_config.fps_avg_frames;

    profiler = malloc(sizeof(vmu_profiler_t) + sizeof(float) * fps_frames);
    memset(profiler, 0, sizeof(vmu_profiler_t) + sizeof(float) * fps_frames);
    if (!profiler)
    {
        dbgio_printf("\tVMU Profiler failed to allocate!\n");
        success = false;
        goto done;
    }

    memcpy(&profiler->config, &default_config, sizeof(vmu_profiler_config_t));
    if (config)
    {
        if (config->thread_priority)
            profiler->config.thread_priority = config->thread_priority;

        if (config->polling_interval_ms)
            profiler->config.polling_interval_ms = config->polling_interval_ms;

        if (config->fps_avg_frames)
            profiler->config.fps_avg_frames = config->fps_avg_frames;

        if (config->maple_port)
            profiler->config.maple_port = config->maple_port;
    }

    if (rwsem_init(&profiler->rwsem) < 0)
    {
        dbgio_printf("\tCould not initialize RW semaphore!");
        success = false;
        goto dealloc;
    }
    profiler->config.maple_port = 0;
    const kthread_attr_t attr = {
        .create_detached = false,
        .stack_size = VMU_PROFILER_THD_STACK_SIZE_,
        .prio = 10, // profiler_->config.thread_priority,
        .label = VMU_PROFILER_THD_LABEL_,
    };
    profiler->done = 0;
    profiler_ = profiler;
    setup_my_measures();
    profiler->thread = thd_create_ex(&attr, vmu_profiler_run_, profiler);

    if (!profiler->thread)
    {

        dbgio_printf("\tFailed to spawn profiler thread!\n");
        success = false;
        goto deinit_sem;
    }

    goto done;

deinit_sem:

    //   rwsem_destroy(&profiler->rwsem);
dealloc:
    //    free(profiler);
    //   profiler = NULL;
    vmu_profiler_stop();
done:

    //    profiler_ = profiler;
    return success;
}

int vmu_profiler_stop(void)
{
    int success = true;

    if (!vmu_profiler_running())
    {
        dbgio_printf("vmu_profiler_stop(): Profiler isn't running!\n");
        return false;
    }

    printf("vmu_profiler_stop(): Stopping profiler!\n");

    profiler_->done = true;

    bool join_value = true;
    if (thd_join(profiler_->thread, (void **)&join_value) < 0)
    {
        dbgio_printf("\tFailed to join thread!\n");
        return false;
    }

    if (!join_value)
    {
        dbgio_printf("\tThread had some issues!\n");
        success = false;
    }

    //    if(rwsem_destroy(&profiler_->rwsem) < 0) {
    //      dbgio_printf("\tFailed to destroy rwsem: [%s]\n", strerror(errno));
    //    success = false;
    //}

    free(profiler_);
    profiler_ = NULL;

    return success;
}

int vmu_profiler_running(void)
{
    return !!profiler_;
}

int vmu_profiler_update(void)
{
    if (!vmu_profiler_running())
    {
        dbgio_printf("not running\n");
        return false;
    }

    if (rwsem_write_lock(&profiler_->rwsem) < 0)
    {
        dbgio_printf("vmu_profiler_update(): Failed to write rwlock: [%s]\n",
                     strerror(errno));
        return false;
    }

    pvr_stats_t pvr_stats;
    pvr_get_stats(&pvr_stats);

    profiler_->fps_frames[profiler_->fps_frame++] = pvr_stats.frame_rate;

    if (profiler_->fps_frame >= profiler_->config.fps_avg_frames)
        profiler_->fps_frame = 0;

    for (int i = 0; i < profiler_->measure_count; i++)
    {
        vmu_profiler_measurement_t *measure = profiler_->measures[i];
        (*measure->generate_value)((void *)measure, profiler_);
    }

    if (rwsem_write_unlock(&profiler_->rwsem) < 0)
    {
        dbgio_printf("vmu_profiler_update(): Failed to unlock rwlock: [%s]\n",
                     strerror(errno));
        return false;
    }

    return true;
}
