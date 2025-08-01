/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2013 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 */

#ifndef __MSM_GPU_H__
#define __MSM_GPU_H__

#include <linux/adreno-smmu-priv.h>
#include <linux/clk.h>
#include <linux/devfreq.h>
#include <linux/interconnect.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>

#include "msm_drv.h"
#include "msm_fence.h"
#include "msm_ringbuffer.h"
#include "msm_gem.h"

struct msm_gem_submit;
struct msm_gem_vm_log_entry;
struct msm_gpu_perfcntr;
struct msm_gpu_state;
struct msm_context;

struct msm_gpu_config {
	const char *ioname;
	unsigned int nr_rings;
};

/* So far, with hardware that I've seen to date, we can have:
 *  + zero, one, or two z180 2d cores
 *  + a3xx or a2xx 3d core, which share a common CP (the firmware
 *    for the CP seems to implement some different PM4 packet types
 *    but the basics of cmdstream submission are the same)
 *
 * Which means that the eventual complete "class" hierarchy, once
 * support for all past and present hw is in place, becomes:
 *  + msm_gpu
 *    + adreno_gpu
 *      + a3xx_gpu
 *      + a2xx_gpu
 *    + z180_gpu
 */
struct msm_gpu_funcs {
	int (*get_param)(struct msm_gpu *gpu, struct msm_context *ctx,
			 uint32_t param, uint64_t *value, uint32_t *len);
	int (*set_param)(struct msm_gpu *gpu, struct msm_context *ctx,
			 uint32_t param, uint64_t value, uint32_t len);
	int (*hw_init)(struct msm_gpu *gpu);

	/**
	 * @ucode_load: Optional hook to upload fw to GEM objs
	 */
	int (*ucode_load)(struct msm_gpu *gpu);

	int (*pm_suspend)(struct msm_gpu *gpu);
	int (*pm_resume)(struct msm_gpu *gpu);
	void (*submit)(struct msm_gpu *gpu, struct msm_gem_submit *submit);
	void (*flush)(struct msm_gpu *gpu, struct msm_ringbuffer *ring);
	irqreturn_t (*irq)(struct msm_gpu *irq);
	struct msm_ringbuffer *(*active_ring)(struct msm_gpu *gpu);
	void (*recover)(struct msm_gpu *gpu);
	void (*destroy)(struct msm_gpu *gpu);
#if defined(CONFIG_DEBUG_FS) || defined(CONFIG_DEV_COREDUMP)
	/* show GPU status in debugfs: */
	void (*show)(struct msm_gpu *gpu, struct msm_gpu_state *state,
			struct drm_printer *p);
	/* for generation specific debugfs: */
	void (*debugfs_init)(struct msm_gpu *gpu, struct drm_minor *minor);
#endif
	/* note: gpu_busy() can assume that we have been pm_resumed */
	u64 (*gpu_busy)(struct msm_gpu *gpu, unsigned long *out_sample_rate);
	struct msm_gpu_state *(*gpu_state_get)(struct msm_gpu *gpu);
	int (*gpu_state_put)(struct msm_gpu_state *state);
	unsigned long (*gpu_get_freq)(struct msm_gpu *gpu);
	/* note: gpu_set_freq() can assume that we have been pm_resumed */
	void (*gpu_set_freq)(struct msm_gpu *gpu, struct dev_pm_opp *opp,
			     bool suspended);
	struct drm_gpuvm *(*create_vm)(struct msm_gpu *gpu, struct platform_device *pdev);
	struct drm_gpuvm *(*create_private_vm)(struct msm_gpu *gpu, bool kernel_managed);
	uint32_t (*get_rptr)(struct msm_gpu *gpu, struct msm_ringbuffer *ring);

	/**
	 * progress: Has the GPU made progress?
	 *
	 * Return true if GPU position in cmdstream has advanced (or changed)
	 * since the last call.  To avoid false negatives, this should account
	 * for cmdstream that is buffered in this FIFO upstream of the CP fw.
	 */
	bool (*progress)(struct msm_gpu *gpu, struct msm_ringbuffer *ring);
};

/* Additional state for iommu faults: */
struct msm_gpu_fault_info {
	u64 ttbr0;
	unsigned long iova;
	int flags;
	const char *type;
	const char *block;

