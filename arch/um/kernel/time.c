// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015 Anton Ivanov (aivanov@{brocade.com,kot-begemot.co.uk})
 * Copyright (C) 2015 Thomas Meyer (thomas@m3y3r.de)
 * Copyright (C) 2012-2014 Cisco Systems
 * Copyright (C) 2000 - 2007 Jeff Dike (jdike@{addtoit,linux.intel}.com)
 * Copyright (C) 2019 Intel Corporation
 */

#include <linux/clockchips.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/threads.h>
#include <asm/irq.h>
#include <asm/param.h>
#include <kern_util.h>
#include <os.h>
#include <linux/delay.h>
#include <linux/time-internal.h>
#include <linux/um_timetravel.h>
#include <shared/init.h>

#ifdef CONFIG_UML_TIME_TRAVEL_SUPPORT
#include <linux/sched/clock.h>

enum time_travel_mode time_travel_mode;
EXPORT_SYMBOL_GPL(time_travel_mode);

static bool time_travel_start_set;
static unsigned long long time_travel_start;
static unsigned long long time_travel_time;
static unsigned long long time_travel_shm_offset;
static LIST_HEAD(time_travel_events);
static LIST_HEAD(time_travel_irqs);
static unsigned long long time_travel_timer_interval;
static unsigned long long time_travel_next_event;
static struct time_travel_event time_travel_timer_event;
static int time_travel_ext_fd = -1;
static unsigned int time_travel_ext_waiting;
static bool time_travel_ext_prev_request_valid;
static unsigned long long time_travel_ext_prev_request;
static unsigned long long *time_travel_ext_free_until;
static unsigned long long _time_travel_ext_free_until;
static u16 time_travel_shm_id;
static struct um_timetravel_schedshm *time_travel_shm;
static union um_timetravel_schedshm_client *time_travel_shm_client;

unsigned long tt_extra_sched_jiffies;

notrace unsigned long long sched_clock(void)
{
	return (unsigned long long)(jiffies - INITIAL_JIFFIES +
				    tt_extra_sched_jiffies)
					* (NSEC_PER_SEC / HZ);
}

static void time_travel_set_time(unsigned long long ns)
{
	if (unlikely(ns < time_travel_time))
		panic("time-travel: time goes backwards %lld -> %lld\n",
		      time_travel_time, ns);
	else if (unlikely(ns >= S64_MAX))
		panic("The system was going to sleep forever, aborting");

	time_travel_time = ns;
}

enum time_travel_message_handling {
	TTMH_IDLE,
	TTMH_POLL,
	TTMH_READ,
	TTMH_READ_START_ACK,
};

static u64 bc_message;
int time_travel_should_print_bc_msg;

void _time_travel_print_bc_msg(void)
{
	time_travel_should_print_bc_msg = 0;
	printk(KERN_INFO "time-travel: received broadcast 0x%llx\n", bc_message);
}

static void time_travel_setup_shm(int fd, u16 id)
{
	u32 len;

	time_travel_shm = os_mmap_rw_shared(fd, sizeof(*time_travel_shm));

	if (!time_travel_shm)
		goto out;

	len = time_travel_shm->len;

	if (time_travel_shm->version != UM_TIMETRAVEL_SCHEDSHM_VERSION ||
	    len < struct_size(time_travel_shm, clients, id + 1)) {
		os_unmap_memory(time_travel_shm, sizeof(*time_travel_shm));
		time_travel_shm = NULL;
		goto out;
	}

	time_travel_shm = os_mremap_rw_shared(time_travel_shm,
					      sizeof(*time_travel_shm),
					      len);
	if (!time_travel_shm)
		goto out;

	time_travel_shm_offset = time_travel_shm->current_time;
	time_travel_shm_client = &time_travel_shm->clients[id];
	time_travel_shm_client->capa |= UM_TIMETRAVEL_SCHEDSHM_CAP_TIME_SHARE;
	time_travel_shm_id = id;
	/* always look at that free_until from now on */
	time_travel_ext_free_until = &time_travel_shm->free_until;
out:
	os_close_file(fd);
}

static void time_travel_handle_message(struct um_timetravel_msg *msg,
				       enum time_travel_message_handling mode)
{
	struct um_timetravel_msg resp = {
		.op = UM_TIMETRAVEL_ACK,
	};
	int ret;

