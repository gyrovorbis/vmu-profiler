/*  vmu_profiler.c

	Copyright (C) 2024 Falco Girgis, Jason Martin
 */

#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

#include <errno.h>

#include <kos/thread.h>
#include <kos/rwsem.h>

#include <arch/arch.h>

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


// I didn't measure what 5 lines of output use, someone can
static char pfstr[1024];

static vmu_profiler_t *profiler_ = NULL;


// sample measure for FPS, depends on internal implementaiton details
void update_fps(vmu_profiler_measurement_t *m)
{
	float fps = 0.0f;

	for (unsigned f = 0; f < profiler_->config.fps_avg_frames; ++f) {
		fps += profiler_->fps_frames[f];
	}

	fps /= (float)profiler_->config.fps_avg_frames;

	m->fstorage = fps;
}


// sample measure for PVR memory usage
void update_pvr_ram(vmu_profiler_measurement_t *m)
{
	size_t vram_stats = pvr_mem_available();

	float pvr_mem = (MB_(8) - vram_stats) / (float)MB_(8) * 100.0f;

	m->fstorage = pvr_mem;
}


void vmu_profiler_add_measure(vmu_profiler_t *prof, vmu_profiler_measurement_t *measure)
{
	if (prof->measure_count < VMU_PROFILER_MAX_MEASURES) {
		prof->measures[prof->measure_count++] = measure;
	}
}


vmu_profiler_measurement_t *init_measurement(const char *name, enum measure_type m, void (*callback)(vmu_profiler_measurement_t *m), void *user_data)
{
	vmu_profiler_measurement_t *measure = (vmu_profiler_measurement_t *)malloc(sizeof(vmu_profiler_measurement_t));

	measure->disp_name = name;
	measure->m = m;
	measure->user_data = user_data;
	measure->ustorage = 0;
	measure->sstorage[0] = 0;
	measure->generate_value = callback;

	return measure;
}


