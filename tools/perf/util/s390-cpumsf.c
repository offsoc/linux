// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright IBM Corp. 2018
 * Auxtrace support for s390 CPU-Measurement Sampling Facility
 *
 * Author(s):  Thomas Richter <tmricht@linux.ibm.com>
 *
 * Auxiliary traces are collected during 'perf record' using rbd000 event.
 * Several PERF_RECORD_XXX are generated during recording:
 *
 * PERF_RECORD_AUX:
 *	Records that new data landed in the AUX buffer part.
 * PERF_RECORD_AUXTRACE:
 *	Defines auxtrace data. Followed by the actual data. The contents of
 *	the auxtrace data is dependent on the event and the CPU.
 *	This record is generated by perf record command. For details
 *	see Documentation/perf.data-file-format.txt.
 * PERF_RECORD_AUXTRACE_INFO:
 *	Defines a table of contains for PERF_RECORD_AUXTRACE records. This
 *	record is generated during 'perf record' command. Each record contains
 *	up to 256 entries describing offset and size of the AUXTRACE data in the
 *	perf.data file.
 * PERF_RECORD_AUXTRACE_ERROR:
 *	Indicates an error during AUXTRACE collection such as buffer overflow.
 * PERF_RECORD_FINISHED_ROUND:
 *	Perf events are not necessarily in time stamp order, as they can be
 *	collected in parallel on different CPUs. If the events should be
 *	processed in time order they need to be sorted first.
 *	Perf report guarantees that there is no reordering over a
 *	PERF_RECORD_FINISHED_ROUND boundary event. All perf records with a
 *	time stamp lower than this record are processed (and displayed) before
 *	the succeeding perf record are processed.
 *
 * These records are evaluated during perf report command.
 *
 * 1. PERF_RECORD_AUXTRACE_INFO is used to set up the infrastructure for
 * auxiliary trace data processing. See s390_cpumsf_process_auxtrace_info()
 * below.
 * Auxiliary trace data is collected per CPU. To merge the data into the report
 * an auxtrace_queue is created for each CPU. It is assumed that the auxtrace
 * data is in ascending order.
 *
 * Each queue has a double linked list of auxtrace_buffers. This list contains
 * the offset and size of a CPU's auxtrace data. During auxtrace processing
 * the data portion is mmap()'ed.
 *
 * To sort the queues in chronological order, all queue access is controlled
 * by the auxtrace_heap. This is basically a stack, each stack element has two
 * entries, the queue number and a time stamp. However the stack is sorted by
 * the time stamps. The highest time stamp is at the bottom the lowest
 * (nearest) time stamp is at the top. That sort order is maintained at all
 * times!
 *
 * After the auxtrace infrastructure has been setup, the auxtrace queues are
 * filled with data (offset/size pairs) and the auxtrace_heap is populated.
 *
 * 2. PERF_RECORD_XXX processing triggers access to the auxtrace_queues.
 * Each record is handled by s390_cpumsf_process_event(). The time stamp of
 * the perf record is compared with the time stamp located on the auxtrace_heap
 * top element. If that time stamp is lower than the time stamp from the
 * record sample, the auxtrace queues will be processed. As auxtrace queues
 * control many auxtrace_buffers and each buffer can be quite large, the
 * auxtrace buffer might be processed only partially. In this case the
 * position in the auxtrace_buffer of that queue is remembered and the time
 * stamp of the last processed entry of the auxtrace_buffer replaces the
 * current auxtrace_heap top.
 *
 * 3. Auxtrace_queues might run of out data and are fed by the
 * PERF_RECORD_AUXTRACE handling, see s390_cpumsf_process_auxtrace_event().
 *
 * Event Generation
 * Each sampling-data entry in the auxiliary trace data generates a perf sample.
 * This sample is filled
 * with data from the auxtrace such as PID/TID, instruction address, CPU state,
 * etc. This sample is processed with perf_session__deliver_synth_event() to
 * be included into the GUI.
 *
 * 4. PERF_RECORD_FINISHED_ROUND event is used to process all the remaining
 * auxiliary traces entries until the time stamp of this record is reached
 * auxtrace_heap top. This is triggered by ordered_event->deliver().
 *
 *
 * Perf event processing.
 * Event processing of PERF_RECORD_XXX entries relies on time stamp entries.
 * This is the function call sequence:
 *
 * __cmd_report()
 * |
 * perf_session__process_events()
 * |
 * __perf_session__process_events()
 * |
 * perf_session__process_event()
 * |  This functions splits the PERF_RECORD_XXX records.
 * |  - Those generated by perf record command (type number equal or higher
 * |    than PERF_RECORD_USER_TYPE_START) are handled by
 * |    perf_session__process_user_event(see below)
 * |  - Those generated by the kernel are handled by
 * |    evlist__parse_sample_timestamp()
 * |
 * evlist__parse_sample_timestamp()
 * |  Extract time stamp from sample data.
 * |
 * perf_session__queue_event()
 * |  If timestamp is positive the sample is entered into an ordered_event
 * |  list, sort order is the timestamp. The event processing is deferred until
 * |  later (see perf_session__process_user_event()).
 * |  Other timestamps (0 or -1) are handled immediately by
 * |  perf_session__deliver_event(). These are events generated at start up
 * |  of command perf record. They create PERF_RECORD_COMM and PERF_RECORD_MMAP*
 * |  records. They are needed to create a list of running processes and its
 * |  memory mappings and layout. They are needed at the beginning to enable
 * |  command perf report to create process trees and memory mappings.
 * |
 * perf_session__deliver_event()
 * |  Delivers a PERF_RECORD_XXX entry for handling.
 * |
 * auxtrace__process_event()
 * |  The timestamp of the PERF_RECORD_XXX entry is taken to correlate with
 * |  time stamps from the auxiliary trace buffers. This enables
 * |  synchronization between auxiliary trace data and the events on the
 * |  perf.data file.
 * |
 * machine__deliver_event()
 * |  Handles the PERF_RECORD_XXX event. This depends on the record type.
 *    It might update the process tree, update a process memory map or enter
 *    a sample with IP and call back chain data into GUI data pool.
 *
 *
 * Deferred processing determined by perf_session__process_user_event() is
 * finally processed when a PERF_RECORD_FINISHED_ROUND is encountered. These
 * are generated during command perf record.
 * The timestamp of PERF_RECORD_FINISHED_ROUND event is taken to process all
 * PERF_RECORD_XXX entries stored in the ordered_event list. This list was
 * built up while reading the perf.data file.
 * Each event is now processed by calling perf_session__deliver_event().
 * This enables time synchronization between the data in the perf.data file and
 * the data in the auxiliary trace buffers.
 */

