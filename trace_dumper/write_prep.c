/*
 * write_prep.c: Routines used to prepare I/O vectors for writing
 *
 *      File Created on: Feb 3, 2013 by Yitzik Casapu, Infinidat
 *      Original Author: Yotam Rubin, 2012
 *      Maintainer:      Yitzik Casapu, Infinidat
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
 */

#include <syslog.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include "../trace_lib.h"
#include "../trace_user.h"
#include "../trace_str_util.h"
#include "../trace_clock.h"
#include "trace_dumper.h"
#include "buffers.h"
#include "metadata.h"
#include "writer.h"
#include "write_prep.h"

static inline unsigned current_read_index(const struct trace_mapped_records *mapped_records)
{
    return mapped_records->current_read_record & mapped_records->imutab->max_records_mask;
}

void calculate_delta(
        const struct trace_mapped_records *mapped_records,
        struct records_pending_write *delta)
{

#ifndef _LP64
#warning "Use of this implementation is not recommended on platforms where sizeof(long) < 8, problems with wrap-around are likely."
#endif


    trace_record_counter_t last_written_record = mapped_records->mutab->last_committed_record;
    unsigned last_written_idx = last_written_record & mapped_records->imutab->max_records_mask;
    volatile const struct trace_record *last_record = &mapped_records->records[last_written_idx];

    memset(delta, 0, sizeof(*delta));
    if(TRACE_SEV_INVALID == last_record->severity) {
        if (-1UL != last_written_record) {  /* Some traces have been written */
            syslog(LOG_USER|LOG_ERR,
                    "Record %lu was uninitialized but marked as committed while dumping from a buffer with for pid %d",
                    last_written_record, last_record->pid);
        }
        delta->remaining_before_loss = mapped_records->imutab->max_records;
        return;
    }

    /* Verify the record counters haven't wrapped around. On 64-bit platforms this should never happen. */
    assert(last_written_record + 1UL >= mapped_records->current_read_record);
    long backlog_len = last_written_record + 1UL - mapped_records->current_read_record;

    /* Check whether the number of records written to the shared-memory buffers exceeds the number read by the dumper by more than the buffer size.
           * If so - we have lost records. */
    long overrun_records =
            (long)(backlog_len - mapped_records->imutab->max_records);

    delta->lost = MAX(overrun_records, 0L);
    delta->remaining_before_loss = MAX(-overrun_records, 0L);
    delta->total = MIN(backlog_len, TRACE_FILE_MAX_RECORDS_PER_CHUNK);
    delta->beyond_chunk_size = backlog_len - delta->total;

    unsigned current_read_idx = current_read_index(mapped_records);
    delta->up_to_buf_end  = MIN(delta->total, mapped_records->imutab->max_records - current_read_idx);
    delta->from_buf_start = delta->total - delta->up_to_buf_end;

    assert(delta->total <= TRACE_FILE_MAX_RECORDS_PER_CHUNK);
    assert(delta->from_buf_start + delta->up_to_buf_end == delta->total);
}

void init_dump_header(struct trace_dumper_configuration_s *conf, struct trace_record *dump_header_rec,
                             unsigned long long cur_ts,
                             struct iovec **iovec, unsigned int *num_iovecs, unsigned int *total_written_records)
{
    memset(dump_header_rec, 0, sizeof(*dump_header_rec));
    *iovec = &conf->flush_iovec[(*num_iovecs)++];
    (*iovec)->iov_base = dump_header_rec;
    (*iovec)->iov_len = sizeof(*dump_header_rec);

    (*total_written_records)++;
    dump_header_rec->rec_type = TRACE_REC_TYPE_DUMP_HEADER;
    dump_header_rec->termination = (TRACE_TERMINATION_LAST | TRACE_TERMINATION_FIRST);
    dump_header_rec->u.dump_header.prev_dump_offset = conf->last_flush_offset;
    dump_header_rec->ts = cur_ts;
    dump_header_rec->u.dump_header.records_previously_discarded = conf->record_file.records_discarded;
}

/* Initialize the buffer chunk header and set-up the iovec for the no wrap-around case. */
void init_buffer_chunk_record(struct trace_dumper_configuration_s *conf, const struct trace_mapped_buffer *mapped_buffer,
                                     struct trace_mapped_records *mapped_records, struct trace_record_buffer_dump **bd,
                                     struct iovec **iovec, unsigned int *iovcnt,
                                     const struct records_pending_write *deltas,
                                     unsigned long long cur_ts, unsigned int total_written_records)
{
    memset(&mapped_records->buffer_dump_record, 0, sizeof(mapped_records->buffer_dump_record));
    mapped_records->buffer_dump_record.rec_type = TRACE_REC_TYPE_BUFFER_CHUNK;
    mapped_records->buffer_dump_record.ts = cur_ts;
    mapped_records->buffer_dump_record.termination = (TRACE_TERMINATION_LAST |
                                                      TRACE_TERMINATION_FIRST);
    mapped_records->buffer_dump_record.pid = mapped_buffer->pid;

    /* Fill the buffer chunk header */
    (*bd) = &mapped_records->buffer_dump_record.u.buffer_chunk;
    (*bd)->last_metadata_offset = mapped_buffer->last_metadata_offset;
    (*bd)->prev_chunk_offset = mapped_records->last_flush_offset;
    (*bd)->dump_header_offset = conf->last_flush_offset;
    (*bd)->ts = cur_ts;
    (*bd)->lost_records = deltas->lost + mapped_records->num_records_discarded;
    (*bd)->records = deltas->total;
    (*bd)->severity_type = mapped_records->imutab->severity_type;

    mapped_records->next_flush_offset = conf->record_file.records_written + total_written_records;

    /* Place the buffer chunk header record in the iovec. */
    (*iovec) = &conf->flush_iovec[(*iovcnt)++];
    (*iovec)->iov_base = &mapped_records->buffer_dump_record;
    (*iovec)->iov_len = sizeof(mapped_records->buffer_dump_record);

    /* Add the records in the chunk to the iovec. */
    (*iovec) = &conf->flush_iovec[(*iovcnt)++];
    (*iovec)->iov_base = (void *)&mapped_records->records[current_read_index(mapped_records)];
    (*iovec)->iov_len = TRACE_RECORD_SIZE * deltas->up_to_buf_end;
}