	/* Information about what we think/expect is the current SMMU state,
	 * for example expected_ttbr0 should match smmu_info.ttbr0 which
	 * was read back from SMMU registers.
	 */
	phys_addr_t pgtbl_ttbr0;
	u64 ptes[4];
	int asid;
};

/**
 * struct msm_gpu_devfreq - devfreq related state
 */
struct msm_gpu_devfreq {
	/** devfreq: devfreq instance */
	struct devfreq *devfreq;

	/** lock: lock for "suspended", "busy_cycles", and "time" */
	struct mutex lock;

	/**
	 * idle_freq:
	 *
	 * Shadow frequency used while the GPU is idle.  From the PoV of
	 * the devfreq governor, we are continuing to sample busyness and
	 * adjust frequency while the GPU is idle, but we use this shadow
	 * value as the GPU is actually clamped to minimum frequency while
	 * it is inactive.
	 */
	unsigned long idle_freq;

	/**
	 * boost_constraint:
	 *
	 * A PM QoS constraint to boost min freq for a period of time
	 * until the boost expires.
	 */
	struct dev_pm_qos_request boost_freq;

	/**
	 * busy_cycles: Last busy counter value, for calculating elapsed busy
	 * cycles since last sampling period.
	 */
	u64 busy_cycles;

	/** time: Time of last sampling period. */
	ktime_t time;

	/** idle_time: Time of last transition to idle: */
	ktime_t idle_time;

	/**
	 * idle_work:
	 *
	 * Used to delay clamping to idle freq on active->idle transition.
	 */
	struct msm_hrtimer_work idle_work;

	/**
	 * boost_work:
	 *
	 * Used to reset the boost_constraint after the boost period has
	 * elapsed
	 */
	struct msm_hrtimer_work boost_work;

	/** suspended: tracks if we're suspended */
	bool suspended;
};

struct msm_gpu {
	const char *name;
	struct drm_device *dev;
	struct platform_device *pdev;
	const struct msm_gpu_funcs *funcs;

	struct adreno_smmu_priv adreno_smmu;

	/* performance counters (hw & sw): */
	spinlock_t perf_lock;
	bool perfcntr_active;
	struct {
		bool active;
		ktime_t time;
	} last_sample;
	uint32_t totaltime, activetime;    /* sw counters */
	uint32_t last_cntrs[5];            /* hw counters */
	const struct msm_gpu_perfcntr *perfcntrs;
	uint32_t num_perfcntrs;

	struct msm_ringbuffer *rb[MSM_GPU_MAX_RINGS];
	int nr_rings;

	/**
	 * sysprof_active:
	 *
	 * The count of contexts that have enabled system profiling.
	 */
	refcount_t sysprof_active;

	/**
	 * lock:
	 *
	 * General lock for serializing all the gpu things.
	 *
	 * TODO move to per-ring locking where feasible (ie. submit/retire
	 * path, etc)
	 */
	struct mutex lock;

	/**
	 * active_submits:
	 *
	 * The number of submitted but not yet retired submits, used to
	 * determine transitions between active and idle.
	 *
	 * Protected by active_lock
	 */
	int active_submits;

	/** lock: protects active_submits and idle/active transitions */
	struct mutex active_lock;

	/* does gpu need hw_init? */
	bool needs_hw_init;

	/**
	 * global_faults: number of GPU hangs not attributed to a particular
	 * address space
	 */
	int global_faults;

	void __iomem *mmio;
	int irq;

	struct drm_gpuvm *vm;

	/* Power Control: */
	struct regulator *gpu_reg, *gpu_cx;
	struct clk_bulk_data *grp_clks;
	int nr_clocks;
	struct clk *ebi1_clk, *core_clk, *rbbmtimer_clk;
	uint32_t fast_rate;

	/* Hang and Inactivity Detection:
	 */
#define DRM_MSM_INACTIVE_PERIOD   66 /* in ms (roughly four frames) */

#define DRM_MSM_HANGCHECK_DEFAULT_PERIOD 500 /* in ms */
#define DRM_MSM_HANGCHECK_PROGRESS_RETRIES 3
	struct timer_list hangcheck_timer;

	/* work for handling GPU recovery: */
	struct kthread_work recover_work;

	/** retire_event: notified when submits are retired: */
	wait_queue_head_t retire_event;