	/*
	 * We can't unlock here, but interrupt signals with a timetravel_handler
	 * (see um_request_irq_tt) get to the timetravel_handler anyway.
	 */
	if (mode != TTMH_READ) {
		BUG_ON(mode == TTMH_IDLE && !irqs_disabled());

		while (os_poll(1, &time_travel_ext_fd) != 0) {
			/* nothing */
		}
	}

	if (unlikely(mode == TTMH_READ_START_ACK)) {
		int fd[UM_TIMETRAVEL_SHARED_MAX_FDS];

		ret = os_rcv_fd_msg(time_travel_ext_fd, fd,
				    ARRAY_SIZE(fd), msg, sizeof(*msg));
		if (ret == sizeof(*msg)) {
			time_travel_setup_shm(fd[UM_TIMETRAVEL_SHARED_MEMFD],
					      msg->time & UM_TIMETRAVEL_START_ACK_ID);
			/* we don't use the logging for now */
			os_close_file(fd[UM_TIMETRAVEL_SHARED_LOGFD]);
		}
	} else {
		ret = os_read_file(time_travel_ext_fd, msg, sizeof(*msg));
	}

	if (ret == 0)
		panic("time-travel external link is broken\n");
	if (ret != sizeof(*msg))
		panic("invalid time-travel message - %d bytes\n", ret);

	switch (msg->op) {
	default:
		WARN_ONCE(1, "time-travel: unexpected message %lld\n",
			  (unsigned long long)msg->op);
		break;
	case UM_TIMETRAVEL_ACK:
		return;
	case UM_TIMETRAVEL_RUN:
		time_travel_set_time(msg->time);
		if (time_travel_shm) {
			/* no request right now since we're running */
			time_travel_shm_client->flags &=
				~UM_TIMETRAVEL_SCHEDSHM_FLAGS_REQ_RUN;
			/* no ack for shared memory RUN */
			return;
		}
		break;
	case UM_TIMETRAVEL_FREE_UNTIL:
		/* not supposed to get this with shm, but ignore it */
		if (time_travel_shm)
			break;
		time_travel_ext_free_until = &_time_travel_ext_free_until;
		_time_travel_ext_free_until = msg->time;
		break;
	case UM_TIMETRAVEL_BROADCAST:
		bc_message = msg->time;
		time_travel_should_print_bc_msg = 1;
		break;
	}

	resp.seq = msg->seq;
	os_write_file(time_travel_ext_fd, &resp, sizeof(resp));
}

static u64 time_travel_ext_req(u32 op, u64 time)
{
	static int seq;
	int mseq = ++seq;
	struct um_timetravel_msg msg = {
		.op = op,
		.time = time,
		.seq = mseq,
	};

	/*
	 * We need to block even the timetravel handlers of SIGIO here and
	 * only restore their use when we got the ACK - otherwise we may
	 * (will) get interrupted by that, try to queue the IRQ for future
	 * processing and thus send another request while we're still waiting
	 * for an ACK, but the peer doesn't know we got interrupted and will
	 * send the ACKs in the same order as the message, but we'd need to
	 * see them in the opposite order ...
	 *
	 * This wouldn't matter *too* much, but some ACKs carry the
	 * current time (for UM_TIMETRAVEL_GET) and getting another
	 * ACK without a time would confuse us a lot!
	 *
	 * The sequence number assignment that happens here lets us
	 * debug such message handling issues more easily.
	 */
	block_signals_hard();
	os_write_file(time_travel_ext_fd, &msg, sizeof(msg));

	/* no ACK expected for WAIT in shared memory mode */
	if (msg.op == UM_TIMETRAVEL_WAIT && time_travel_shm)
		goto done;

	while (msg.op != UM_TIMETRAVEL_ACK)
		time_travel_handle_message(&msg,
					   op == UM_TIMETRAVEL_START ?
						TTMH_READ_START_ACK :
						TTMH_READ);

	if (msg.seq != mseq)
		panic("time-travel: ACK message has different seqno! op=%d, seq=%d != %d time=%lld\n",
		      msg.op, msg.seq, mseq, msg.time);

	if (op == UM_TIMETRAVEL_GET)
		time_travel_set_time(msg.time);
done:
	unblock_signals_hard();

	return msg.time;
}

