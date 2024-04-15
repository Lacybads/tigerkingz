/* Copyright 2015 greenbytes GmbH (https://www.greenbytes.de)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <apr_strings.h>
#include <ap_mpm.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_connection.h>
#include <http_request.h>

#include "h2_private.h"
#include "h2_bucket_eoc.h"
#include "h2_bucket_eos.h"
#include "h2_config.h"
#include "h2_conn_io.h"
#include "h2_h2.h"
#include "h2_session.h"
#include "h2_util.h"

#define TLS_DATA_MAX          (16*1024) 

/* Calculated like this: assuming MTU 1500 bytes
 * 1500 - 40 (IP) - 20 (TCP) - 40 (TCP options) 
 *      - TLS overhead (60-100) 
 * ~= 1300 bytes */
#define WRITE_SIZE_INITIAL    1300
/* Calculated like this: max TLS record size 16*1024
 *   - 40 (IP) - 20 (TCP) - 40 (TCP options) 
 *    - TLS overhead (60-100) 
 * which seems to create less TCP packets overall
 */
#define WRITE_SIZE_MAX        (TLS_DATA_MAX - 100) 


static void h2_conn_io_bb_log(conn_rec *c, int stream_id, int level, 
                              const char *tag, apr_bucket_brigade *bb)
{
    char buffer[16 * 1024];
    const char *line = "(null)";
    apr_size_t bmax = sizeof(buffer)/sizeof(buffer[0]);
    int off = 0;
    apr_bucket *b;
    
    if (bb) {
        memset(buffer, 0, bmax--);
        for (b = APR_BRIGADE_FIRST(bb); 
             bmax && (b != APR_BRIGADE_SENTINEL(bb));
             b = APR_BUCKET_NEXT(b)) {
            
            if (APR_BUCKET_IS_METADATA(b)) {
                if (APR_BUCKET_IS_EOS(b)) {
                    off += apr_snprintf(buffer+off, bmax-off, "eos ");
                }
                else if (APR_BUCKET_IS_FLUSH(b)) {
                    off += apr_snprintf(buffer+off, bmax-off, "flush ");
                }
                else if (AP_BUCKET_IS_EOR(b)) {
                    off += apr_snprintf(buffer+off, bmax-off, "eor ");
                }
                else if (H2_BUCKET_IS_H2EOC(b)) {
                    off += apr_snprintf(buffer+off, bmax-off, "h2eoc ");
                }
                else if (H2_BUCKET_IS_H2EOS(b)) {
                    off += apr_snprintf(buffer+off, bmax-off, "h2eos ");
                }
                else {
                    off += apr_snprintf(buffer+off, bmax-off, "meta(unknown) ");
                }
            }
            else {
                const char *btype = "data";
                if (APR_BUCKET_IS_FILE(b)) {
                    btype = "file";
                }
                else if (APR_BUCKET_IS_PIPE(b)) {
                    btype = "pipe";
                }
                else if (APR_BUCKET_IS_SOCKET(b)) {
                    btype = "socket";
                }
                else if (APR_BUCKET_IS_HEAP(b)) {
                    btype = "heap";
                }
                else if (APR_BUCKET_IS_TRANSIENT(b)) {
                    btype = "transient";
                }
                else if (APR_BUCKET_IS_IMMORTAL(b)) {
                    btype = "immortal";
                }
#if APR_HAS_MMAP
                else if (APR_BUCKET_IS_MMAP(b)) {
                    btype = "mmap";
                }
#endif
                else if (APR_BUCKET_IS_POOL(b)) {
                    btype = "pool";
                }
                
                off += apr_snprintf(buffer+off, bmax-off, "%s[%ld] ", 
                                    btype, 
                                    (long)(b->length == ((apr_size_t)-1)? 
                                           -1 : b->length));
            }
        }
        line = *buffer? buffer : "(empty)";
    }
    /* Intentional no APLOGNO */
    ap_log_cerror(APLOG_MARK, level, 0, c, "bb_dump(%s)-%s: %s", 
                  c->log_id, tag, line);

}