#include <endian.h>
#include <errno.h>
#include <byteswap.h>
#include <inttypes.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/log2.h>
#include <linux/zalloc.h>

#include <sys/stat.h>
#include <sys/types.h>

#include "color.h"
#include "evsel.h"
#include "evlist.h"
#include "machine.h"
#include "session.h"
#include "tool.h"
#include "debug.h"
#include "auxtrace.h"
#include "s390-cpumsf.h"
#include "s390-cpumsf-kernel.h"
#include "s390-cpumcf-kernel.h"
#include "config.h"
#include "util/sample.h"

struct s390_cpumsf {
	struct auxtrace		auxtrace;
	struct auxtrace_queues	queues;
	struct auxtrace_heap	heap;
	struct perf_session	*session;
	struct machine		*machine;
	u32			auxtrace_type;
	u32			pmu_type;
	u16			machine_type;
	bool			data_queued;
	bool			use_logfile;
	char			*logdir;
};

struct s390_cpumsf_queue {
	struct s390_cpumsf	*sf;
	unsigned int		queue_nr;
	struct auxtrace_buffer	*buffer;
	int			cpu;
	FILE			*logfile;
	FILE			*logfile_ctr;
};

/* Check if the raw data should be dumped to file. If this is the case and
 * the file to dump to has not been opened for writing, do so.
 *
 * Return 0 on success and greater zero on error so processing continues.
 */
static int s390_cpumcf_dumpctr(struct s390_cpumsf *sf,
			       struct perf_sample *sample)
{
	struct s390_cpumsf_queue *sfq;
	struct auxtrace_queue *q;
	int rc = 0;

	if (!sf->use_logfile || sf->queues.nr_queues <= sample->cpu)
		return rc;

	q = &sf->queues.queue_array[sample->cpu];
	sfq = q->priv;
	if (!sfq)		/* Queue not yet allocated */
		return rc;

	if (!sfq->logfile_ctr) {
		char *name;

		rc = (sf->logdir)
			? asprintf(&name, "%s/aux.ctr.%02x",
				 sf->logdir, sample->cpu)
			: asprintf(&name, "aux.ctr.%02x", sample->cpu);
		if (rc > 0)
			sfq->logfile_ctr = fopen(name, "w");
		if (sfq->logfile_ctr == NULL) {
			pr_err("Failed to open counter set log file %s, "
			       "continue...\n", name);
			rc = 1;
		}
		free(name);
	}

	if (sfq->logfile_ctr) {
		/* See comment above for -4 */
		size_t n = fwrite(sample->raw_data, sample->raw_size - 4, 1,
				  sfq->logfile_ctr);
		if (n != 1) {
			pr_err("Failed to write counter set data\n");
			rc = 1;
		}
	}
	return rc;
}

/* Display s390 CPU measurement facility basic-sampling data entry
 * Data written on s390 in big endian byte order and contains bit
 * fields across byte boundaries.
 */
static bool s390_cpumsf_basic_show(const char *color, size_t pos,
				   struct hws_basic_entry *basicp)
{
	struct hws_basic_entry *basic = basicp;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	struct hws_basic_entry local;
	unsigned long long word = be64toh(*(unsigned long long *)basicp);