void __time_travel_wait_readable(int fd)
{
	int fds[2] = { fd, time_travel_ext_fd };
	int ret;

	if (time_travel_mode != TT_MODE_EXTERNAL)
		return;

	while ((ret = os_poll(2, fds))) {
		struct um_timetravel_msg msg;

		if (ret == 1)
			time_travel_handle_message(&msg, TTMH_READ);
	}
}
EXPORT_SYMBOL_GPL(__time_travel_wait_readable);

static void time_travel_ext_update_request(unsigned long long time)
{
	if (time_travel_mode != TT_MODE_EXTERNAL)
		return;

	/* asked for exactly this time previously */
	if (time_travel_ext_prev_request_valid &&
	    time == time_travel_ext_prev_request)
		return;

	/*
	 * if we're running and are allowed to run past the request
	 * then we don't need to update it either
	 *
	 * Note for shm we ignore FREE_UNTIL messages and leave the pointer
	 * to shared memory, and for non-shm the offset is 0.
	 */
	if (!time_travel_ext_waiting && time_travel_ext_free_until &&
	    time < (*time_travel_ext_free_until - time_travel_shm_offset))
		return;

	time_travel_ext_prev_request = time;
	time_travel_ext_prev_request_valid = true;

	if (time_travel_shm) {
		union um_timetravel_schedshm_client *running;

		running = &time_travel_shm->clients[time_travel_shm->running_id];

		if (running->capa & UM_TIMETRAVEL_SCHEDSHM_CAP_TIME_SHARE) {
			time_travel_shm_client->flags |=
				UM_TIMETRAVEL_SCHEDSHM_FLAGS_REQ_RUN;
			time += time_travel_shm_offset;
			time_travel_shm_client->req_time = time;
			if (time < time_travel_shm->free_until)
				time_travel_shm->free_until = time;
			return;
		}
	}

	time_travel_ext_req(UM_TIMETRAVEL_REQUEST, time);
}

void __time_travel_propagate_time(void)
{
	static unsigned long long last_propagated;

	if (time_travel_shm) {
		if (time_travel_shm->running_id != time_travel_shm_id)
			panic("time-travel: setting time while not running\n");
		time_travel_shm->current_time = time_travel_time +
						time_travel_shm_offset;
		return;
	}

	if (last_propagated == time_travel_time)
		return;

	time_travel_ext_req(UM_TIMETRAVEL_UPDATE, time_travel_time);
	last_propagated = time_travel_time;
}
EXPORT_SYMBOL_GPL(__time_travel_propagate_time);

/* returns true if we must do a wait to the simtime device */
static bool time_travel_ext_request(unsigned long long time)
{
	/*
	 * If we received an external sync point ("free until") then we
	 * don't have to request/wait for anything until then, unless
	 * we're already waiting.
	 *
	 * Note for shm we ignore FREE_UNTIL messages and leave the pointer
	 * to shared memory, and for non-shm the offset is 0.
	 */
	if (!time_travel_ext_waiting && time_travel_ext_free_until &&
	    time < (*time_travel_ext_free_until - time_travel_shm_offset))
		return false;

	time_travel_ext_update_request(time);
	return true;
}

static void time_travel_ext_wait(bool idle)
{
	struct um_timetravel_msg msg = {
		.op = UM_TIMETRAVEL_ACK,
	};

	time_travel_ext_prev_request_valid = false;
	if (!time_travel_shm)
		time_travel_ext_free_until = NULL;
	time_travel_ext_waiting++;

	time_travel_ext_req(UM_TIMETRAVEL_WAIT, -1);

	/*
	 * Here we are deep in the idle loop, so we have to break out of the
	 * kernel abstraction in a sense and implement this in terms of the
	 * UML system waiting on the VQ interrupt while sleeping, when we get
	 * the signal it'll call time_travel_ext_vq_notify_done() completing the
	 * call.
	 */
	while (msg.op != UM_TIMETRAVEL_RUN)
		time_travel_handle_message(&msg, idle ? TTMH_IDLE : TTMH_POLL);

	time_travel_ext_waiting--;

	/* we might request more stuff while polling - reset when we run */
	time_travel_ext_prev_request_valid = false;
}