apr_status_t h2_conn_io_init(h2_conn_io *io, conn_rec *c, 
                             const h2_config *cfg)
{
    io->c              = c;
    io->output         = apr_brigade_create(c->pool, c->bucket_alloc);
    io->is_tls         = h2_h2_is_tls(c);
    io->buffer_output  = io->is_tls;
    io->pass_threshold = (apr_size_t)h2_config_geti64(cfg, H2_CONF_STREAM_MAX_MEM);
    io->flush_factor   = h2_config_geti(cfg, H2_CONF_TLS_FLUSH_COUNT);
    io->speed_factor   = 1.0;

    if (io->is_tls) {
        /* This is what we start with, 
         * see https://issues.apache.org/jira/browse/TS-2503 
         */
        io->warmup_size    = h2_config_geti64(cfg, H2_CONF_TLS_WARMUP_SIZE);
        io->cooldown_usecs = (h2_config_geti(cfg, H2_CONF_TLS_COOLDOWN_SECS) 
                              * APR_USEC_PER_SEC);
        io->write_size     = (io->cooldown_usecs > 0? 
                              WRITE_SIZE_INITIAL : WRITE_SIZE_MAX); 
    }
    else {
        io->warmup_size    = 0;
        io->cooldown_usecs = 0;
        io->write_size     = 0;
    }

    if (APLOGctrace1(c)) {
        ap_log_cerror(APLOG_MARK, APLOG_TRACE4, 0, io->c,
                      "h2_conn_io(%ld): init, buffering=%d, warmup_size=%ld, "
                      "cd_secs=%f", io->c->id, io->buffer_output, 
                      (long)io->warmup_size,
                      ((float)io->cooldown_usecs/APR_USEC_PER_SEC));
    }

    return APR_SUCCESS;
}

#define LOG_SCRATCH 0

static void append_scratch(h2_conn_io *io) 
{
    if (io->scratch && io->slen > 0) {
        apr_bucket *b = apr_bucket_heap_create(io->scratch, io->slen,
                                               apr_bucket_free,
                                               io->c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(io->output, b);
#if LOG_SCRATCH
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, io->c, APLOGNO(03386)
                      "h2_conn_io(%ld): append_scratch(%ld)", 
                      io->c->id, (long)io->slen);
#endif
        io->scratch = NULL;
        io->slen = io->ssize = 0;
    }
}

static apr_size_t assure_scratch_space(h2_conn_io *io) {
    apr_size_t remain = io->ssize - io->slen; 
    if (io->scratch && remain == 0) {
        append_scratch(io);
    }
    if (!io->scratch) {
        /* we control the size and it is larger than what buckets usually
         * allocate. */
        io->scratch = apr_bucket_alloc(io->write_size, io->c->bucket_alloc);
        io->ssize = io->write_size;
        io->slen = 0;
        remain = io->ssize;
    }
    return remain;
}
    
static apr_status_t read_to_scratch(h2_conn_io *io, apr_bucket *b)
{
    apr_status_t status;
    const char *data;
    apr_size_t len;
    
    if (!b->length) {
        return APR_SUCCESS;
    }
    
    ap_assert(b->length <= (io->ssize - io->slen));
    if (APR_BUCKET_IS_FILE(b)) {
        apr_bucket_file *f = (apr_bucket_file *)b->data;
        apr_file_t *fd = f->fd;
        apr_off_t offset = b->start;
        apr_size_t len = b->length;
        
        /* file buckets will either mmap (which we do not want) or
         * read 8000 byte chunks and split themself. However, we do
         * know *exactly* how many bytes we need where.
         */
        status = apr_file_seek(fd, APR_SET, &offset);
        if (status != APR_SUCCESS) {
            return status;
        }
        status = apr_file_read(fd, io->scratch + io->slen, &len);
#if LOG_SCRATCH
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, io->c, APLOGNO(03387)
                      "h2_conn_io(%ld): FILE_to_scratch(%ld)", 
                      io->c->id, (long)len); 
#endif
        if (status != APR_SUCCESS && status != APR_EOF) {
            return status;
        }
        io->slen += len;
    }
    else {
        status = apr_bucket_read(b, &data, &len, APR_BLOCK_READ);
        if (status == APR_SUCCESS) {
#if LOG_SCRATCH
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, io->c, APLOGNO(03388)
                          "h2_conn_io(%ld): read_to_scratch(%ld)", 
                          io->c->id, (long)b->length); 
#endif
            memcpy(io->scratch+io->slen, data, len);
            io->slen += len;
        }
    }
    return status;
}