	memset(&local, 0, sizeof(local));
	local.def = be16toh(basicp->def);
	local.prim_asn = word & 0xffff;
	local.CL = word >> 30 & 0x3;
	local.I = word >> 32 & 0x1;
	local.AS = word >> 33 & 0x3;
	local.P = word >> 35 & 0x1;
	local.W = word >> 36 & 0x1;
	local.T = word >> 37 & 0x1;
	local.U = word >> 40 & 0xf;
	local.ia = be64toh(basicp->ia);
	local.gpp = be64toh(basicp->gpp);
	local.hpp = be64toh(basicp->hpp);
	basic = &local;
#endif
	if (basic->def != 1) {
		pr_err("Invalid AUX trace basic entry [%#08zx]\n", pos);
		return false;
	}
	color_fprintf(stdout, color, "    [%#08zx] Basic   Def:%04x Inst:%#04x"
		      " %c%c%c%c AS:%d ASN:%#04x IA:%#018llx\n"
		      "\t\tCL:%d HPP:%#018llx GPP:%#018llx\n",
		      pos, basic->def, basic->U,
		      basic->T ? 'T' : ' ',
		      basic->W ? 'W' : ' ',
		      basic->P ? 'P' : ' ',
		      basic->I ? 'I' : ' ',
		      basic->AS, basic->prim_asn, basic->ia, basic->CL,
		      basic->hpp, basic->gpp);
	return true;
}

/* Display s390 CPU measurement facility diagnostic-sampling data entry.
 * Data written on s390 in big endian byte order and contains bit
 * fields across byte boundaries.
 */
static bool s390_cpumsf_diag_show(const char *color, size_t pos,
				  struct hws_diag_entry *diagp)
{
	struct hws_diag_entry *diag = diagp;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	struct hws_diag_entry local;
	unsigned long long word = be64toh(*(unsigned long long *)diagp);

	local.def = be16toh(diagp->def);
	local.I = word >> 32 & 0x1;
	diag = &local;
#endif
	if (diag->def < S390_CPUMSF_DIAG_DEF_FIRST) {
		pr_err("Invalid AUX trace diagnostic entry [%#08zx]\n", pos);
		return false;
	}
	color_fprintf(stdout, color, "    [%#08zx] Diag    Def:%04x %c\n",
		      pos, diag->def, diag->I ? 'I' : ' ');
	return true;
}

/* Return TOD timestamp contained in an trailer entry */
static unsigned long long trailer_timestamp(struct hws_trailer_entry *te,
					    int idx)
{
	/* te->t set: TOD in STCKE format, bytes 8-15
	 * to->t not set: TOD in STCK format, bytes 0-7
	 */
	unsigned long long ts;

	memcpy(&ts, &te->timestamp[idx], sizeof(ts));
	return be64toh(ts);
}

/* Display s390 CPU measurement facility trailer entry */
static bool s390_cpumsf_trailer_show(const char *color, size_t pos,
				     struct hws_trailer_entry *te)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	struct hws_trailer_entry local;
	const unsigned long long flags = be64toh(te->flags);

	memset(&local, 0, sizeof(local));
	local.f = flags >> 63 & 0x1;
	local.a = flags >> 62 & 0x1;
	local.t = flags >> 61 & 0x1;
	local.bsdes = be16toh((flags >> 16 & 0xffff));
	local.dsdes = be16toh((flags & 0xffff));
	memcpy(&local.timestamp, te->timestamp, sizeof(te->timestamp));
	local.overflow = be64toh(te->overflow);
	local.clock_base = be64toh(te->progusage[0]) >> 63 & 1;
	local.progusage2 = be64toh(te->progusage2);
	te = &local;
#endif
	if (te->bsdes != sizeof(struct hws_basic_entry)) {
		pr_err("Invalid AUX trace trailer entry [%#08zx]\n", pos);
		return false;
	}
	color_fprintf(stdout, color, "    [%#08zx] Trailer %c%c%c bsdes:%d"
		      " dsdes:%d Overflow:%lld Time:%#llx\n"
		      "\t\tC:%d TOD:%#llx\n",
		      pos,
		      te->f ? 'F' : ' ',
		      te->a ? 'A' : ' ',
		      te->t ? 'T' : ' ',
		      te->bsdes, te->dsdes, te->overflow,
		      trailer_timestamp(te, te->clock_base),
		      te->clock_base, te->progusage2);
	return true;
}

/* Test a sample data block. It must be 4KB or a multiple thereof in size and
 * 4KB page aligned. Each sample data page has a trailer entry at the
 * end which contains the sample entry data sizes.
 *
 * Return true if the sample data block passes the checks and set the
 * basic set entry size and diagnostic set entry size.
 *
 * Return false on failure.
 *
 * Note: Old hardware does not set the basic or diagnostic entry sizes
 * in the trailer entry. Use the type number instead.
 */
static bool s390_cpumsf_validate(int machine_type,
				 unsigned char *buf, size_t len,
				 unsigned short *bsdes,
				 unsigned short *dsdes)
{
	struct hws_basic_entry *basic = (struct hws_basic_entry *)buf;
	struct hws_trailer_entry *te;

	*dsdes = *bsdes = 0;
	if (len & (S390_CPUMSF_PAGESZ - 1))	/* Illegal size */
		return false;
	if (be16toh(basic->def) != 1)	/* No basic set entry, must be first */
		return false;
	/* Check for trailer entry at end of SDB */
	te = (struct hws_trailer_entry *)(buf + S390_CPUMSF_PAGESZ
					      - sizeof(*te));
	*bsdes = be16toh(te->bsdes);
	*dsdes = be16toh(te->dsdes);
	if (!te->bsdes && !te->dsdes) {
		/* Very old hardware, use CPUID */
		switch (machine_type) {
		case 2097:
		case 2098:
			*dsdes = 64;
			*bsdes = 32;
			break;
		case 2817:
		case 2818:
			*dsdes = 74;
			*bsdes = 32;
			break;
		case 2827:
		case 2828:
			*dsdes = 85;
			*bsdes = 32;
			break;
		case 2964:
		case 2965:
			*dsdes = 112;
			*bsdes = 32;
			break;
		default:
			/* Illegal trailer entry */
			return false;
		}
	}
	return true;
}