	/* work for handling active-list retiring: */
	struct kthread_work retire_work;

	/* worker for retire/recover: */
	struct kthread_worker *worker;

	struct drm_gem_object *memptrs_bo;

	struct msm_gpu_devfreq devfreq;

	uint32_t suspend_count;

	struct msm_gpu_state *crashstate;

	/* True if the hardware supports expanded apriv (a650 and newer) */
	bool hw_apriv;

	/**
	 * @allow_relocs: allow relocs in SUBMIT ioctl
	 *
	 * Mesa won't use relocs for driver version 1.4.0 and later.  This
	 * switch-over happened early enough in mesa a6xx bringup that we
	 * can disallow relocs for a6xx and newer.
	 */
	bool allow_relocs;

	struct thermal_cooling_device *cooling;
};

static inline struct msm_gpu *dev_to_gpu(struct device *dev)
{
	struct adreno_smmu_priv *adreno_smmu = dev_get_drvdata(dev);

	if (!adreno_smmu)
		return NULL;

	return container_of(adreno_smmu, struct msm_gpu, adreno_smmu);
}

/* It turns out that all targets use the same ringbuffer size */
#define MSM_GPU_RINGBUFFER_SZ SZ_32K
#define MSM_GPU_RINGBUFFER_BLKSIZE 32

#define MSM_GPU_RB_CNTL_DEFAULT \
		(AXXX_CP_RB_CNTL_BUFSZ(ilog2(MSM_GPU_RINGBUFFER_SZ / 8)) | \
		AXXX_CP_RB_CNTL_BLKSZ(ilog2(MSM_GPU_RINGBUFFER_BLKSIZE / 8)))

static inline bool msm_gpu_active(struct msm_gpu *gpu)
{
	int i;

	for (i = 0; i < gpu->nr_rings; i++) {
		struct msm_ringbuffer *ring = gpu->rb[i];

		if (fence_after(ring->fctx->last_fence, ring->memptrs->fence))
			return true;
	}

	return false;
}

/* Perf-Counters:
 * The select_reg and select_val are just there for the benefit of the child
 * class that actually enables the perf counter..  but msm_gpu base class
 * will handle sampling/displaying the counters.
 */

struct msm_gpu_perfcntr {
	uint32_t select_reg;
	uint32_t sample_reg;
	uint32_t select_val;
	const char *name;
};

/*
 * The number of priority levels provided by drm gpu scheduler.  The
 * DRM_SCHED_PRIORITY_KERNEL priority level is treated specially in some
 * cases, so we don't use it (no need for kernel generated jobs).
 */
#define NR_SCHED_PRIORITIES (1 + DRM_SCHED_PRIORITY_LOW - DRM_SCHED_PRIORITY_HIGH)

/**
 * struct msm_context - per-drm_file context
 */
struct msm_context {
	/** @queuelock: synchronizes access to submitqueues list */
	rwlock_t queuelock;

	/** @submitqueues: list of &msm_gpu_submitqueue created by userspace */
	struct list_head submitqueues;

	/**
	 * @queueid:
	 *
	 * Counter incremented each time a submitqueue is created, used to
	 * assign &msm_gpu_submitqueue.id
	 */
	int queueid;

	/**
	 * @closed: The device file associated with this context has been closed.
	 *
	 * Once the device is closed, any submits that have not been written
	 * to the ring buffer are no-op'd.
	 */
	bool closed;

	/**
	 * @userspace_managed_vm:
	 *
	 * Has userspace opted-in to userspace managed VM (ie. VM_BIND) via
	 * MSM_PARAM_EN_VM_BIND?
	 */
	bool userspace_managed_vm;

	/**
	 * @vm:
	 *
	 * The per-process GPU address-space.  Do not access directly, use
	 * msm_context_vm().
	 */
	struct drm_gpuvm *vm;

	/** @kref: the reference count */
	struct kref ref;

	/**
	 * @seqno:
	 *
	 * A unique per-process sequence number.  Used to detect context
	 * switches, without relying on keeping a, potentially dangling,
	 * pointer to the previous context.
	 */
	int seqno;

