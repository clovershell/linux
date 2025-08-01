/* SPDX-License-Identifier: GPL-2.0 */
#ifndef BLK_MQ_H
#define BLK_MQ_H

#include <linux/blkdev.h>
#include <linux/sbitmap.h>
#include <linux/lockdep.h>
#include <linux/scatterlist.h>
#include <linux/prefetch.h>
#include <linux/srcu.h>
#include <linux/rw_hint.h>
#include <linux/rwsem.h>

struct blk_mq_tags;
struct blk_flush_queue;

#define BLKDEV_MIN_RQ	4
#define BLKDEV_DEFAULT_RQ	128

enum rq_end_io_ret {
	RQ_END_IO_NONE,
	RQ_END_IO_FREE,
};

typedef enum rq_end_io_ret (rq_end_io_fn)(struct request *, blk_status_t);

/*
 * request flags */
typedef __u32 __bitwise req_flags_t;

/* Keep rqf_name[] in sync with the definitions below */
enum rqf_flags {
	/* drive already may have started this one */
	__RQF_STARTED,
	/* request for flush sequence */
	__RQF_FLUSH_SEQ,
	/* merge of different types, fail separately */
	__RQF_MIXED_MERGE,
	/* don't call prep for this one */
	__RQF_DONTPREP,
	/* use hctx->sched_tags */
	__RQF_SCHED_TAGS,
	/* use an I/O scheduler for this request */
	__RQF_USE_SCHED,
	/* vaguely specified driver internal error.  Ignored by block layer */
	__RQF_FAILED,
	/* don't warn about errors */
	__RQF_QUIET,
	/* account into disk and partition IO statistics */
	__RQF_IO_STAT,
	/* runtime pm request */
	__RQF_PM,
	/* on IO scheduler merge hash */
	__RQF_HASHED,
	/* track IO completion time */
	__RQF_STATS,
	/* Look at ->special_vec for the actual data payload instead of the
	   bio chain. */
	__RQF_SPECIAL_PAYLOAD,
	/* request completion needs to be signaled to zone write plugging. */
	__RQF_ZONE_WRITE_PLUGGING,
	/* ->timeout has been called, don't expire again */
	__RQF_TIMED_OUT,
	__RQF_RESV,
	__RQF_BITS
};

#define RQF_STARTED		((__force req_flags_t)(1 << __RQF_STARTED))
#define RQF_FLUSH_SEQ		((__force req_flags_t)(1 << __RQF_FLUSH_SEQ))
#define RQF_MIXED_MERGE		((__force req_flags_t)(1 << __RQF_MIXED_MERGE))
#define RQF_DONTPREP		((__force req_flags_t)(1 << __RQF_DONTPREP))
#define RQF_SCHED_TAGS		((__force req_flags_t)(1 << __RQF_SCHED_TAGS))
#define RQF_USE_SCHED		((__force req_flags_t)(1 << __RQF_USE_SCHED))
#define RQF_FAILED		((__force req_flags_t)(1 << __RQF_FAILED))
#define RQF_QUIET		((__force req_flags_t)(1 << __RQF_QUIET))
#define RQF_IO_STAT		((__force req_flags_t)(1 << __RQF_IO_STAT))
#define RQF_PM			((__force req_flags_t)(1 << __RQF_PM))
#define RQF_HASHED		((__force req_flags_t)(1 << __RQF_HASHED))
#define RQF_STATS		((__force req_flags_t)(1 << __RQF_STATS))
#define RQF_SPECIAL_PAYLOAD	\
			((__force req_flags_t)(1 << __RQF_SPECIAL_PAYLOAD))
#define RQF_ZONE_WRITE_PLUGGING	\
			((__force req_flags_t)(1 << __RQF_ZONE_WRITE_PLUGGING))
#define RQF_TIMED_OUT		((__force req_flags_t)(1 << __RQF_TIMED_OUT))
#define RQF_RESV		((__force req_flags_t)(1 << __RQF_RESV))

/* flags that prevent us from merging requests: */
#define RQF_NOMERGE_FLAGS \
	(RQF_STARTED | RQF_FLUSH_SEQ | RQF_SPECIAL_PAYLOAD)

enum mq_rq_state {
	MQ_RQ_IDLE		= 0,
	MQ_RQ_IN_FLIGHT		= 1,
	MQ_RQ_COMPLETE		= 2,
};

/*
 * Try to put the fields that are referenced together in the same cacheline.
 *
 * If you modify this structure, make sure to update blk_rq_init() and
 * especially blk_mq_rq_ctx_init() to take care of the added fields.
 */
struct request {
	struct request_queue *q;
	struct blk_mq_ctx *mq_ctx;
	struct blk_mq_hw_ctx *mq_hctx;

	blk_opf_t cmd_flags;		/* op and common flags */
	req_flags_t rq_flags;

	int tag;
	int internal_tag;

	unsigned int timeout;

	/* the following two fields are internal, NEVER access directly */
	unsigned int __data_len;	/* total data len */
	sector_t __sector;		/* sector cursor */

	struct bio *bio;
	struct bio *biotail;

	union {
		struct list_head queuelist;
		struct request *rq_next;
	};