/* Return true if there is room for another entry */
static bool s390_cpumsf_reached_trailer(size_t entry_sz, size_t pos)
{
	size_t payload = S390_CPUMSF_PAGESZ - sizeof(struct hws_trailer_entry);

	if (payload - (pos & (S390_CPUMSF_PAGESZ - 1)) < entry_sz)
		return false;
	return true;
}

/* Dump an auxiliary buffer. These buffers are multiple of
 * 4KB SDB pages.
 */
static void s390_cpumsf_dump(struct s390_cpumsf *sf,
			     unsigned char *buf, size_t len)
{
	const char *color = PERF_COLOR_BLUE;
	struct hws_basic_entry *basic;
	struct hws_diag_entry *diag;
	unsigned short bsdes, dsdes;
	size_t pos = 0;

	color_fprintf(stdout, color,
		      ". ... s390 AUX data: size %zu bytes\n",
		      len);

	if (!s390_cpumsf_validate(sf->machine_type, buf, len, &bsdes,
				  &dsdes)) {
		pr_err("Invalid AUX trace data block size:%zu"
		       " (type:%d bsdes:%hd dsdes:%hd)\n",
		       len, sf->machine_type, bsdes, dsdes);
		return;
	}

	/* s390 kernel always returns 4KB blocks fully occupied,
	 * no partially filled SDBs.
	 */
	while (pos < len) {
		/* Handle Basic entry */
		basic = (struct hws_basic_entry *)(buf + pos);
		if (s390_cpumsf_basic_show(color, pos, basic))
			pos += bsdes;
		else
			return;

		/* Handle Diagnostic entry */
		diag = (struct hws_diag_entry *)(buf + pos);
		if (s390_cpumsf_diag_show(color, pos, diag))
			pos += dsdes;
		else
			return;

		/* Check for trailer entry */
		if (!s390_cpumsf_reached_trailer(bsdes + dsdes, pos)) {
			/* Show trailer entry */
			struct hws_trailer_entry te;

			pos = (pos + S390_CPUMSF_PAGESZ)
			       & ~(S390_CPUMSF_PAGESZ - 1);
			pos -= sizeof(te);
			memcpy(&te, buf + pos, sizeof(te));
			/* Set descriptor sizes in case of old hardware
			 * where these values are not set.
			 */
			te.bsdes = bsdes;
			te.dsdes = dsdes;
			if (s390_cpumsf_trailer_show(color, pos, &te))
				pos += sizeof(te);
			else
				return;
		}
	}
}

static void s390_cpumsf_dump_event(struct s390_cpumsf *sf, unsigned char *buf,
				   size_t len)
{
	printf(".\n");
	s390_cpumsf_dump(sf, buf, len);
}

#define	S390_LPP_PID_MASK	0xffffffff

static bool s390_cpumsf_make_event(size_t pos,
				   struct hws_basic_entry *basic,
				   struct s390_cpumsf_queue *sfq)
{
	struct perf_sample sample = {
				.ip = basic->ia,
				.pid = basic->hpp & S390_LPP_PID_MASK,
				.tid = basic->hpp & S390_LPP_PID_MASK,
				.cpumode = PERF_RECORD_MISC_CPUMODE_UNKNOWN,
				.cpu = sfq->cpu,
				.period = 1
			    };
	union perf_event event;
	int ret;

	memset(&event, 0, sizeof(event));
	if (basic->CL == 1)	/* Native LPAR mode */
		sample.cpumode = basic->P ? PERF_RECORD_MISC_USER
					  : PERF_RECORD_MISC_KERNEL;
	else if (basic->CL == 2)	/* Guest kernel/user space */
		sample.cpumode = basic->P ? PERF_RECORD_MISC_GUEST_USER
					  : PERF_RECORD_MISC_GUEST_KERNEL;
	else if (basic->gpp || basic->prim_asn != 0xffff)
		/* Use heuristics on old hardware */
		sample.cpumode = basic->P ? PERF_RECORD_MISC_GUEST_USER
					  : PERF_RECORD_MISC_GUEST_KERNEL;
	else
		sample.cpumode = basic->P ? PERF_RECORD_MISC_USER
					  : PERF_RECORD_MISC_KERNEL;

	event.sample.header.type = PERF_RECORD_SAMPLE;
	event.sample.header.misc = sample.cpumode;
	event.sample.header.size = sizeof(struct perf_event_header);

	pr_debug4("%s pos:%#zx ip:%#" PRIx64 " P:%d CL:%d pid:%d.%d cpumode:%d cpu:%d\n",
		 __func__, pos, sample.ip, basic->P, basic->CL, sample.pid,
		 sample.tid, sample.cpumode, sample.cpu);
	ret = perf_session__deliver_synth_event(sfq->sf->session, &event, &sample);
	perf_sample__exit(&sample);
	if (ret) {
		pr_err("s390 Auxiliary Trace: failed to deliver event\n");
		return false;
	}
	return true;
}