static void time_travel_ext_get_time(void)
{
	if (time_travel_shm)
		time_travel_set_time(time_travel_shm->current_time -
				     time_travel_shm_offset);
	else
		time_travel_ext_req(UM_TIMETRAVEL_GET, -1);
}

static void __time_travel_update_time(unsigned long long ns, bool idle)
{
	if (time_travel_mode == TT_MODE_EXTERNAL && time_travel_ext_request(ns))
		time_travel_ext_wait(idle);
	else
		time_travel_set_time(ns);
}

static struct time_travel_event *time_travel_first_event(void)
{
	return list_first_entry_or_null(&time_travel_events,
					struct time_travel_event,
					list);
}

static void __time_travel_add_event(struct time_travel_event *e,
				    unsigned long long time)
{
	struct time_travel_event *tmp;
	bool inserted = false;
	unsigned long flags;

	if (e->pending)
		return;

	e->pending = true;
	e->time = time;

	local_irq_save(flags);
	list_for_each_entry(tmp, &time_travel_events, list) {
		/*
		 * Add the new entry before one with higher time,
		 * or if they're equal and both on stack, because
		 * in that case we need to unwind the stack in the
		 * right order, and the later event (timer sleep
		 * or such) must be dequeued first.
		 */
		if ((tmp->time > e->time) ||
		    (tmp->time == e->time && tmp->onstack && e->onstack)) {
			list_add_tail(&e->list, &tmp->list);
			inserted = true;
			break;
		}
	}

	if (!inserted)
		list_add_tail(&e->list, &time_travel_events);

	tmp = time_travel_first_event();
	time_travel_ext_update_request(tmp->time);
	time_travel_next_event = tmp->time;
	local_irq_restore(flags);
}

static void time_travel_add_event(struct time_travel_event *e,
				  unsigned long long time)
{
	if (WARN_ON(!e->fn))
		return;

	__time_travel_add_event(e, time);
}

void time_travel_add_event_rel(struct time_travel_event *e,
			       unsigned long long delay_ns)
{
	time_travel_add_event(e, time_travel_time + delay_ns);
}

static void time_travel_periodic_timer(struct time_travel_event *e)
{
	time_travel_add_event(&time_travel_timer_event,
			      time_travel_time + time_travel_timer_interval);

	/* clock tick; decrease extra jiffies by keeping sched_clock constant */
	if (tt_extra_sched_jiffies > 0)
		tt_extra_sched_jiffies -= 1;

	deliver_alarm();
}

void deliver_time_travel_irqs(void)
{
	struct time_travel_event *e;
	unsigned long flags;

	/*
	 * Don't do anything for most cases. Note that because here we have
	 * to disable IRQs (and re-enable later) we'll actually recurse at
	 * the end of the function, so this is strictly necessary.
	 */
	if (likely(list_empty(&time_travel_irqs)))
		return;

	local_irq_save(flags);
	irq_enter();
	while ((e = list_first_entry_or_null(&time_travel_irqs,
					     struct time_travel_event,
					     list))) {
		list_del(&e->list);
		e->pending = false;
		e->fn(e);
	}
	irq_exit();
	local_irq_restore(flags);
}

static void time_travel_deliver_event(struct time_travel_event *e)
{
	if (e == &time_travel_timer_event) {
		/*
		 * deliver_alarm() does the irq_enter/irq_exit
		 * by itself, so must handle it specially here
		 */
		e->fn(e);
	} else if (irqs_disabled()) {
		list_add_tail(&e->list, &time_travel_irqs);
		/*
		 * set pending again, it was set to false when the
		 * event was deleted from the original list, but
		 * now it's still pending until we deliver the IRQ.
		 */
		e->pending = true;
	} else {
		unsigned long flags;

		local_irq_save(flags);
		irq_enter();
		e->fn(e);
		irq_exit();
		local_irq_restore(flags);
	}
}

bool time_travel_del_event(struct time_travel_event *e)
{
	unsigned long flags;

	if (!e->pending)
		return false;
	local_irq_save(flags);
	list_del(&e->list);
	e->pending = false;
	local_irq_restore(flags);
	return true;
}