	struct block_device *part;
#ifdef CONFIG_BLK_RQ_ALLOC_TIME
	/* Time that the first bio started allocating this request. */
	u64 alloc_time_ns;
#endif
	/* Time that this request was allocated for this IO. */
	u64 start_time_ns;
	/* Time that I/O was submitted to the device. */
	u64 io_start_time_ns;

#ifdef CONFIG_BLK_WBT
	unsigned short wbt_flags;
#endif
	/*
	 * rq sectors used for blk stats. It has the same value
	 * with blk_rq_sectors(rq), except that it never be zeroed
	 * by completion.
	 */
	unsigned short stats_sectors;

	/*
	 * Number of scatter-gather DMA addr+len pairs after
	 * physical address coalescing is performed.
	 */
	unsigned short nr_phys_segments;
	unsigned short nr_integrity_segments;

#ifdef CONFIG_BLK_INLINE_ENCRYPTION
	struct bio_crypt_ctx *crypt_ctx;
	struct blk_crypto_keyslot *crypt_keyslot;
#endif

	enum mq_rq_state state;
	atomic_t ref;

	unsigned long deadline;

	/*
	 * The hash is used inside the scheduler, and killed once the
	 * request reaches the dispatch list. The ipi_list is only used
	 * to queue the request for softirq completion, which is long
	 * after the request has been unhashed (and even removed from
	 * the dispatch list).
	 */
	union {
		struct hlist_node hash;	/* merge hash */
		struct llist_node ipi_list;
	};

	/*
	 * The rb_node is only used inside the io scheduler, requests
	 * are pruned when moved to the dispatch queue. special_vec must
	 * only be used if RQF_SPECIAL_PAYLOAD is set, and those cannot be
	 * insert into an IO scheduler.
	 */
	union {
		struct rb_node rb_node;	/* sort/lookup */
		struct bio_vec special_vec;
	};

	/*
	 * Three pointers are available for the IO schedulers, if they need
	 * more they have to dynamically allocate it.
	 */
	struct {
		struct io_cq		*icq;
		void			*priv[2];
	} elv;

	struct {
		unsigned int		seq;
		rq_end_io_fn		*saved_end_io;
	} flush;

	u64 fifo_time;

	/*
	 * completion callback.
	 */
	rq_end_io_fn *end_io;
	void *end_io_data;
};

static inline enum req_op req_op(const struct request *req)
{
	return req->cmd_flags & REQ_OP_MASK;
}

static inline bool blk_rq_is_passthrough(struct request *rq)
{
	return blk_op_is_passthrough(rq->cmd_flags);
}

static inline unsigned short req_get_ioprio(struct request *req)
{
	if (req->bio)
		return req->bio->bi_ioprio;
	return 0;
}

#define rq_data_dir(rq)		(op_is_write(req_op(rq)) ? WRITE : READ)

#define rq_dma_dir(rq) \
	(op_is_write(req_op(rq)) ? DMA_TO_DEVICE : DMA_FROM_DEVICE)

static inline int rq_list_empty(const struct rq_list *rl)
{
	return rl->head == NULL;
}

static inline void rq_list_init(struct rq_list *rl)
{
	rl->head = NULL;
	rl->tail = NULL;
}

static inline void rq_list_add_tail(struct rq_list *rl, struct request *rq)
{
	rq->rq_next = NULL;
	if (rl->tail)
		rl->tail->rq_next = rq;
	else
		rl->head = rq;
	rl->tail = rq;
}

static inline void rq_list_add_head(struct rq_list *rl, struct request *rq)
{
	rq->rq_next = rl->head;
	rl->head = rq;
	if (!rl->tail)
		rl->tail = rq;
}

static inline struct request *rq_list_pop(struct rq_list *rl)
{
	struct request *rq = rl->head;

	if (rq) {
		rl->head = rl->head->rq_next;
		if (!rl->head)
			rl->tail = NULL;
		rq->rq_next = NULL;
	}

	return rq;
}

static inline struct request *rq_list_peek(struct rq_list *rl)
{
	return rl->head;
}

#define rq_list_for_each(rl, pos)					\
	for (pos = rq_list_peek((rl)); (pos); pos = pos->rq_next)

#define rq_list_for_each_safe(rl, pos, nxt)				\
	for (pos = rq_list_peek((rl)), nxt = pos->rq_next;		\
		pos; pos = nxt, nxt = pos ? pos->rq_next : NULL)

/**
 * enum blk_eh_timer_return - How the timeout handler should proceed
 * @BLK_EH_DONE: The block driver completed the command or will complete it at
 *	a later time.
 * @BLK_EH_RESET_TIMER: Reset the request timer and continue waiting for the
 *	request to complete.
 */
enum blk_eh_timer_return {
	BLK_EH_DONE,
	BLK_EH_RESET_TIMER,
};

/**
 * struct blk_mq_hw_ctx - State for a hardware queue facing the hardware
 * block device
 */
struct blk_mq_hw_ctx {
	struct {
		/** @lock: Protects the dispatch list. */
		spinlock_t		lock;
		/**
		 * @dispatch: Used for requests that are ready to be
		 * dispatched to the hardware but for some reason (e.g. lack of
		 * resources) could not be sent to the hardware. As soon as the
		 * driver can send new requests, requests at this list will
		 * be sent first for a fairer dispatch.
		 */
		struct list_head	dispatch;
		 /**
		  * @state: BLK_MQ_S_* flags. Defines the state of the hw
		  * queue (active, scheduled to restart, stopped).
		  */
		unsigned long		state;
	} ____cacheline_aligned_in_smp;