static unsigned long long get_trailer_time(const unsigned char *buf)
{
	struct hws_trailer_entry *te;
	unsigned long long aux_time, progusage2;
	bool clock_base;

	te = (struct hws_trailer_entry *)(buf + S390_CPUMSF_PAGESZ
					      - sizeof(*te));

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	clock_base = be64toh(te->progusage[0]) >> 63 & 0x1;
	progusage2 = be64toh(te->progusage[1]);
#else
	clock_base = te->clock_base;
	progusage2 = te->progusage2;
#endif
	if (!clock_base)	/* TOD_CLOCK_BASE value missing */
		return 0;

	/* Correct calculation to convert time stamp in trailer entry to
	 * nano seconds (taken from arch/s390 function tod_to_ns()).
	 * TOD_CLOCK_BASE is stored in trailer entry member progusage2.
	 */
	aux_time = trailer_timestamp(te, clock_base) - progusage2;
	aux_time = (aux_time >> 9) * 125 + (((aux_time & 0x1ff) * 125) >> 9);
	return aux_time;
}

/* Process the data samples of a single queue. The first parameter is a
 * pointer to the queue, the second parameter is the time stamp. This
 * is the time stamp:
 * - of the event that triggered this processing.
 * - or the time stamp when the last processing of this queue stopped.
 *   In this case it stopped at a 4KB page boundary and record the
 *   position on where to continue processing on the next invocation
 *   (see buffer->use_data and buffer->use_size).
 *
 * When this function returns the second parameter is updated to
 * reflect the time stamp of the last processed auxiliary data entry
 * (taken from the trailer entry of that page). The caller uses this
 * returned time stamp to record the last processed entry in this
 * queue.
 *
 * The function returns:
 * 0:  Processing successful. The second parameter returns the
 *     time stamp from the trailer entry until which position
 *     processing took place. Subsequent calls resume from this
 *     position.
 * <0: An error occurred during processing. The second parameter
 *     returns the maximum time stamp.
 * >0: Done on this queue. The second parameter returns the
 *     maximum time stamp.
 */
static int s390_cpumsf_samples(struct s390_cpumsf_queue *sfq, u64 *ts)
{
	struct s390_cpumsf *sf = sfq->sf;
	unsigned char *buf = sfq->buffer->use_data;
	size_t len = sfq->buffer->use_size;
	struct hws_basic_entry *basic;
	unsigned short bsdes, dsdes;
	size_t pos = 0;
	int err = 1;
	u64 aux_ts;

	if (!s390_cpumsf_validate(sf->machine_type, buf, len, &bsdes,
				  &dsdes)) {
		*ts = ~0ULL;
		return -1;
	}

	/* Get trailer entry time stamp and check if entries in
	 * this auxiliary page are ready for processing. If the
	 * time stamp of the first entry is too high, whole buffer
	 * can be skipped. In this case return time stamp.
	 */
	aux_ts = get_trailer_time(buf);
	if (!aux_ts) {
		pr_err("[%#08" PRIx64 "] Invalid AUX trailer entry TOD clock base\n",
		       (s64)sfq->buffer->data_offset);
		aux_ts = ~0ULL;
		goto out;
	}
	if (aux_ts > *ts) {
		*ts = aux_ts;
		return 0;
	}

	while (pos < len) {
		/* Handle Basic entry */
		basic = (struct hws_basic_entry *)(buf + pos);
		if (s390_cpumsf_make_event(pos, basic, sfq))
			pos += bsdes;
		else {
			err = -EBADF;
			goto out;
		}

		pos += dsdes;	/* Skip diagnostic entry */

		/* Check for trailer entry */
		if (!s390_cpumsf_reached_trailer(bsdes + dsdes, pos)) {
			pos = (pos + S390_CPUMSF_PAGESZ)
			       & ~(S390_CPUMSF_PAGESZ - 1);
			/* Check existence of next page */
			if (pos >= len)
				break;
			aux_ts = get_trailer_time(buf + pos);
			if (!aux_ts) {
				aux_ts = ~0ULL;
				goto out;
			}
			if (aux_ts > *ts) {
				*ts = aux_ts;
				sfq->buffer->use_data += pos;
				sfq->buffer->use_size -= pos;
				return 0;
			}
		}
	}
out:
	*ts = aux_ts;
	sfq->buffer->use_size = 0;
	sfq->buffer->use_data = NULL;
	return err;	/* Buffer completely scanned or error */
}

/* Run the s390 auxiliary trace decoder.
 * Select the queue buffer to operate on, the caller already selected
 * the proper queue, depending on second parameter 'ts'.
 * This is the time stamp until which the auxiliary entries should
 * be processed. This value is updated by called functions and
 * returned to the caller.
 *
 * Resume processing in the current buffer. If there is no buffer
 * get a new buffer from the queue and setup start position for
 * processing.
 * When a buffer is completely processed remove it from the queue
 * before returning.
 *
 * This function returns
 * 1: When the queue is empty. Second parameter will be set to
 *    maximum time stamp.
 * 0: Normal processing done.
 * <0: Error during queue buffer setup. This causes the caller
 *     to stop processing completely.
 */