static void time_travel_update_time(unsigned long long next, bool idle)
{
	struct time_travel_event ne = {
		.onstack = true,
	};
	struct time_travel_event *e;
	bool finished = idle;

	/* add it without a handler - we deal with that specifically below */
	__time_travel_add_event(&ne, next);

	do {
		e = time_travel_first_event();

		BUG_ON(!e);
		__time_travel_update_time(e->time, idle);

		/* new events may have been inserted while we were waiting */
		if (e == time_travel_first_event()) {
			BUG_ON(!time_travel_del_event(e));
			BUG_ON(time_travel_time != e->time);

			if (e == &ne) {
				finished = true;
			} else {
				if (e->onstack)
					panic("On-stack event dequeued outside of the stack! time=%lld, event time=%lld, event=%pS\n",
					      time_travel_time, e->time, e);
				time_travel_deliver_event(e);
			}
		}

		e = time_travel_first_event();
		if (e)
			time_travel_ext_update_request(e->time);
	} while (ne.pending && !finished);

	time_travel_del_event(&ne);
}

static void time_travel_update_time_rel(unsigned long long offs)
{
	unsigned long flags;

	/*
	 * Disable interrupts before calculating the new time so
	 * that a real timer interrupt (signal) can't happen at
	 * a bad time e.g. after we read time_travel_time but
	 * before we've completed updating the time.
	 */
	local_irq_save(flags);
	time_travel_update_time(time_travel_time + offs, false);
	local_irq_restore(flags);
}

void time_travel_ndelay(unsigned long nsec)
{
	/*
	 * Not strictly needed to use _rel() version since this is
	 * only used in INFCPU/EXT modes, but it doesn't hurt and
	 * is more readable too.
	 */
	time_travel_update_time_rel(nsec);
}
EXPORT_SYMBOL(time_travel_ndelay);

void time_travel_add_irq_event(struct time_travel_event *e)
{
	BUG_ON(time_travel_mode != TT_MODE_EXTERNAL);

	time_travel_ext_get_time();
	/*
	 * We could model interrupt latency here, for now just
	 * don't have any latency at all and request the exact
	 * same time (again) to run the interrupt...
	 */
	time_travel_add_event(e, time_travel_time);
}
EXPORT_SYMBOL_GPL(time_travel_add_irq_event);

static void time_travel_oneshot_timer(struct time_travel_event *e)
{
	/* clock tick; decrease extra jiffies by keeping sched_clock constant */
	if (tt_extra_sched_jiffies > 0)
		tt_extra_sched_jiffies -= 1;

	deliver_alarm();
}

void time_travel_sleep(void)
{
	/*
	 * Wait "forever" (using S64_MAX because there are some potential
	 * wrapping issues, especially with the current TT_MODE_EXTERNAL
	 * controller application.
	 */
	unsigned long long next = S64_MAX;

	if (time_travel_mode == TT_MODE_BASIC)
		os_timer_disable();

	time_travel_update_time(next, true);

	if (time_travel_mode == TT_MODE_BASIC &&
	    time_travel_timer_event.pending) {
		if (time_travel_timer_event.fn == time_travel_periodic_timer) {
			/*
			 * This is somewhat wrong - we should get the first
			 * one sooner like the os_timer_one_shot() below...
			 */
			os_timer_set_interval(time_travel_timer_interval);
		} else {
			os_timer_one_shot(time_travel_timer_event.time - next);
		}
	}
}

static void time_travel_handle_real_alarm(void)
{
	time_travel_set_time(time_travel_next_event);

	time_travel_del_event(&time_travel_timer_event);

	if (time_travel_timer_event.fn == time_travel_periodic_timer)
		time_travel_add_event(&time_travel_timer_event,
				      time_travel_time +
				      time_travel_timer_interval);
}

static void time_travel_set_interval(unsigned long long interval)
{
	time_travel_timer_interval = interval;
}