static bool_t records_are_from_same_trace(volatile const struct trace_record *rec1, volatile const struct trace_record *rec2)
{
    return (rec1->ts == rec2->ts) && (rec1->tid == rec2->tid)  && (rec1->severity == rec2->severity);
}

static bool_t record_ends_trace(volatile const struct trace_record *ending_candidate, volatile const struct trace_record *start_rec) {
    assert(start_rec->termination & TRACE_TERMINATION_FIRST);
    return (ending_candidate->termination & TRACE_TERMINATION_LAST) || !records_are_from_same_trace(ending_candidate, start_rec);
}

static volatile const struct trace_record *n_records_after(volatile const struct trace_record *rec, const struct trace_mapped_records *mapped_records, ssize_t n)
{
    const size_t idx =  rec - mapped_records->records;
    assert(idx < mapped_records->imutab->max_records);
    return mapped_records->records + ((idx + n) & mapped_records->imutab->max_records_mask);
}

static volatile const struct trace_record *previous_record(volatile const struct trace_record *rec, const struct trace_mapped_records *mapped_records)
{
    return n_records_after(rec, mapped_records, -1);
}

unsigned add_warn_records_to_iov(
        const struct trace_mapped_records *mapped_records,
        unsigned count,
        enum trace_severity threshold_severity,
        struct trace_record_file *record_file)
{
    unsigned recs_covered = 0;
    unsigned start_idx = mapped_records->imutab->max_records_mask & mapped_records->current_read_record;
    unsigned i;
    const unsigned initial_count = record_file->iov_count;

    const useconds_t retry_wait_len = 10;
    const unsigned num_retries_on_partial_record = 3;
    unsigned retries_left = num_retries_on_partial_record;

    volatile const struct trace_record *const end_rec = n_records_after(mapped_records->records + start_idx, mapped_records, count);
    for (i = 0; i < count; i+= recs_covered) {
        volatile const struct trace_record *rec = n_records_after(mapped_records->records + start_idx, mapped_records, i);
        struct iovec *iov = increase_iov_if_necessary(record_file, record_file->iov_count + 2);

        if ((rec->termination & TRACE_TERMINATION_FIRST) &&
            (TRACE_REC_TYPE_TYPED == rec->rec_type) &&
            (rec->severity >= threshold_severity)) {
                unsigned iov_idx = record_file->iov_count;
                volatile const struct trace_record *const starting_rec = rec;
                do {
                    /* In case of wrap-around within the record sequence for a single trace, start a new iovec */
                    if (__builtin_expect(rec >= mapped_records->records + mapped_records->imutab->max_records, 0)) {
                        assert(rec == mapped_records->records + mapped_records->imutab->max_records);
                        recs_covered = mapped_records->imutab->max_records - (start_idx + i);
                        assert(recs_covered > 0);
                        DEBUG("Buffer wrap-around while scanning for notifications", recs_covered, iov_idx, i);
                        iov[iov_idx].iov_len = sizeof(*rec) * recs_covered;
                        i+= recs_covered;
                        iov_idx++;
                        rec = mapped_records->records;
                        iov[iov_idx].iov_base = (void *)rec;
                    }
                } while (! record_ends_trace(rec++, starting_rec) && (end_rec != rec));

                recs_covered = (rec - starting_rec) & mapped_records->imutab->max_records_mask;
                assert(recs_covered >= 1);
                assert(i + recs_covered <= count);

                if (! records_are_from_same_trace(previous_record(rec, mapped_records), starting_rec)) {
                    if (retries_left > 0) {
                       INFO("Unterminated record found while scanning for notifications, The scan will be retried", retries_left, iov_idx, i, recs_covered);
                       retries_left--;
                       recs_covered = 0;
                       usleep(retry_wait_len);
                    }
                    else {
                        WARN("Skipped a partial record while building the notification iov of severity", (enum trace_severity)(starting_rec->severity), start_idx, i, recs_covered, count);
                        syslog(LOG_USER|LOG_NOTICE, "Was about to add a partial record of severity %s to the notification iov, at start_idx=%u, i=%u, recs_covered=%u, count=%u",
                                trace_severity_to_str_array[starting_rec->severity], start_idx, i, recs_covered, count);
                    }
                    continue;
                }

                retries_left = num_retries_on_partial_record;
                iov[iov_idx].iov_base = (void *)starting_rec;
                iov[iov_idx].iov_len = sizeof(*rec) * recs_covered;
                record_file->iov_count = iov_idx + 1;
        }
        else {
            recs_covered = 1;
        }
    }

    return record_file->iov_count - initial_count;
}

trace_ts_t get_nsec_monotonic(void)
{
    const trace_ts_t now = trace_get_nsec_monotonic();
    if ((trace_ts_t) -1 == now) {
        syslog(LOG_ERR|LOG_USER, "Trace dumper has failed to read system time because of the following error: %s", strerror(errno));
    }

    return now;
}