static void check_write_size(h2_conn_io *io) 
{
    if (io->write_size > WRITE_SIZE_INITIAL 
        && (io->cooldown_usecs > 0)
        && (apr_time_now() - io->last_write) >= io->cooldown_usecs) {
        /* long time not written, reset write size */
        io->write_size = WRITE_SIZE_INITIAL;
        io->bytes_written = 0;
        io->speed_factor = 1.0;
        ap_log_cerror(APLOG_MARK, APLOG_TRACE4, 0, io->c,
                      "h2_conn_io(%ld): timeout write size reset to %ld", 
                      (long)io->c->id, (long)io->write_size);
    }
    else if (io->write_size < WRITE_SIZE_MAX 
             && io->bytes_written >= io->warmup_size) {
        /* connection is hot, use max size */
        io->write_size = WRITE_SIZE_MAX;
        ap_log_cerror(APLOG_MARK, APLOG_TRACE4, 0, io->c,
                      "h2_conn_io(%ld): threshold reached, write size now %ld", 
                      (long)io->c->id, (long)io->write_size);
    }
}

static apr_status_t pass_output(h2_conn_io *io, int flush,
                                h2_session *session_eoc)
{
    conn_rec *c = io->c;
    apr_bucket_brigade *bb = io->output;
    apr_bucket *b;
    apr_off_t bblen;
    apr_status_t status;
    
    append_scratch(io);
    if (flush) {
        b = apr_bucket_flush_create(c->bucket_alloc);
        APR_BRIGADE_INSERT_TAIL(bb, b);
    }
    
    if (APR_BRIGADE_EMPTY(bb)) {
        return APR_SUCCESS;
    }
    
    ap_log_cerror(APLOG_MARK, APLOG_TRACE4, 0, c, "h2_conn_io: pass_output");
    ap_update_child_status(c->sbh, SERVER_BUSY_WRITE, NULL);
    apr_brigade_length(bb, 0, &bblen);
    
    h2_conn_io_bb_log(c, 0, APLOG_TRACE2, "master conn pass", bb);
    status = ap_pass_brigade(c->output_filters, bb);
    if (status == APR_SUCCESS) {
        io->bytes_written += (apr_size_t)bblen;
        io->last_write = apr_time_now();
    }
    apr_brigade_cleanup(bb);

    if (session_eoc) {
        apr_status_t tmp;
        b = h2_bucket_eoc_create(c->bucket_alloc, session_eoc);
        APR_BRIGADE_INSERT_TAIL(bb, b);
        h2_conn_io_bb_log(c, 0, APLOG_TRACE2, "master conn pass", bb);
        tmp = ap_pass_brigade(c->output_filters, bb);
        if (status == APR_SUCCESS) {
            status = tmp;
        }
        /* careful with access to io after this, we have flushed an EOC bucket
         * that de-allocated us all. */
        apr_brigade_cleanup(bb);
    }
    
    if (status != APR_SUCCESS) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, status, c, APLOGNO(03044)
                      "h2_conn_io(%ld): pass_out brigade %ld bytes",
                      c->id, (long)bblen);
    }
    return status;
}

apr_status_t h2_conn_io_flush(h2_conn_io *io)
{
    apr_time_t start = 0;
    apr_status_t status;
    
    if (io->needs_flush > 0) {
        /* this is a buffer size triggered flush, let's measure how
         * long it takes and try to adjust our speed factor accordingly */
        start = apr_time_now();
    }
    status = pass_output(io, 1, NULL);
    check_write_size(io);
    if (start && status == APR_SUCCESS) {
        apr_time_t duration = apr_time_now() - start;
        if (duration < apr_time_from_msec(100)) {
            io->speed_factor *= 1.0 + ((apr_time_from_msec(100) - duration) 
                                        / (float)apr_time_from_msec(100));
            ap_log_cerror(APLOG_MARK, APLOG_TRACE3, status, io->c,
                          "h2_conn_io(%ld): incr speed_factor to %f",
                          io->c->id, io->speed_factor);
        }
        else if (duration > apr_time_from_msec(200)) {
            io->speed_factor *= 0.5;
            ap_log_cerror(APLOG_MARK, APLOG_TRACE3, status, io->c,
                          "h2_conn_io(%ld): decr speed_factor to %f",
                          io->c->id, io->speed_factor);
        }
    }
    io->needs_flush = -1;
    return status;
}