static int time_travel_connect_external(const char *socket)
{
	const char *sep;
	unsigned long long id = (unsigned long long)-1;
	int rc;

	if ((sep = strchr(socket, ':'))) {
		char buf[25] = {};
		if (sep - socket > sizeof(buf) - 1)
			goto invalid_number;

		memcpy(buf, socket, sep - socket);
		if (kstrtoull(buf, 0, &id)) {
invalid_number:
			panic("time-travel: invalid external ID in string '%s'\n",
			      socket);
			return -EINVAL;
		}

		socket = sep + 1;
	}

	rc = os_connect_socket(socket);
	if (rc < 0) {
		panic("time-travel: failed to connect to external socket %s\n",
		      socket);
		return rc;
	}

	time_travel_ext_fd = rc;

	time_travel_ext_req(UM_TIMETRAVEL_START, id);

	return 1;
}

static void time_travel_set_start(void)
{
	if (time_travel_start_set)
		return;

	switch (time_travel_mode) {
	case TT_MODE_EXTERNAL:
		time_travel_start = time_travel_ext_req(UM_TIMETRAVEL_GET_TOD, -1);
		/* controller gave us the *current* time, so adjust by that */
		time_travel_ext_get_time();
		time_travel_start -= time_travel_time;
		break;
	case TT_MODE_INFCPU:
	case TT_MODE_BASIC:
		if (!time_travel_start_set)
			time_travel_start = os_persistent_clock_emulation();
		break;
	case TT_MODE_OFF:
		/* we just read the host clock with os_persistent_clock_emulation() */
		break;
	}

	time_travel_start_set = true;
}
#else /* CONFIG_UML_TIME_TRAVEL_SUPPORT */
#define time_travel_start_set 0
#define time_travel_start 0
#define time_travel_time 0
#define time_travel_ext_waiting 0

static inline void time_travel_update_time(unsigned long long ns, bool idle)
{
}

static inline void time_travel_update_time_rel(unsigned long long offs)
{
}

static inline void time_travel_handle_real_alarm(void)
{
}

static void time_travel_set_interval(unsigned long long interval)
{
}

static inline void time_travel_set_start(void)
{
}

/* fail link if this actually gets used */
extern u64 time_travel_ext_req(u32 op, u64 time);

/* these are empty macros so the struct/fn need not exist */
#define time_travel_add_event(e, time) do { } while (0)
/* externally not usable - redefine here so we can */
#undef time_travel_del_event
#define time_travel_del_event(e) do { } while (0)
#endif

void timer_handler(int sig, struct siginfo *unused_si, struct uml_pt_regs *regs)
{
	unsigned long flags;

	/*
	 * In basic time-travel mode we still get real interrupts
	 * (signals) but since we don't read time from the OS, we
	 * must update the simulated time here to the expiry when
	 * we get a signal.
	 * This is not the case in inf-cpu mode, since there we
	 * never get any real signals from the OS.
	 */
	if (time_travel_mode == TT_MODE_BASIC)
		time_travel_handle_real_alarm();

	local_irq_save(flags);
	do_IRQ(TIMER_IRQ, regs);
	local_irq_restore(flags);
}

static int itimer_shutdown(struct clock_event_device *evt)
{
	if (time_travel_mode != TT_MODE_OFF)
		time_travel_del_event(&time_travel_timer_event);

	if (time_travel_mode != TT_MODE_INFCPU &&
	    time_travel_mode != TT_MODE_EXTERNAL)
		os_timer_disable();

	return 0;
}

static int itimer_set_periodic(struct clock_event_device *evt)
{
	unsigned long long interval = NSEC_PER_SEC / HZ;

	if (time_travel_mode != TT_MODE_OFF) {
		time_travel_del_event(&time_travel_timer_event);
		time_travel_set_event_fn(&time_travel_timer_event,
					 time_travel_periodic_timer);
		time_travel_set_interval(interval);
		time_travel_add_event(&time_travel_timer_event,
				      time_travel_time + interval);
	}

	if (time_travel_mode != TT_MODE_INFCPU &&
	    time_travel_mode != TT_MODE_EXTERNAL)
		os_timer_set_interval(interval);

	return 0;
}

static int itimer_next_event(unsigned long delta,
			     struct clock_event_device *evt)
{
	delta += 1;

	if (time_travel_mode != TT_MODE_OFF) {
		time_travel_del_event(&time_travel_timer_event);
		time_travel_set_event_fn(&time_travel_timer_event,
					 time_travel_oneshot_timer);
		time_travel_add_event(&time_travel_timer_event,
				      time_travel_time + delta);
	}