// generic to_string for all measure types
// for number types
//  display name is right-padded / left-justified 4 characters long
//  separator is always 5th character followed by a space
//  values are left-padded / right-justified 5 characters long
// for string types
//  return raw sstring as-is
static char *to_string(unsigned msr_i, unsigned call)
{
	vmu_profiler_measurement_t *measure = profiler_->measures[msr_i];

	switch (measure->m)
	{
		case use_float:
			if (!msr_i)
				sprintf(measure->sstorage, "%-4s: %5.2f", measure->disp_name, measure->fstorage);
			else
				sprintf(measure->sstorage, "\n%-4s: %5.2f", measure->disp_name, measure->fstorage);
			break;
		case use_unsigned:
			if (!msr_i)
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


static void *vmu_profiler_run_(void *arg)
{
	vmu_profiler_t *self = arg;
	int success = true;
	unsigned runs = 0;
	unsigned msr_i;	

	while (!self->done) {
		thd_sleep(self->config.polling_interval_ms);
		// "reset" vmu string for strcat calls
		pfstr[0] = 0;

		for (msr_i = 0; msr_i < profiler_->measure_count; msr_i++) {
			strcat(pfstr, to_string(msr_i, runs++));
		}

		if (rwsem_read_lock(&profiler_->rwsem) < 0) {
			fprintf(stderr, "vmu_profiler_run(): RWSEM lock failed: [%s]!\n",
						 strerror(errno));
			success = false;
			continue;
		}

		vmu_printf(pfstr);

		if (rwsem_read_unlock(&self->rwsem) < 0) {
			fprintf(stderr, "vmu_profiler_run(): RWSEM unlock failed: [%s]!\n",
						 strerror(errno));
			success = false;
			continue;
		}
	}

	return (void *)success;
}

vmu_profiler_t *vmu_profiler_start(const vmu_profiler_config_t *config)
{
	static const vmu_profiler_config_t default_config = {
		.thread_priority = VMU_PROFILER_THREAD_PRIO_DEFAULT_,
		.polling_interval_ms = VMU_PROFILER_POLL_INT_DEFAULT_,
		.fps_avg_frames = VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_,
		.maple_port = VMU_PROFILER_MAPLE_PORT_DEFAULT_};

	vmu_profiler_t *profiler = NULL;

	if (vmu_profiler_running()) {
		fprintf(stderr, "vmu_profiler_start(): Profiler already running!\n");
		goto done;
	} else {
		fprintf(stderr, "vmu_profiler_start(): Launching profiler thread.\n");
	}

	unsigned fps_frames =
		(config && config->fps_avg_frames) ? config->fps_avg_frames : default_config.fps_avg_frames;

	profiler = malloc(sizeof(vmu_profiler_t) + sizeof(float) * fps_frames);
	if (!profiler) {
		fprintf(stderr, "\tVMU Profiler failed to allocate!\n");
		goto done;
	}

	memset(profiler, 0, sizeof(vmu_profiler_t) + sizeof(float) * fps_frames);

	memcpy(&profiler->config, &default_config, sizeof(vmu_profiler_config_t));

	if (config) {
		if (config->thread_priority)
			profiler->config.thread_priority = config->thread_priority;

		if (config->polling_interval_ms)
			profiler->config.polling_interval_ms = config->polling_interval_ms;

		if (config->fps_avg_frames)
			profiler->config.fps_avg_frames = config->fps_avg_frames;

		if (config->maple_port)
			profiler->config.maple_port = config->maple_port;
	}

	if (rwsem_init(&profiler->rwsem) < 0) {
		fprintf(stderr, "\tCould not initialize RW semaphore!");
		goto dealloc;
	}

	const kthread_attr_t attr = {
		.create_detached = false,
		.stack_size = VMU_PROFILER_THD_STACK_SIZE_,
		.prio = profiler->config.thread_priority,
		.label = VMU_PROFILER_THD_LABEL_,
	};

	profiler->done = 0;

	profiler->thread = thd_create_ex(&attr, vmu_profiler_run_, profiler);

	if (!profiler->thread) {
		fprintf(stderr, "\tFailed to spawn profiler thread!\n");
		goto deinit_sem;
	}

	profiler_ = profiler;

	goto done;

deinit_sem:
	rwsem_destroy(&profiler->rwsem);

dealloc:
	free(profiler);
	profiler = NULL;

done:
	return profiler_;
}

int vmu_profiler_stop(void)
{
	int success = true;

	if (!profiler_) {
		fprintf(stderr, "vmu_profiler_stop(): Profiler is NULL!\n");
		return false;
	}

	if (!vmu_profiler_running()) {
		fprintf(stderr, "vmu_profiler_stop(): Profiler isn't running!\n");
		return false;
	}

	printf("vmu_profiler_stop(): Stopping profiler!\n");

	profiler_->done = true;

	bool join_value = true;
	if (thd_join(profiler_->thread, (void **)&join_value) < 0) {
		fprintf(stderr, "\tFailed to join thread!\n");
		return false;
	}

	if (!join_value) {
		fprintf(stderr, "\tThread had some issues!\n");
		success = false;
	}

	if(rwsem_destroy(&profiler_->rwsem) < 0) {
		fprintf(stderr, "\tFailed to destroy rwsem: [%s]\n", strerror(errno));
		success = false;
	}

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
	if (!profiler_) {
		fprintf(stderr, "profiler_ is NULL\n");
	}

	if (!vmu_profiler_running()) {
		fprintf(stderr, "not running\n");
		return false;
	}

	if (rwsem_write_lock(&profiler_->rwsem) < 0) {
		fprintf(stderr, "vmu_profiler_update(): Failed to write rwlock: [%s]\n",
					strerror(errno));
		return false;
	}

	for (int i = 0; i < profiler_->measure_count; i++) {
		vmu_profiler_measurement_t *measure = profiler_->measures[i];
		if (measure->generate_value) {
			measure->generate_value(measure);
		}
	}

	if (rwsem_write_unlock(&profiler_->rwsem) < 0) {
		fprintf(stderr, "vmu_profiler_update(): Failed to unlock rwlock: [%s]\n",
					strerror(errno));
		return false;
	}

	return true;
}