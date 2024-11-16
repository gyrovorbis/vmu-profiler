#include "vmu_profiler.h"

#include <kos/thread.h>
#include <kos/rwsem.h>

#include <stdlib.h>
#include <stdatomic.h>

#define VMU_PROFILER_THREAD_PRIO_DEFAULT_		PRIO_DEFAULT
#define VMU_PROFILER_POLL_INT_DEFAULT_			200
#define VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_ 	20
#define VMU_PROFILER_MAPLE_PORT_DEFAULT_		0

#define VMU_PROFILER_THD_STACK_SIZE				1024
#define VMU_PROFILER_THD_LABEL					"VmuProfiler"

static struct vmu_profiler {
	vmu_profiler_config_t config;

	kthread_t *thread;
	rw_semaphore_t *rwsem;
	atomic_bool done;

	float fps;
	float ram;
	float vram;
	float sram;
	size_t verts;

	unsigned fps_frame;
	float fps_frames[];
} *profiler_ = NULL;

static void *vmu_profiler_run_(void *arg) {
	while(!profiler_->done) {


		thd_sleep(profiler_->config.polling_interval_ms);
	}

	return NULL;
}

bool vmu_profiler_start(const vmu_profiler_config_t *config) {
	static const vmu_profiler_config_t default_config = {
		.thread_priority 	 = VMU_PROFILER_THREAD_PRIO_DEFAULT_,
		.polling_interval_ms = VMU_PROFILER_POLL_INT_DEFAULT_,
		.fps_avg_frames 	 = VMU_PROFILER_FPS_AVG_FRAMES_DEFAULT_,
		.maple_port 		 = VMU_PROFILER_MAPLE_PORT_DEFAULT_
	};

	bool success = true;

	if(vmu_profiler_running()) {
		fprintf(stderr, "vmu_profiler_start(): Profiler already running!\n");
		success = false;
		goto done;
	} else
		printf("vmu_profiler_start(): Launching profiler thread.\n");

	unsigned fps_frames = 
		(config && config->fps_avg_frames)? 
			config->fps_avg_frames : default_config.fps_avg_frames;

	profiler_ = calloc(sizeof(vmu_profiler_t) + sizeof(float) * fps_avg_frames);

	if(!profiler_) {
		fprintf(stderr, "\tVMU Profiler failed to allocate!\n");
		success = false;
		goto done;
	}

	memcpy(&profiler_->config, &default_config, sizeof(vmu_profiler_config_t));

	if(config) {
		if(config->thread_priority) 
			profiler_->config.thread_priority = config->thread_priority;

		if(config->polling_interval_ms)
			profiler_->config.polling_interval_ms = config->polling_interval_ms;
		
		if(config->fps_avg_frames)
			profiler_->config.fps_avg_frames = config->fps_avg_frames;
		
		if(config->maple_port)
			profiler_->config.maple_port = config->maple_port;
	}

	if(rwsem_init(&profiler_->rwsem) < 0) {
		fprintf(stderr, "\tCould not initialize RW semaphore!");
		success = false;
		goto dealloc;
	}

	const kthread_attr_t attr = {
		.create_detached = false,
		.stack_size 	 = VMU_PROFILER_THD_STACK_SIZE,
		.prio 			 = profiler_->config.priority,
		.label 			 = VMU_PROFILER_THD_LABEL,	
	};

	profiler_->thread = thd_create_ex(&attr, vmu_profiler_run_, NULL);
	
	if(!profiler_->thread) {
		fprintf("\tFailed to spawn profiler thread!\n");
		success = false;
		goto deinit_sem;
	}

	goto done;

deinit_sem:
	rwsem_destroy(&profiler_->rwsem);
dealloc:
	free(profiler_);
done:
	return success;
}


bool vmu_profiler_stop(void) {
	

}

bool vmu_profiler_running(void) {
	return !!profiler_;
}

bool vmu_profiler_update(size_t vert_count);

float vmu_profiler_ram (void);
float vmu_profiler_vram(void);
float vmu_profiler_sram(void);
size_t vmu_profiler_verts(void);