static int s390_cpumsf_run_decoder(struct s390_cpumsf_queue *sfq,
				   u64 *ts)
{

	struct auxtrace_buffer *buffer;
	struct auxtrace_queue *queue;
	int err;

	queue = &sfq->sf->queues.queue_array[sfq->queue_nr];

	/* Get buffer and last position in buffer to resume
	 * decoding the auxiliary entries. One buffer might be large
	 * and decoding might stop in between. This depends on the time
	 * stamp of the trailer entry in each page of the auxiliary
	 * data and the time stamp of the event triggering the decoding.
	 */
	if (sfq->buffer == NULL) {
		sfq->buffer = buffer = auxtrace_buffer__next(queue,
							     sfq->buffer);
		if (!buffer) {
			*ts = ~0ULL;
			return 1;	/* Processing done on this queue */
		}
		/* Start with a new buffer on this queue */
		if (buffer->data) {
			buffer->use_size = buffer->size;
			buffer->use_data = buffer->data;
		}
		if (sfq->logfile) {	/* Write into log file */
			size_t rc = fwrite(buffer->data, buffer->size, 1,
					   sfq->logfile);
			if (rc != 1)
				pr_err("Failed to write auxiliary data\n");
		}
	} else
		buffer = sfq->buffer;

	if (!buffer->data) {
		int fd = perf_data__fd(sfq->sf->session->data);

		buffer->data = auxtrace_buffer__get_data(buffer, fd);
		if (!buffer->data)
			return -ENOMEM;
		buffer->use_size = buffer->size;
		buffer->use_data = buffer->data;

		if (sfq->logfile) {	/* Write into log file */
			size_t rc = fwrite(buffer->data, buffer->size, 1,
					   sfq->logfile);
			if (rc != 1)
				pr_err("Failed to write auxiliary data\n");
		}
	}
	pr_debug4("%s queue_nr:%d buffer:%" PRId64 " offset:%#" PRIx64 " size:%#zx rest:%#zx\n",
		  __func__, sfq->queue_nr, buffer->buffer_nr, buffer->offset,
		  buffer->size, buffer->use_size);
	err = s390_cpumsf_samples(sfq, ts);

	/* If non-zero, there is either an error (err < 0) or the buffer is
	 * completely done (err > 0). The error is unrecoverable, usually
	 * some descriptors could not be read successfully, so continue with
	 * the next buffer.
	 * In both cases the parameter 'ts' has been updated.
	 */
	if (err) {
		sfq->buffer = NULL;
		list_del_init(&buffer->list);
		auxtrace_buffer__free(buffer);
		if (err > 0)		/* Buffer done, no error */
			err = 0;
	}
	return err;
}

static struct s390_cpumsf_queue *
s390_cpumsf_alloc_queue(struct s390_cpumsf *sf, unsigned int queue_nr)
{
	struct s390_cpumsf_queue *sfq;

	sfq = zalloc(sizeof(struct s390_cpumsf_queue));
	if (sfq == NULL)
		return NULL;

	sfq->sf = sf;
	sfq->queue_nr = queue_nr;
	sfq->cpu = -1;
	if (sf->use_logfile) {
		char *name;
		int rc;

		rc = (sf->logdir)
			? asprintf(&name, "%s/aux.smp.%02x",
				 sf->logdir, queue_nr)
			: asprintf(&name, "aux.smp.%02x", queue_nr);
		if (rc > 0)
			sfq->logfile = fopen(name, "w");
		if (sfq->logfile == NULL) {
			pr_err("Failed to open auxiliary log file %s,"
			       "continue...\n", name);
			sf->use_logfile = false;
		}
		free(name);
	}
	return sfq;
}

static int s390_cpumsf_setup_queue(struct s390_cpumsf *sf,
				   struct auxtrace_queue *queue,
				   unsigned int queue_nr, u64 ts)
{
	struct s390_cpumsf_queue *sfq = queue->priv;

	if (list_empty(&queue->head))
		return 0;

	if (sfq == NULL) {
		sfq = s390_cpumsf_alloc_queue(sf, queue_nr);
		if (!sfq)
			return -ENOMEM;
		queue->priv = sfq;

		if (queue->cpu != -1)
			sfq->cpu = queue->cpu;
	}
	return auxtrace_heap__add(&sf->heap, queue_nr, ts);
}

static int s390_cpumsf_setup_queues(struct s390_cpumsf *sf, u64 ts)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < sf->queues.nr_queues; i++) {
		ret = s390_cpumsf_setup_queue(sf, &sf->queues.queue_array[i],
					      i, ts);
		if (ret)
			break;
	}
	return ret;
}

static int s390_cpumsf_update_queues(struct s390_cpumsf *sf, u64 ts)
{
	if (!sf->queues.new_data)
		return 0;

	sf->queues.new_data = false;
	return s390_cpumsf_setup_queues(sf, ts);
}