	/**
	 * @run_work: Used for scheduling a hardware queue run at a later time.
	 */
	struct delayed_work	run_work;
	/** @cpumask: Map of available CPUs where this hctx can run. */
	cpumask_var_t		cpumask;
	/**
	 * @next_cpu: Used by blk_mq_hctx_next_cpu() for round-robin CPU
	 * selection from @cpumask.
	 */
	int			next_cpu;
	/**
	 * @next_cpu_batch: Counter of how many works left in the batch before
	 * changing to the next CPU.
	 */
	int			next_cpu_batch;

	/** @flags: BLK_MQ_F_* flags. Defines the behaviour of the queue. */
	unsigned long		flags;

	/**
	 * @sched_data: Pointer owned by the IO scheduler attached to a request
	 * queue. It's up to the IO scheduler how to use this pointer.
	 */
	void			*sched_data;
	/**
	 * @queue: Pointer to the request queue that owns this hardware context.
	 */
	struct request_queue	*queue;
	/** @fq: Queue of requests that need to perform a flush operation. */
	struct blk_flush_queue	*fq;

	/**
	 * @driver_data: Pointer to data owned by the block driver that created
	 * this hctx
	 */
	void			*driver_data;

	/**
	 * @ctx_map: Bitmap for each software queue. If bit is on, there is a
	 * pending request in that software queue.
	 */
	struct sbitmap		ctx_map;

	/**
	 * @dispatch_from: Software queue to be used when no scheduler was
	 * selected.
	 */
	struct blk_mq_ctx	*dispatch_from;
	/**
	 * @dispatch_busy: Number used by blk_mq_update_dispatch_busy() to
	 * decide if the hw_queue is busy using Exponential Weighted Moving
	 * Average algorithm.
	 */
	unsigned int		dispatch_busy;

	/** @type: HCTX_TYPE_* flags. Type of hardware queue. */
	unsigned short		type;
	/** @nr_ctx: Number of software queues. */
	unsigned short		nr_ctx;
	/** @ctxs: Array of software queues. */
	struct blk_mq_ctx	**ctxs;

	/** @dispatch_wait_lock: Lock for dispatch_wait queue. */
	spinlock_t		dispatch_wait_lock;
	/**
	 * @dispatch_wait: Waitqueue to put requests when there is no tag
	 * available at the moment, to wait for another try in the future.
	 */
	wait_queue_entry_t	dispatch_wait;

	/**
	 * @wait_index: Index of next available dispatch_wait queue to insert
	 * requests.
	 */
	atomic_t		wait_index;

	/**
	 * @tags: Tags owned by the block driver. A tag at this set is only
	 * assigned when a request is dispatched from a hardware queue.
	 */
	struct blk_mq_tags	*tags;
	/**
	 * @sched_tags: Tags owned by I/O scheduler. If there is an I/O
	 * scheduler associated with a request queue, a tag is assigned when
	 * that request is allocated. Else, this member is not used.
	 */
	struct blk_mq_tags	*sched_tags;

	/** @numa_node: NUMA node the storage adapter has been connected to. */
	unsigned int		numa_node;
	/** @queue_num: Index of this hardware queue. */
	unsigned int		queue_num;

	/**
	 * @nr_active: Number of active requests. Only used when a tag set is
	 * shared across request queues.
	 */
	atomic_t		nr_active;

	/** @cpuhp_online: List to store request if CPU is going to die */
	struct hlist_node	cpuhp_online;
	/** @cpuhp_dead: List to store request if some CPU die. */
	struct hlist_node	cpuhp_dead;
	/** @kobj: Kernel object for sysfs. */
	struct kobject		kobj;

#ifdef CONFIG_BLK_DEBUG_FS
	/**
	 * @debugfs_dir: debugfs directory for this hardware queue. Named
	 * as cpu<cpu_number>.
	 */
	struct dentry		*debugfs_dir;
	/** @sched_debugfs_dir:	debugfs directory for the scheduler. */
	struct dentry		*sched_debugfs_dir;
#endif

	/**
	 * @hctx_list: if this hctx is not in use, this is an entry in
	 * q->unused_hctx_list.
	 */
	struct list_head	hctx_list;
};

/**
 * struct blk_mq_queue_map - Map software queues to hardware queues
 * @mq_map:       CPU ID to hardware queue index map. This is an array
 *	with nr_cpu_ids elements. Each element has a value in the range
 *	[@queue_offset, @queue_offset + @nr_queues).
 * @nr_queues:    Number of hardware queues to map CPU IDs onto.
 * @queue_offset: First hardware queue to map onto. Used by the PCIe NVMe
 *	driver to map each hardware queue type (enum hctx_type) onto a distinct
 *	set of hardware queues.
 */
struct blk_mq_queue_map {
	unsigned int *mq_map;
	unsigned int nr_queues;
	unsigned int queue_offset;
};

/**
 * enum hctx_type - Type of hardware queue
 * @HCTX_TYPE_DEFAULT:	All I/O not otherwise accounted for.
 * @HCTX_TYPE_READ:	Just for READ I/O.
 * @HCTX_TYPE_POLL:	Polled I/O of any kind.
 * @HCTX_MAX_TYPES:	Number of types of hctx.
 */