	/**
	 * @sysprof:
	 *
	 * The value of MSM_PARAM_SYSPROF set by userspace.  This is
	 * intended to be used by system profiling tools like Mesa's
	 * pps-producer (perfetto), and restricted to CAP_SYS_ADMIN.
	 *
	 * Setting a value of 1 will preserve performance counters across
	 * context switches.  Setting a value of 2 will in addition
	 * suppress suspend.  (Performance counters lose state across
	 * power collapse, which is undesirable for profiling in some
	 * cases.)
	 *
	 * The value automatically reverts to zero when the drm device
	 * file is closed.
	 */
	int sysprof;

	/**
	 * @comm: Overridden task comm, see MSM_PARAM_COMM
	 *
	 * Accessed under msm_gpu::lock
	 */
	char *comm;

	/**
	 * @cmdline: Overridden task cmdline, see MSM_PARAM_CMDLINE
	 *
	 * Accessed under msm_gpu::lock
	 */
	char *cmdline;

	/**
	 * @elapsed:
	 *
	 * The total (cumulative) elapsed time GPU was busy with rendering
	 * from this context in ns.
	 */
	uint64_t elapsed_ns;

	/**
	 * @cycles:
	 *
	 * The total (cumulative) GPU cycles elapsed attributed to this
	 * context.
	 */
	uint64_t cycles;

	/**
	 * @entities:
	 *
	 * Table of per-priority-level sched entities used by submitqueues
	 * associated with this &drm_file.  Because some userspace apps
	 * make assumptions about rendering from multiple gl contexts
	 * (of the same priority) within the process happening in FIFO
	 * order without requiring any fencing beyond MakeCurrent(), we
	 * create at most one &drm_sched_entity per-process per-priority-
	 * level.
	 */
	struct drm_sched_entity *entities[NR_SCHED_PRIORITIES * MSM_GPU_MAX_RINGS];

	/**
	 * @ctx_mem:
	 *
	 * Total amount of memory of GEM buffers with handles attached for
	 * this context.
	 */
	atomic64_t ctx_mem;
};

struct drm_gpuvm *msm_context_vm(struct drm_device *dev, struct msm_context *ctx);

/**
 * msm_context_is_vm_bind() - has userspace opted in to VM_BIND?
 *
 * @ctx: the drm_file context
 *
 * See MSM_PARAM_EN_VM_BIND.  If userspace is managing the VM, it can
 * do sparse binding including having multiple, potentially partial,
 * mappings in the VM.  Therefore certain legacy uabi (ie. GET_IOVA,
 * SET_IOVA) are rejected because they don't have a sensible meaning.
 */
static inline bool
msm_context_is_vmbind(struct msm_context *ctx)
{
	return ctx->userspace_managed_vm;
}

/**
 * msm_gpu_convert_priority - Map userspace priority to ring # and sched priority
 *
 * @gpu:        the gpu instance
 * @prio:       the userspace priority level
 * @ring_nr:    [out] the ringbuffer the userspace priority maps to
 * @sched_prio: [out] the gpu scheduler priority level which the userspace
 *              priority maps to
 *
 * With drm/scheduler providing it's own level of prioritization, our total
 * number of available priority levels is (nr_rings * NR_SCHED_PRIORITIES).
 * Each ring is associated with it's own scheduler instance.  However, our
 * UABI is that lower numerical values are higher priority.  So mapping the
 * single userspace priority level into ring_nr and sched_prio takes some
 * care.  The userspace provided priority (when a submitqueue is created)
 * is mapped to ring nr and scheduler priority as such:
 *
 *   ring_nr    = userspace_prio / NR_SCHED_PRIORITIES
 *   sched_prio = NR_SCHED_PRIORITIES -
 *                (userspace_prio % NR_SCHED_PRIORITIES) - 1
 *
 * This allows generations without preemption (nr_rings==1) to have some
 * amount of prioritization, and provides more priority levels for gens
 * that do have preemption.
 */
static inline int msm_gpu_convert_priority(struct msm_gpu *gpu, int prio,
		unsigned *ring_nr, enum drm_sched_priority *sched_prio)
{
	unsigned rn, sp;

	rn = div_u64_rem(prio, NR_SCHED_PRIORITIES, &sp);

	/* invert sched priority to map to higher-numeric-is-higher-
	 * priority convention
	 */
	sp = NR_SCHED_PRIORITIES - sp - 1;

	if (rn >= gpu->nr_rings)
		return -EINVAL;

	*ring_nr = rn;
	*sched_prio = sp;

	return 0;
}