	if (time_travel_mode != TT_MODE_INFCPU &&
	    time_travel_mode != TT_MODE_EXTERNAL)
		return os_timer_one_shot(delta);

	return 0;
}

static int itimer_one_shot(struct clock_event_device *evt)
{
	return itimer_next_event(0, evt);
}

static struct clock_event_device timer_clockevent = {
	.name			= "posix-timer",
	.rating			= 250,
	.cpumask		= cpu_possible_mask,
	.features		= CLOCK_EVT_FEAT_PERIODIC |
				  CLOCK_EVT_FEAT_ONESHOT,
	.set_state_shutdown	= itimer_shutdown,
	.set_state_periodic	= itimer_set_periodic,
	.set_state_oneshot	= itimer_one_shot,
	.set_next_event		= itimer_next_event,
	.shift			= 0,
	.max_delta_ns		= 0xffffffff,
	.max_delta_ticks	= 0xffffffff,
	.min_delta_ns		= TIMER_MIN_DELTA,
	.min_delta_ticks	= TIMER_MIN_DELTA, // microsecond resolution should be enough for anyone, same as 640K RAM
	.irq			= 0,
	.mult			= 1,
};

static irqreturn_t um_timer(int irq, void *dev)
{
	/*
	 * Interrupt the (possibly) running userspace process, technically this
	 * should only happen if userspace is currently executing.
	 * With infinite CPU time-travel, we can only get here when userspace
	 * is not executing. Do not notify there and avoid spurious scheduling.
	 */
	if (time_travel_mode != TT_MODE_INFCPU &&
	    time_travel_mode != TT_MODE_EXTERNAL &&
	    get_current()->mm)
		os_alarm_process(get_current()->mm->context.id.pid);

	(*timer_clockevent.event_handler)(&timer_clockevent);

	return IRQ_HANDLED;
}

static u64 timer_read(struct clocksource *cs)
{
	if (time_travel_mode != TT_MODE_OFF) {
		/*
		 * We make reading the timer cost a bit so that we don't get
		 * stuck in loops that expect time to move more than the
		 * exact requested sleep amount, e.g. python's socket server,
		 * see https://bugs.python.org/issue37026.
		 *
		 * However, don't do that when we're in interrupt or such as
		 * then we might recurse into our own processing, and get to
		 * even more waiting, and that's not good - it messes up the
		 * "what do I do next" and onstack event we use to know when
		 * to return from time_travel_update_time().
		 */
		if (!irqs_disabled() && !in_interrupt() && !in_softirq() &&
		    !time_travel_ext_waiting)
			time_travel_update_time_rel(TIMER_MULTIPLIER);
		return time_travel_time / TIMER_MULTIPLIER;
	}

	return os_nsecs() / TIMER_MULTIPLIER;
}

static struct clocksource timer_clocksource = {
	.name		= "timer",
	.rating		= 300,
	.read		= timer_read,
	.mask		= CLOCKSOURCE_MASK(64),
	.flags		= CLOCK_SOURCE_IS_CONTINUOUS,
};

static void __init um_timer_setup(void)
{
	int err;

	err = request_irq(TIMER_IRQ, um_timer, IRQF_TIMER, "hr timer", NULL);
	if (err != 0)
		printk(KERN_ERR "register_timer : request_irq failed - "
		       "errno = %d\n", -err);

	err = os_timer_create();
	if (err != 0) {
		printk(KERN_ERR "creation of timer failed - errno = %d\n", -err);
		return;
	}

	err = clocksource_register_hz(&timer_clocksource, NSEC_PER_SEC/TIMER_MULTIPLIER);
	if (err) {
		printk(KERN_ERR "clocksource_register_hz returned %d\n", err);
		return;
	}
	clockevents_register_device(&timer_clockevent);
}

void read_persistent_clock64(struct timespec64 *ts)
{
	long long nsecs;

	time_travel_set_start();

	if (time_travel_mode != TT_MODE_OFF)
		nsecs = time_travel_start + time_travel_time;
	else
		nsecs = os_persistent_clock_emulation();

	set_normalized_timespec64(ts, nsecs / NSEC_PER_SEC,
				  nsecs % NSEC_PER_SEC);
}