enum hctx_type {
	HCTX_TYPE_DEFAULT,
	HCTX_TYPE_READ,
	HCTX_TYPE_POLL,

	HCTX_MAX_TYPES,
};

/**
 * struct blk_mq_tag_set - tag set that can be shared between request queues
 * @ops:	   Pointers to functions that implement block driver behavior.
 * @map:	   One or more ctx -> hctx mappings. One map exists for each
 *		   hardware queue type (enum hctx_type) that the driver wishes
 *		   to support. There are no restrictions on maps being of the
 *		   same size, and it's perfectly legal to share maps between
 *		   types.
 * @nr_maps:	   Number of elements in the @map array. A number in the range
 *		   [1, HCTX_MAX_TYPES].
 * @nr_hw_queues:  Number of hardware queues supported by the block driver that
 *		   owns this data structure.
 * @queue_depth:   Number of tags per hardware queue, reserved tags included.
 * @reserved_tags: Number of tags to set aside for BLK_MQ_REQ_RESERVED tag
 *		   allocations.
 * @cmd_size:	   Number of additional bytes to allocate per request. The block
 *		   driver owns these additional bytes.
 * @numa_node:	   NUMA node the storage adapter has been connected to.
 * @timeout:	   Request processing timeout in jiffies.
 * @flags:	   Zero or more BLK_MQ_F_* flags.
 * @driver_data:   Pointer to data owned by the block driver that created this
 *		   tag set.
 * @tags:	   Tag sets. One tag set per hardware queue. Has @nr_hw_queues
 *		   elements.
 * @shared_tags:
 *		   Shared set of tags. Has @nr_hw_queues elements. If set,
 *		   shared by all @tags.
 * @tag_list_lock: Serializes tag_list accesses.
 * @tag_list:	   List of the request queues that use this tag set. See also
 *		   request_queue.tag_set_list.
 * @srcu:	   Use as lock when type of the request queue is blocking
 *		   (BLK_MQ_F_BLOCKING).
 * @update_nr_hwq_lock:
 * 		   Synchronize updating nr_hw_queues with add/del disk &
 * 		   switching elevator.
 */
struct blk_mq_tag_set {
	const struct blk_mq_ops	*ops;
	struct blk_mq_queue_map	map[HCTX_MAX_TYPES];
	unsigned int		nr_maps;
	unsigned int		nr_hw_queues;
	unsigned int		queue_depth;
	unsigned int		reserved_tags;
	unsigned int		cmd_size;
	int			numa_node;
	unsigned int		timeout;
	unsigned int		flags;
	void			*driver_data;

	struct blk_mq_tags	**tags;

	struct blk_mq_tags	*shared_tags;

	struct mutex		tag_list_lock;
	struct list_head	tag_list;
	struct srcu_struct	*srcu;

	struct rw_semaphore	update_nr_hwq_lock;
};

/**
 * struct blk_mq_queue_data - Data about a request inserted in a queue
 *
 * @rq:   Request pointer.
 * @last: If it is the last request in the queue.
 */
struct blk_mq_queue_data {
	struct request *rq;
	bool last;
};

typedef bool (busy_tag_iter_fn)(struct request *, void *);

/**
 * struct blk_mq_ops - Callback functions that implements block driver
 * behaviour.
 */
struct blk_mq_ops {
	/**
	 * @queue_rq: Queue a new request from block IO.
	 */
	blk_status_t (*queue_rq)(struct blk_mq_hw_ctx *,
				 const struct blk_mq_queue_data *);

	/**
	 * @commit_rqs: If a driver uses bd->last to judge when to submit
	 * requests to hardware, it must define this function. In case of errors
	 * that make us stop issuing further requests, this hook serves the
	 * purpose of kicking the hardware (which the last request otherwise
	 * would have done).
	 */
	void (*commit_rqs)(struct blk_mq_hw_ctx *);

	/**
	 * @queue_rqs: Queue a list of new requests. Driver is guaranteed
	 * that each request belongs to the same queue. If the driver doesn't
	 * empty the @rqlist completely, then the rest will be queued
	 * individually by the block layer upon return.
	 */
	void (*queue_rqs)(struct rq_list *rqlist);

	/**
	 * @get_budget: Reserve budget before queue request, once .queue_rq is
	 * run, it is driver's responsibility to release the
	 * reserved budget. Also we have to handle failure case
	 * of .get_budget for avoiding I/O deadlock.
	 */
	int (*get_budget)(struct request_queue *);

	/**
	 * @put_budget: Release the reserved budget.
	 */
	void (*put_budget)(struct request_queue *, int);

	/**
	 * @set_rq_budget_token: store rq's budget token
	 */
	void (*set_rq_budget_token)(struct request *, int);
	/**
	 * @get_rq_budget_token: retrieve rq's budget token
	 */
	int (*get_rq_budget_token)(struct request *);

	/**
	 * @timeout: Called on request timeout.
	 */
	enum blk_eh_timer_return (*timeout)(struct request *);

	/**
	 * @poll: Called to poll for completion of a specific tag.
	 */
	int (*poll)(struct blk_mq_hw_ctx *, struct io_comp_batch *);