static int s390_cpumsf_process_queues(struct s390_cpumsf *sf, u64 timestamp)
{
	unsigned int queue_nr;
	u64 ts;
	int ret;

	while (1) {
		struct auxtrace_queue *queue;
		struct s390_cpumsf_queue *sfq;

		if (!sf->heap.heap_cnt)
			return 0;

		if (sf->heap.heap_array[0].ordinal >= timestamp)
			return 0;

		queue_nr = sf->heap.heap_array[0].queue_nr;
		queue = &sf->queues.queue_array[queue_nr];
		sfq = queue->priv;

		auxtrace_heap__pop(&sf->heap);
		if (sf->heap.heap_cnt) {
			ts = sf->heap.heap_array[0].ordinal + 1;
			if (ts > timestamp)
				ts = timestamp;
		} else {
			ts = timestamp;
		}

		ret = s390_cpumsf_run_decoder(sfq, &ts);
		if (ret < 0) {
			auxtrace_heap__add(&sf->heap, queue_nr, ts);
			return ret;
		}
		if (!ret) {
			ret = auxtrace_heap__add(&sf->heap, queue_nr, ts);
			if (ret < 0)
				return ret;
		}
	}
	return 0;
}

static int s390_cpumsf_synth_error(struct s390_cpumsf *sf, int code, int cpu,
				   pid_t pid, pid_t tid, u64 ip, u64 timestamp)
{
	char msg[MAX_AUXTRACE_ERROR_MSG];
	union perf_event event;
	int err;

	strncpy(msg, "Lost Auxiliary Trace Buffer", sizeof(msg) - 1);
	auxtrace_synth_error(&event.auxtrace_error, PERF_AUXTRACE_ERROR_ITRACE,
			     code, cpu, pid, tid, ip, msg, timestamp);

	err = perf_session__deliver_synth_event(sf->session, &event, NULL);
	if (err)
		pr_err("s390 Auxiliary Trace: failed to deliver error event,"
			"error %d\n", err);
	return err;
}

static int s390_cpumsf_lost(struct s390_cpumsf *sf, struct perf_sample *sample)
{
	return s390_cpumsf_synth_error(sf, 1, sample->cpu,
				       sample->pid, sample->tid, 0,
				       sample->time);
}

static int
s390_cpumsf_process_event(struct perf_session *session,
			  union perf_event *event,
			  struct perf_sample *sample,
			  const struct perf_tool *tool)
{
	struct s390_cpumsf *sf = container_of(session->auxtrace,
					      struct s390_cpumsf,
					      auxtrace);
	u64 timestamp = sample->time;
	struct evsel *ev_bc000;

	int err = 0;

	if (dump_trace)
		return 0;

	if (!tool->ordered_events) {
		pr_err("s390 Auxiliary Trace requires ordered events\n");
		return -EINVAL;
	}

	if (event->header.type == PERF_RECORD_SAMPLE &&
	    sample->raw_size) {
		/* Handle event with raw data */
		ev_bc000 = evlist__event2evsel(session->evlist, event);
		if (ev_bc000 &&
		    ev_bc000->core.attr.config == PERF_EVENT_CPUM_CF_DIAG)
			err = s390_cpumcf_dumpctr(sf, sample);
		return err;
	}

	if (event->header.type == PERF_RECORD_AUX &&
	    event->aux.flags & PERF_AUX_FLAG_TRUNCATED)
		return s390_cpumsf_lost(sf, sample);

	if (timestamp) {
		err = s390_cpumsf_update_queues(sf, timestamp);
		if (!err)
			err = s390_cpumsf_process_queues(sf, timestamp);
	}
	return err;
}

static int
s390_cpumsf_process_auxtrace_event(struct perf_session *session,
				   union perf_event *event __maybe_unused,
				   const struct perf_tool *tool __maybe_unused)
{
	struct s390_cpumsf *sf = container_of(session->auxtrace,
					      struct s390_cpumsf,
					      auxtrace);

	int fd = perf_data__fd(session->data);
	struct auxtrace_buffer *buffer;
	off_t data_offset;
	int err;

	if (sf->data_queued)
		return 0;

	if (perf_data__is_pipe(session->data)) {
		data_offset = 0;
	} else {
		data_offset = lseek(fd, 0, SEEK_CUR);
		if (data_offset == -1)
			return -errno;
	}

	err = auxtrace_queues__add_event(&sf->queues, session, event,
					 data_offset, &buffer);
	if (err)
		return err;

	/* Dump here after copying piped trace out of the pipe */
	if (dump_trace) {
		if (auxtrace_buffer__get_data(buffer, fd)) {
			s390_cpumsf_dump_event(sf, buffer->data,
					       buffer->size);
			auxtrace_buffer__put_data(buffer);
		}
	}
	return 0;
}

static void s390_cpumsf_free_events(struct perf_session *session __maybe_unused)
{
}

static int s390_cpumsf_flush(struct perf_session *session __maybe_unused,
			     const struct perf_tool *tool __maybe_unused)
{
	return 0;
}