void __init time_init(void)
{
	timer_set_signal_handler();
	late_time_init = um_timer_setup;
}

#ifdef CONFIG_UML_TIME_TRAVEL_SUPPORT
unsigned long calibrate_delay_is_known(void)
{
	if (time_travel_mode == TT_MODE_INFCPU ||
	    time_travel_mode == TT_MODE_EXTERNAL)
		return 1;
	return 0;
}

static int setup_time_travel(char *str)
{
	if (strcmp(str, "=inf-cpu") == 0) {
		time_travel_mode = TT_MODE_INFCPU;
		timer_clockevent.name = "time-travel-timer-infcpu";
		timer_clocksource.name = "time-travel-clock";
		return 1;
	}

	if (strncmp(str, "=ext:", 5) == 0) {
		time_travel_mode = TT_MODE_EXTERNAL;
		timer_clockevent.name = "time-travel-timer-external";
		timer_clocksource.name = "time-travel-clock-external";
		return time_travel_connect_external(str + 5);
	}

	if (!*str) {
		time_travel_mode = TT_MODE_BASIC;
		timer_clockevent.name = "time-travel-timer";
		timer_clocksource.name = "time-travel-clock";
		return 1;
	}

	return -EINVAL;
}

__setup("time-travel", setup_time_travel);
__uml_help(setup_time_travel,
"time-travel\n"
"This option just enables basic time travel mode, in which the clock/timers\n"
"inside the UML instance skip forward when there's nothing to do, rather than\n"
"waiting for real time to elapse. However, instance CPU speed is limited by\n"
"the real CPU speed, so e.g. a 10ms timer will always fire after ~10ms wall\n"
"clock (but quicker when there's nothing to do).\n"
"\n"
"time-travel=inf-cpu\n"
"This enables time travel mode with infinite processing power, in which there\n"
"are no wall clock timers, and any CPU processing happens - as seen from the\n"
"guest - instantly. This can be useful for accurate simulation regardless of\n"
"debug overhead, physical CPU speed, etc. but is somewhat dangerous as it can\n"
"easily lead to getting stuck (e.g. if anything in the system busy loops).\n"
"\n"
"time-travel=ext:[ID:]/path/to/socket\n"
"This enables time travel mode similar to =inf-cpu, except the system will\n"
"use the given socket to coordinate with a central scheduler, in order to\n"
"have more than one system simultaneously be on simulated time. The virtio\n"
"driver code in UML knows about this so you can also simulate networks and\n"
"devices using it, assuming the device has the right capabilities.\n"
"The optional ID is a 64-bit integer that's sent to the central scheduler.\n");

static int setup_time_travel_start(char *str)
{
	int err;

	err = kstrtoull(str, 0, &time_travel_start);
	if (err)
		return err;

	time_travel_start_set = 1;
	return 1;
}

__setup("time-travel-start=", setup_time_travel_start);
__uml_help(setup_time_travel_start,
"time-travel-start=<nanoseconds>\n"
"Configure the UML instance's wall clock to start at this value rather than\n"
"the host's wall clock at the time of UML boot.\n");
static struct kobject *bc_time_kobject;

static ssize_t bc_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "0x%llx", bc_message);
}

static ssize_t bc_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buf, size_t count)
{
	int ret;
	u64 user_bc_message;

	ret = kstrtou64(buf, 0, &user_bc_message);
	if (ret)
		return ret;

	bc_message = user_bc_message;

	time_travel_ext_req(UM_TIMETRAVEL_BROADCAST, bc_message);
	pr_info("um: time: sent broadcast message: 0x%llx\n", bc_message);
	return count;
}

static struct kobj_attribute bc_attribute = __ATTR(bc-message, 0660, bc_show, bc_store);

static int __init um_bc_start(void)
{
	if (time_travel_mode != TT_MODE_EXTERNAL)
		return 0;

	bc_time_kobject = kobject_create_and_add("um-ext-time", kernel_kobj);
	if (!bc_time_kobject)
		return 0;

	if (sysfs_create_file(bc_time_kobject, &bc_attribute.attr))
		pr_debug("failed to create the bc file in /sys/kernel/um_time");

	return 0;
}
late_initcall(um_bc_start);
#endif