	/**
	 * @complete: Mark the request as complete.
	 */
	void (*complete)(struct request *);

	/**
	 * @init_hctx: Called when the block layer side of a hardware queue has
	 * been set up, allowing the driver to allocate/init matching
	 * structures.
	 */
	int (*init_hctx)(struct blk_mq_hw_ctx *, void *, unsigned int);
	/**
	 * @exit_hctx: Ditto for exit/teardown.
	 */
	void (*exit_hctx)(struct blk_mq_hw_ctx *, unsigned int);

	/**
	 * @init_request: Called for every command allocated by the block layer
	 * to allow the driver to set up driver specific data.
	 *
	 * Tag greater than or equal to queue_depth is for setting up
	 * flush request.
	 */
	int (*init_request)(struct blk_mq_tag_set *set, struct request *,
			    unsigned int, unsigned int);
	/**
	 * @exit_request: Ditto for exit/teardown.
	 */
	void (*exit_request)(struct blk_mq_tag_set *set, struct request *,
			     unsigned int);

	/**
	 * @cleanup_rq: Called before freeing one request which isn't completed
	 * yet, and usually for freeing the driver private data.
	 */
	void (*cleanup_rq)(struct request *);

	/**
	 * @busy: If set, returns whether or not this queue currently is busy.
	 */
	bool (*busy)(struct request_queue *);

	/**
	 * @map_queues: This allows drivers specify their own queue mapping by
	 * overriding the setup-time function that builds the mq_map.
	 */
	void (*map_queues)(struct blk_mq_tag_set *set);

#ifdef CONFIG_BLK_DEBUG_FS
	/**
	 * @show_rq: Used by the debugfs implementation to show driver-specific
	 * information about a request.
	 */
	void (*show_rq)(struct seq_file *m, struct request *rq);
#endif
};

/* Keep hctx_flag_name[] in sync with the definitions below */
enum {
	BLK_MQ_F_TAG_QUEUE_SHARED = 1 << 1,
	/*
	 * Set when this device requires underlying blk-mq device for
	 * completing IO:
	 */
	BLK_MQ_F_STACKING	= 1 << 2,
	BLK_MQ_F_TAG_HCTX_SHARED = 1 << 3,
	BLK_MQ_F_BLOCKING	= 1 << 4,

	/*
	 * Alloc tags on a round-robin base instead of the first available one.
	 */
	BLK_MQ_F_TAG_RR		= 1 << 5,

	/*
	 * Select 'none' during queue registration in case of a single hwq
	 * or shared hwqs instead of 'mq-deadline'.
	 */
	BLK_MQ_F_NO_SCHED_BY_DEFAULT	= 1 << 6,

	BLK_MQ_F_MAX = 1 << 7,
};

#define BLK_MQ_MAX_DEPTH	(10240)
#define BLK_MQ_NO_HCTX_IDX	(-1U)

enum {
	/* Keep hctx_state_name[] in sync with the definitions below */
	BLK_MQ_S_STOPPED,
	BLK_MQ_S_TAG_ACTIVE,
	BLK_MQ_S_SCHED_RESTART,
	/* hw queue is inactive after all its CPUs become offline */
	BLK_MQ_S_INACTIVE,
	BLK_MQ_S_MAX
};

struct gendisk *__blk_mq_alloc_disk(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata,
		struct lock_class_key *lkclass);
#define blk_mq_alloc_disk(set, lim, queuedata)				\
({									\
	static struct lock_class_key __key;				\
									\
	__blk_mq_alloc_disk(set, lim, queuedata, &__key);		\
})
struct gendisk *blk_mq_alloc_disk_for_queue(struct request_queue *q,
		struct lock_class_key *lkclass);
struct request_queue *blk_mq_alloc_queue(struct blk_mq_tag_set *set,
		struct queue_limits *lim, void *queuedata);
int blk_mq_init_allocated_queue(struct blk_mq_tag_set *set,
		struct request_queue *q);
void blk_mq_destroy_queue(struct request_queue *);

int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set);
int blk_mq_alloc_sq_tag_set(struct blk_mq_tag_set *set,
		const struct blk_mq_ops *ops, unsigned int queue_depth,
		unsigned int set_flags);
void blk_mq_free_tag_set(struct blk_mq_tag_set *set);

void blk_mq_free_request(struct request *rq);
int blk_rq_poll(struct request *rq, struct io_comp_batch *iob,
		unsigned int poll_flags);

bool blk_mq_queue_inflight(struct request_queue *q);

enum {
	/* return when out of requests */
	BLK_MQ_REQ_NOWAIT	= (__force blk_mq_req_flags_t)(1 << 0),
	/* allocate from reserved pool */
	BLK_MQ_REQ_RESERVED	= (__force blk_mq_req_flags_t)(1 << 1),
	/* set RQF_PM */
	BLK_MQ_REQ_PM		= (__force blk_mq_req_flags_t)(1 << 2),
};

struct request *blk_mq_alloc_request(struct request_queue *q, blk_opf_t opf,
		blk_mq_req_flags_t flags);
struct request *blk_mq_alloc_request_hctx(struct request_queue *q,
		blk_opf_t opf, blk_mq_req_flags_t flags,
		unsigned int hctx_idx);