apr_status_t h2_conn_io_write_eoc(h2_conn_io *io, h2_session *session)
{
    return pass_output(io, 1, session);
}

apr_status_t h2_conn_io_write(h2_conn_io *io, const char *data, size_t length)
{
    apr_status_t status = APR_SUCCESS;
    apr_size_t remain;
    
    if (io->buffer_output) {
        while (length > 0) {
            remain = assure_scratch_space(io);
            if (remain >= length) {
#if LOG_SCRATCH
                ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, io->c, APLOGNO(03389)
                              "h2_conn_io(%ld): write_to_scratch(%ld)", 
                              io->c->id, (long)length); 
#endif
                memcpy(io->scratch + io->slen, data, length);
                io->slen += length;
                length = 0;
            }
            else {
#if LOG_SCRATCH
                ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, io->c, APLOGNO(03390)
                              "h2_conn_io(%ld): write_to_scratch(%ld)", 
                              io->c->id, (long)remain); 
#endif
                memcpy(io->scratch + io->slen, data, remain);
                io->slen += remain;
                data += remain;
                length -= remain;
            }
        }
    }
    else {
        status = apr_brigade_write(io->output, NULL, NULL, data, length);
    }
    io->needs_flush = -1;
    return status;
}

apr_status_t h2_conn_io_pass(h2_conn_io *io, apr_bucket_brigade *bb)
{
    apr_bucket *b;
    apr_status_t status = APR_SUCCESS;
    
    while (!APR_BRIGADE_EMPTY(bb) && status == APR_SUCCESS) {
        b = APR_BRIGADE_FIRST(bb);
        
        if (APR_BUCKET_IS_METADATA(b)) {
            /* need to finish any open scratch bucket, as meta data 
             * needs to be forward "in order". */
            append_scratch(io);
            APR_BUCKET_REMOVE(b);
            APR_BRIGADE_INSERT_TAIL(io->output, b);
        }
        else if (io->buffer_output) {
            apr_size_t remain = assure_scratch_space(io);
            if (b->length > remain) {
                apr_bucket_split(b, remain);
                if (io->slen == 0) {
                    /* complete write_size bucket, append unchanged */
                    APR_BUCKET_REMOVE(b);
                    APR_BRIGADE_INSERT_TAIL(io->output, b);
#if LOG_SCRATCH
                    ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, io->c, APLOGNO(03391)
                                  "h2_conn_io(%ld): pass bucket(%ld)", 
                                  io->c->id, (long)b->length);
#endif
                    continue;
                }
            }
            else {
                /* bucket fits in remain, copy to scratch */
                status = read_to_scratch(io, b);
                apr_bucket_delete(b);
                continue;
            }
        }
        else {
            /* no buffering, forward buckets setaside on flush */
            if (APR_BUCKET_IS_TRANSIENT(b)) {
                apr_bucket_setaside(b, io->c->pool);
            }
            APR_BUCKET_REMOVE(b);
            APR_BRIGADE_INSERT_TAIL(io->output, b);
        }
    }
    io->needs_flush = -1;
    return status;
}

int h2_conn_io_needs_flush(h2_conn_io *io)
{
    if (io->needs_flush < 0) {
        apr_off_t len;
        apr_brigade_length(io->output, 0, &len);
        if (len > (io->pass_threshold * io->flush_factor * io->speed_factor)) {
            /* don't want to keep too much around */
            io->needs_flush = 1;
            return 1;
        }
        io->needs_flush = 0;
    }
    return 0;
}