/**
 * struct msm_gpu_submitqueues - Userspace created context.
 *
 * A submitqueue is associated with a gl context or vk queue (or equiv)
 * in userspace.
 *
 * @id:        userspace id for the submitqueue, unique within the drm_file
 * @flags:     userspace flags for the submitqueue, specified at creation
 *             (currently unusued)
 * @ring_nr:   the ringbuffer used by this submitqueue, which is determined
 *             by the submitqueue's priority
 * @faults:    the number of GPU hangs associated with this submitqueue
 * @last_fence: the sequence number of the last allocated fence (for error
 *             checking)
 * @ctx:       the per-drm_file context associated with the submitqueue (ie.
 *             which set of pgtables do submits jobs associated with the
 *             submitqueue use)
 * @node:      node in the context's list of submitqueues
 * @fence_idr: maps fence-id to dma_fence for userspace visible fence
 *             seqno, protected by submitqueue lock
 * @idr_lock:  for serializing access to fence_idr
 * @lock:      submitqueue lock for serializing submits on a queue
 * @ref:       reference count
 * @entity:    the submit job-queue
 */
struct msm_gpu_submitqueue {
	int id;
	u32 flags;
	u32 ring_nr;
	int faults;
	uint32_t last_fence;
	struct msm_context *ctx;
	struct list_head node;
	struct idr fence_idr;
	struct spinlock idr_lock;
	struct mutex lock;
	struct kref ref;
	struct drm_sched_entity *entity;

	/** @_vm_bind_entity: used for @entity pointer for VM_BIND queues */
	struct drm_sched_entity _vm_bind_entity[0];
};

struct msm_gpu_state_bo {
	u64 iova;
	size_t size;
	u32 flags;
	void *data;
	bool encoded;
	char name[32];
};

struct msm_gpu_state {
	struct kref ref;
	struct timespec64 time;

	struct {
		u64 iova;
		u32 fence;
		u32 seqno;
		u32 rptr;
		u32 wptr;
		void *data;
		int data_size;
		bool encoded;
	} ring[MSM_GPU_MAX_RINGS];

	int nr_registers;
	u32 *registers;

	u32 rbbm_status;

	char *comm;
	char *cmd;

	struct msm_gpu_fault_info fault_info;

	int nr_vm_logs;
	struct msm_gem_vm_log_entry *vm_logs;

	int nr_bos;
	struct msm_gpu_state_bo *bos;
};

static inline void gpu_write(struct msm_gpu *gpu, u32 reg, u32 data)
{
	writel(data, gpu->mmio + (reg << 2));
}

static inline u32 gpu_read(struct msm_gpu *gpu, u32 reg)
{
	return readl(gpu->mmio + (reg << 2));
}

static inline void gpu_rmw(struct msm_gpu *gpu, u32 reg, u32 mask, u32 or)
{
	msm_rmw(gpu->mmio + (reg << 2), mask, or);
}

static inline u64 gpu_read64(struct msm_gpu *gpu, u32 reg)
{
	u64 val;

	/*
	 * Why not a readq here? Two reasons: 1) many of the LO registers are
	 * not quad word aligned and 2) the GPU hardware designers have a bit
	 * of a history of putting registers where they fit, especially in
	 * spins. The longer a GPU family goes the higher the chance that
	 * we'll get burned.  We could do a series of validity checks if we
	 * wanted to, but really is a readq() that much better? Nah.
	 */

	/*
	 * For some lo/hi registers (like perfcounters), the hi value is latched
	 * when the lo is read, so make sure to read the lo first to trigger
	 * that
	 */
	val = (u64) readl(gpu->mmio + (reg << 2));
	val |= ((u64) readl(gpu->mmio + ((reg + 1) << 2)) << 32);

	return val;
}

static inline void gpu_write64(struct msm_gpu *gpu, u32 reg, u64 val)
{
	/* Why not a writeq here? Read the screed above */
	writel(lower_32_bits(val), gpu->mmio + (reg << 2));
	writel(upper_32_bits(val), gpu->mmio + ((reg + 1) << 2));
}

int msm_gpu_pm_suspend(struct msm_gpu *gpu);
int msm_gpu_pm_resume(struct msm_gpu *gpu);