/*
 * Tag address space map.
 */
struct blk_mq_tags {
	unsigned int nr_tags;
	unsigned int nr_reserved_tags;
	unsigned int active_queues;

	struct sbitmap_queue bitmap_tags;
	struct sbitmap_queue breserved_tags;

	struct request **rqs;
	struct request **static_rqs;
	struct list_head page_list;

	/*
	 * used to clear request reference in rqs[] before freeing one
	 * request pool
	 */
	spinlock_t lock;
};

static inline struct request *blk_mq_tag_to_rq(struct blk_mq_tags *tags,
					       unsigned int tag)
{
	if (tag < tags->nr_tags) {
		prefetch(tags->rqs[tag]);
		return tags->rqs[tag];
	}

	return NULL;
}

enum {
	BLK_MQ_UNIQUE_TAG_BITS = 16,
	BLK_MQ_UNIQUE_TAG_MASK = (1 << BLK_MQ_UNIQUE_TAG_BITS) - 1,
};

u32 blk_mq_unique_tag(struct request *rq);

static inline u16 blk_mq_unique_tag_to_hwq(u32 unique_tag)
{
	return unique_tag >> BLK_MQ_UNIQUE_TAG_BITS;
}

static inline u16 blk_mq_unique_tag_to_tag(u32 unique_tag)
{
	return unique_tag & BLK_MQ_UNIQUE_TAG_MASK;
}

/**
 * blk_mq_rq_state() - read the current MQ_RQ_* state of a request
 * @rq: target request.
 */
static inline enum mq_rq_state blk_mq_rq_state(struct request *rq)
{
	return READ_ONCE(rq->state);
}

static inline int blk_mq_request_started(struct request *rq)
{
	return blk_mq_rq_state(rq) != MQ_RQ_IDLE;
}

static inline int blk_mq_request_completed(struct request *rq)
{
	return blk_mq_rq_state(rq) == MQ_RQ_COMPLETE;
}

/*
 * 
 * Set the state to complete when completing a request from inside ->queue_rq.
 * This is used by drivers that want to ensure special complete actions that
 * need access to the request are called on failure, e.g. by nvme for
 * multipathing.
 */
static inline void blk_mq_set_request_complete(struct request *rq)
{
	WRITE_ONCE(rq->state, MQ_RQ_COMPLETE);
}

/*
 * Complete the request directly instead of deferring it to softirq or
 * completing it another CPU. Useful in preemptible instead of an interrupt.
 */
static inline void blk_mq_complete_request_direct(struct request *rq,
		   void (*complete)(struct request *rq))
{
	WRITE_ONCE(rq->state, MQ_RQ_COMPLETE);
	complete(rq);
}

void blk_mq_start_request(struct request *rq);
void blk_mq_end_request(struct request *rq, blk_status_t error);
void __blk_mq_end_request(struct request *rq, blk_status_t error);
void blk_mq_end_request_batch(struct io_comp_batch *ib);

/*
 * Only need start/end time stamping if we have iostat or
 * blk stats enabled, or using an IO scheduler.
 */
static inline bool blk_mq_need_time_stamp(struct request *rq)
{
	return (rq->rq_flags & (RQF_IO_STAT | RQF_STATS | RQF_USE_SCHED));
}

static inline bool blk_mq_is_reserved_rq(struct request *rq)
{
	return rq->rq_flags & RQF_RESV;
}

/**
 * blk_mq_add_to_batch() - add a request to the completion batch
 * @req: The request to add to batch
 * @iob: The batch to add the request
 * @is_error: Specify true if the request failed with an error
 * @complete: The completaion handler for the request
 *
 * Batched completions only work when there is no I/O error and no special
 * ->end_io handler.
 *
 * Return: true when the request was added to the batch, otherwise false
 */
static inline bool blk_mq_add_to_batch(struct request *req,
				       struct io_comp_batch *iob, bool is_error,
				       void (*complete)(struct io_comp_batch *))
{
	/*
	 * Check various conditions that exclude batch processing:
	 * 1) No batch container
	 * 2) Has scheduler data attached
	 * 3) Not a passthrough request and end_io set
	 * 4) Not a passthrough request and failed with an error
	 */
	if (!iob)
		return false;
	if (req->rq_flags & RQF_SCHED_TAGS)
		return false;
	if (!blk_rq_is_passthrough(req)) {
		if (req->end_io)
			return false;
		if (is_error)
			return false;
	}

	if (!iob->complete)
		iob->complete = complete;
	else if (iob->complete != complete)
		return false;
	iob->need_ts |= blk_mq_need_time_stamp(req);
	rq_list_add_tail(&iob->req_list, req);
	return true;
}