static void s390_cpumsf_free_queues(struct perf_session *session)
{
	struct s390_cpumsf *sf = container_of(session->auxtrace,
					      struct s390_cpumsf,
					      auxtrace);
	struct auxtrace_queues *queues = &sf->queues;
	unsigned int i;

	for (i = 0; i < queues->nr_queues; i++) {
		struct s390_cpumsf_queue *sfq = (struct s390_cpumsf_queue *)
						queues->queue_array[i].priv;

		if (sfq != NULL) {
			if (sfq->logfile) {
				fclose(sfq->logfile);
				sfq->logfile = NULL;
			}
			if (sfq->logfile_ctr) {
				fclose(sfq->logfile_ctr);
				sfq->logfile_ctr = NULL;
			}
		}
		zfree(&queues->queue_array[i].priv);
	}
	auxtrace_queues__free(queues);
}

static void s390_cpumsf_free(struct perf_session *session)
{
	struct s390_cpumsf *sf = container_of(session->auxtrace,
					      struct s390_cpumsf,
					      auxtrace);

	auxtrace_heap__free(&sf->heap);
	s390_cpumsf_free_queues(session);
	session->auxtrace = NULL;
	zfree(&sf->logdir);
	free(sf);
}

static bool
s390_cpumsf_evsel_is_auxtrace(struct perf_session *session __maybe_unused,
			      struct evsel *evsel)
{
	return evsel->core.attr.type == PERF_TYPE_RAW &&
	       evsel->core.attr.config == PERF_EVENT_CPUM_SF_DIAG;
}

static int s390_cpumsf_get_type(const char *cpuid)
{
	int ret, family = 0;

	ret = sscanf(cpuid, "%*[^,],%u", &family);
	return (ret == 1) ? family : 0;
}

/* Check itrace options set on perf report command.
 * Return true, if none are set or all options specified can be
 * handled on s390 (currently only option 'd' for logging.
 * Return false otherwise.
 */
static bool check_auxtrace_itrace(struct itrace_synth_opts *itops)
{
	bool ison = false;

	if (!itops || !itops->set)
		return true;
	ison = itops->inject || itops->instructions || itops->branches ||
		itops->transactions || itops->ptwrites ||
		itops->pwr_events || itops->errors ||
		itops->dont_decode || itops->calls || itops->returns ||
		itops->callchain || itops->thread_stack ||
		itops->last_branch || itops->add_callchain ||
		itops->add_last_branch;
	if (!ison)
		return true;
	pr_err("Unsupported --itrace options specified\n");
	return false;
}

/* Check for AUXTRACE dump directory if it is needed.
 * On failure print an error message but continue.
 * Return 0 on wrong keyword in config file and 1 otherwise.
 */
static int s390_cpumsf__config(const char *var, const char *value, void *cb)
{
	struct s390_cpumsf *sf = cb;
	struct stat stbuf;
	int rc;

	if (strcmp(var, "auxtrace.dumpdir"))
		return 0;
	sf->logdir = strdup(value);
	if (sf->logdir == NULL) {
		pr_err("Failed to find auxtrace log directory %s,"
		       " continue with current directory...\n", value);
		return 1;
	}
	rc = stat(sf->logdir, &stbuf);
	if (rc == -1 || !S_ISDIR(stbuf.st_mode)) {
		pr_err("Missing auxtrace log directory %s,"
		       " continue with current directory...\n", value);
		zfree(&sf->logdir);
	}
	return 1;
}

int s390_cpumsf_process_auxtrace_info(union perf_event *event,
				      struct perf_session *session)
{
	struct perf_record_auxtrace_info *auxtrace_info = &event->auxtrace_info;
	struct s390_cpumsf *sf;
	int err;

	if (auxtrace_info->header.size < sizeof(struct perf_record_auxtrace_info))
		return -EINVAL;

	sf = zalloc(sizeof(struct s390_cpumsf));
	if (sf == NULL)
		return -ENOMEM;

	if (!check_auxtrace_itrace(session->itrace_synth_opts)) {
		err = -EINVAL;
		goto err_free;
	}
	sf->use_logfile = session->itrace_synth_opts->log;
	if (sf->use_logfile)
		perf_config(s390_cpumsf__config, sf);

	err = auxtrace_queues__init(&sf->queues);
	if (err)
		goto err_free;

	sf->session = session;
	sf->machine = &session->machines.host; /* No kvm support */
	sf->auxtrace_type = auxtrace_info->type;
	sf->pmu_type = PERF_TYPE_RAW;
	sf->machine_type = s390_cpumsf_get_type(perf_session__env(session)->cpuid);

	sf->auxtrace.process_event = s390_cpumsf_process_event;
	sf->auxtrace.process_auxtrace_event = s390_cpumsf_process_auxtrace_event;
	sf->auxtrace.flush_events = s390_cpumsf_flush;
	sf->auxtrace.free_events = s390_cpumsf_free_events;
	sf->auxtrace.free = s390_cpumsf_free;
	sf->auxtrace.evsel_is_auxtrace = s390_cpumsf_evsel_is_auxtrace;
	session->auxtrace = &sf->auxtrace;

	if (dump_trace)
		return 0;

	err = auxtrace_queues__process_index(&sf->queues, session);
	if (err)
		goto err_free_queues;

	if (sf->queues.populated)
		sf->data_queued = true;

	return 0;

err_free_queues:
	auxtrace_queues__free(&sf->queues);
	session->auxtrace = NULL;
err_free:
	zfree(&sf->logdir);
	free(sf);
	return err;
}