void msm_gpu_show_fdinfo(struct msm_gpu *gpu, struct msm_context *ctx,
			 struct drm_printer *p);

int msm_submitqueue_init(struct drm_device *drm, struct msm_context *ctx);
struct msm_gpu_submitqueue *msm_submitqueue_get(struct msm_context *ctx,
		u32 id);
int msm_submitqueue_create(struct drm_device *drm,
		struct msm_context *ctx,
		u32 prio, u32 flags, u32 *id);
int msm_submitqueue_query(struct drm_device *drm, struct msm_context *ctx,
		struct drm_msm_submitqueue_query *args);
int msm_submitqueue_remove(struct msm_context *ctx, u32 id);
void msm_submitqueue_close(struct msm_context *ctx);

void msm_submitqueue_destroy(struct kref *kref);

int msm_context_set_sysprof(struct msm_context *ctx, struct msm_gpu *gpu, int sysprof);
void __msm_context_destroy(struct kref *kref);

static inline void msm_context_put(struct msm_context *ctx)
{
	kref_put(&ctx->ref, __msm_context_destroy);
}

static inline struct msm_context *msm_context_get(
	struct msm_context *ctx)
{
	kref_get(&ctx->ref);
	return ctx;
}

void msm_devfreq_init(struct msm_gpu *gpu);
void msm_devfreq_cleanup(struct msm_gpu *gpu);
void msm_devfreq_resume(struct msm_gpu *gpu);
void msm_devfreq_suspend(struct msm_gpu *gpu);
void msm_devfreq_boost(struct msm_gpu *gpu, unsigned factor);
void msm_devfreq_active(struct msm_gpu *gpu);
void msm_devfreq_idle(struct msm_gpu *gpu);

int msm_gpu_hw_init(struct msm_gpu *gpu);

void msm_gpu_perfcntr_start(struct msm_gpu *gpu);
void msm_gpu_perfcntr_stop(struct msm_gpu *gpu);
int msm_gpu_perfcntr_sample(struct msm_gpu *gpu, uint32_t *activetime,
		uint32_t *totaltime, uint32_t ncntrs, uint32_t *cntrs);

void msm_gpu_retire(struct msm_gpu *gpu);
void msm_gpu_submit(struct msm_gpu *gpu, struct msm_gem_submit *submit);

int msm_gpu_init(struct drm_device *drm, struct platform_device *pdev,
		struct msm_gpu *gpu, const struct msm_gpu_funcs *funcs,
		const char *name, struct msm_gpu_config *config);

struct drm_gpuvm *
msm_gpu_create_private_vm(struct msm_gpu *gpu, struct task_struct *task,
			  bool kernel_managed);

void msm_gpu_cleanup(struct msm_gpu *gpu);

struct msm_gpu *adreno_load_gpu(struct drm_device *dev);
bool adreno_has_gpu(struct device_node *node);
void __init adreno_register(void);
void __exit adreno_unregister(void);

static inline void msm_submitqueue_put(struct msm_gpu_submitqueue *queue)
{
	if (queue)
		kref_put(&queue->ref, msm_submitqueue_destroy);
}

static inline struct msm_gpu_state *msm_gpu_crashstate_get(struct msm_gpu *gpu)
{
	struct msm_gpu_state *state = NULL;

	mutex_lock(&gpu->lock);

	if (gpu->crashstate) {
		kref_get(&gpu->crashstate->ref);
		state = gpu->crashstate;
	}

	mutex_unlock(&gpu->lock);

	return state;
}

static inline void msm_gpu_crashstate_put(struct msm_gpu *gpu)
{
	mutex_lock(&gpu->lock);

	if (gpu->crashstate) {
		if (gpu->funcs->gpu_state_put(gpu->crashstate))
			gpu->crashstate = NULL;
	}

	mutex_unlock(&gpu->lock);
}

void msm_gpu_fault_crashstate_capture(struct msm_gpu *gpu, struct msm_gpu_fault_info *fault_info);

/*
 * Simple macro to semi-cleanly add the MAP_PRIV flag for targets that can
 * support expanded privileges
 */
#define check_apriv(gpu, flags) \
	(((gpu)->hw_apriv ? MSM_BO_MAP_PRIV : 0) | (flags))


#endif /* __MSM_GPU_H__ */