void blk_mq_requeue_request(struct request *rq, bool kick_requeue_list);
void blk_mq_kick_requeue_list(struct request_queue *q);
void blk_mq_delay_kick_requeue_list(struct request_queue *q, unsigned long msecs);
void blk_mq_complete_request(struct request *rq);
bool blk_mq_complete_request_remote(struct request *rq);
void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx);
void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx);
void blk_mq_stop_hw_queues(struct request_queue *q);
void blk_mq_start_hw_queues(struct request_queue *q);
void blk_mq_start_stopped_hw_queue(struct blk_mq_hw_ctx *hctx, bool async);
void blk_mq_start_stopped_hw_queues(struct request_queue *q, bool async);
void blk_mq_quiesce_queue(struct request_queue *q);
void blk_mq_wait_quiesce_done(struct blk_mq_tag_set *set);
void blk_mq_quiesce_tagset(struct blk_mq_tag_set *set);
void blk_mq_unquiesce_tagset(struct blk_mq_tag_set *set);
void blk_mq_unquiesce_queue(struct request_queue *q);
void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs);
void blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async);
void blk_mq_run_hw_queues(struct request_queue *q, bool async);
void blk_mq_delay_run_hw_queues(struct request_queue *q, unsigned long msecs);
void blk_mq_tagset_busy_iter(struct blk_mq_tag_set *tagset,
		busy_tag_iter_fn *fn, void *priv);
void blk_mq_tagset_wait_completed_request(struct blk_mq_tag_set *tagset);
void blk_mq_freeze_queue_nomemsave(struct request_queue *q);
void blk_mq_unfreeze_queue_nomemrestore(struct request_queue *q);
static inline unsigned int __must_check
blk_mq_freeze_queue(struct request_queue *q)
{
	unsigned int memflags = memalloc_noio_save();

	blk_mq_freeze_queue_nomemsave(q);
	return memflags;
}
static inline void
blk_mq_unfreeze_queue(struct request_queue *q, unsigned int memflags)
{
	blk_mq_unfreeze_queue_nomemrestore(q);
	memalloc_noio_restore(memflags);
}
void blk_freeze_queue_start(struct request_queue *q);
void blk_mq_freeze_queue_wait(struct request_queue *q);
int blk_mq_freeze_queue_wait_timeout(struct request_queue *q,
				     unsigned long timeout);
void blk_mq_unfreeze_queue_non_owner(struct request_queue *q);
void blk_freeze_queue_start_non_owner(struct request_queue *q);

unsigned int blk_mq_num_possible_queues(unsigned int max_queues);
unsigned int blk_mq_num_online_queues(unsigned int max_queues);
void blk_mq_map_queues(struct blk_mq_queue_map *qmap);
void blk_mq_map_hw_queues(struct blk_mq_queue_map *qmap,
			  struct device *dev, unsigned int offset);
void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues);

void blk_mq_quiesce_queue_nowait(struct request_queue *q);

unsigned int blk_mq_rq_cpu(struct request *rq);

bool __blk_should_fake_timeout(struct request_queue *q);
static inline bool blk_should_fake_timeout(struct request_queue *q)
{
	if (IS_ENABLED(CONFIG_FAIL_IO_TIMEOUT) &&
	    test_bit(QUEUE_FLAG_FAIL_IO, &q->queue_flags))
		return __blk_should_fake_timeout(q);
	return false;
}

/**
 * blk_mq_rq_from_pdu - cast a PDU to a request
 * @pdu: the PDU (Protocol Data Unit) to be casted
 *
 * Return: request
 *
 * Driver command data is immediately after the request. So subtract request
 * size to get back to the original request.
 */
static inline struct request *blk_mq_rq_from_pdu(void *pdu)
{
	return pdu - sizeof(struct request);
}

/**
 * blk_mq_rq_to_pdu - cast a request to a PDU
 * @rq: the request to be casted
 *
 * Return: pointer to the PDU
 *
 * Driver command data is immediately after the request. So add request to get
 * the PDU.
 */
static inline void *blk_mq_rq_to_pdu(struct request *rq)
{
	return rq + 1;
}

#define queue_for_each_hw_ctx(q, hctx, i)				\
	xa_for_each(&(q)->hctx_table, (i), (hctx))

#define hctx_for_each_ctx(hctx, ctx, i)					\
	for ((i) = 0; (i) < (hctx)->nr_ctx &&				\
	     ({ ctx = (hctx)->ctxs[(i)]; 1; }); (i)++)

static inline void blk_mq_cleanup_rq(struct request *rq)
{
	if (rq->q->mq_ops->cleanup_rq)
		rq->q->mq_ops->cleanup_rq(rq);
}

void blk_mq_hctx_set_fq_lock_class(struct blk_mq_hw_ctx *hctx,
		struct lock_class_key *key);

static inline bool rq_is_sync(struct request *rq)
{
	return op_is_sync(rq->cmd_flags);
}

void blk_rq_init(struct request_queue *q, struct request *rq);
int blk_rq_prep_clone(struct request *rq, struct request *rq_src,
		struct bio_set *bs, gfp_t gfp_mask,
		int (*bio_ctr)(struct bio *, struct bio *, void *), void *data);
void blk_rq_unprep_clone(struct request *rq);
blk_status_t blk_insert_cloned_request(struct request *rq);

struct rq_map_data {
	struct page **pages;
	unsigned long offset;
	unsigned short page_order;
	unsigned short nr_entries;
	bool null_mapped;
	bool from_user;
};

int blk_rq_map_user(struct request_queue *, struct request *,
		struct rq_map_data *, void __user *, unsigned long, gfp_t);
int blk_rq_map_user_io(struct request *, struct rq_map_data *,
		void __user *, unsigned long, gfp_t, bool, int, bool, int);
int blk_rq_map_user_iov(struct request_queue *, struct request *,
		struct rq_map_data *, const struct iov_iter *, gfp_t);
int blk_rq_unmap_user(struct bio *);
int blk_rq_map_kern(struct request *rq, void *kbuf, unsigned int len,
		gfp_t gfp);
int blk_rq_append_bio(struct request *rq, struct bio *bio);
void blk_execute_rq_nowait(struct request *rq, bool at_head);
blk_status_t blk_execute_rq(struct request *rq, bool at_head);
bool blk_rq_is_poll(struct request *rq);

struct req_iterator {
	struct bvec_iter iter;
	struct bio *bio;
};

#define __rq_for_each_bio(_bio, rq)	\
	if ((rq->bio))			\
		for (_bio = (rq)->bio; _bio; _bio = _bio->bi_next)

#define rq_for_each_segment(bvl, _rq, _iter)			\
	__rq_for_each_bio(_iter.bio, _rq)			\
		bio_for_each_segment(bvl, _iter.bio, _iter.iter)

#define rq_for_each_bvec(bvl, _rq, _iter)			\
	__rq_for_each_bio(_iter.bio, _rq)			\
		bio_for_each_bvec(bvl, _iter.bio, _iter.iter)

#define rq_iter_last(bvec, _iter)				\
		(_iter.bio->bi_next == NULL &&			\
		 bio_iter_last(bvec, _iter.iter))

/*
 * blk_rq_pos()			: the current sector
 * blk_rq_bytes()		: bytes left in the entire request
 * blk_rq_cur_bytes()		: bytes left in the current segment
 * blk_rq_sectors()		: sectors left in the entire request
 * blk_rq_cur_sectors()		: sectors left in the current segment
 * blk_rq_stats_sectors()	: sectors of the entire request used for stats
 */
static inline sector_t blk_rq_pos(const struct request *rq)
{
	return rq->__sector;
}

static inline unsigned int blk_rq_bytes(const struct request *rq)
{
	return rq->__data_len;
}

static inline int blk_rq_cur_bytes(const struct request *rq)
{
	if (!rq->bio)
		return 0;
	if (!bio_has_data(rq->bio))	/* dataless requests such as discard */
		return rq->bio->bi_iter.bi_size;
	return bio_iovec(rq->bio).bv_len;
}

static inline unsigned int blk_rq_sectors(const struct request *rq)
{
	return blk_rq_bytes(rq) >> SECTOR_SHIFT;
}

static inline unsigned int blk_rq_cur_sectors(const struct request *rq)
{
	return blk_rq_cur_bytes(rq) >> SECTOR_SHIFT;
}

static inline unsigned int blk_rq_stats_sectors(const struct request *rq)
{
	return rq->stats_sectors;
}

/*
 * Some commands like WRITE SAME have a payload or data transfer size which
 * is different from the size of the request.  Any driver that supports such
 * commands using the RQF_SPECIAL_PAYLOAD flag needs to use this helper to
 * calculate the data transfer size.
 */
static inline unsigned int blk_rq_payload_bytes(struct request *rq)
{
	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD)
		return rq->special_vec.bv_len;
	return blk_rq_bytes(rq);
}

/*
 * Return the first full biovec in the request.  The caller needs to check that
 * there are any bvecs before calling this helper.
 */
static inline struct bio_vec req_bvec(struct request *rq)
{
	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD)
		return rq->special_vec;
	return mp_bvec_iter_bvec(rq->bio->bi_io_vec, rq->bio->bi_iter);
}

static inline unsigned int blk_rq_count_bios(struct request *rq)
{
	unsigned int nr_bios = 0;
	struct bio *bio;

	__rq_for_each_bio(bio, rq)
		nr_bios++;

	return nr_bios;
}

void blk_steal_bios(struct bio_list *list, struct request *rq);

/*
 * Request completion related functions.
 *
 * blk_update_request() completes given number of bytes and updates
 * the request without completing it.
 */
bool blk_update_request(struct request *rq, blk_status_t error,
			       unsigned int nr_bytes);
void blk_abort_request(struct request *);

/*
 * Number of physical segments as sent to the device.
 *
 * Normally this is the number of discontiguous data segments sent by the
 * submitter.  But for data-less command like discard we might have no
 * actual data segments submitted, but the driver might have to add it's
 * own special payload.  In that case we still return 1 here so that this
 * special payload will be mapped.
 */
static inline unsigned short blk_rq_nr_phys_segments(struct request *rq)
{
	if (rq->rq_flags & RQF_SPECIAL_PAYLOAD)
		return 1;
	return rq->nr_phys_segments;
}

/*
 * Number of discard segments (or ranges) the driver needs to fill in.
 * Each discard bio merged into a request is counted as one segment.
 */
static inline unsigned short blk_rq_nr_discard_segments(struct request *rq)
{
	return max_t(unsigned short, rq->nr_phys_segments, 1);
}

int __blk_rq_map_sg(struct request *rq, struct scatterlist *sglist,
		struct scatterlist **last_sg);
static inline int blk_rq_map_sg(struct request *rq, struct scatterlist *sglist)
{
	struct scatterlist *last_sg = NULL;

	return __blk_rq_map_sg(rq, sglist, &last_sg);
}
void blk_dump_rq_flags(struct request *, char *);

#endif /* BLK_MQ_H */
