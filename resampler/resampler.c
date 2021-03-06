/*
 *  resampler.c - A really brainless app for doing rational resampling of
 *      samples being passed between applications.
 *
 *  Copyright (c)2017 Phil Vachon <phil@security-embedded.com>
 *
 *  This file is a part of The Standard Library (TSL)
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <filter/filter.h>
#include <filter/sample_buf.h>
#include <filter/complex.h>

#include <app/app.h>

#include <config/engine.h>

#include <tsl/diag.h>
#include <tsl/errors.h>
#include <tsl/assert.h>

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#define RES_MSG(sev, sys, msg, ...) MESSAGE("RESAMPLER", sev, sys, msg, ##__VA_ARGS__)

static
unsigned interpolate = 1;

static
unsigned decimate = 1;

static
unsigned input_sample_rate = 0;

static
int in_fifo = -1;

static
int out_fifo = -1;

static
int16_t *filter_coeffs = NULL;

static
size_t nr_filter_coeffs = 0;

static
struct polyphase_fir *pfir = NULL;

static
bool dc_blocker = false;

static
void _usage(const char *appname)
{
    RES_MSG(SEV_INFO, "USAGE", "%s -I [interpolate] -D [decimate] -F [filter file] -S [sample rate] [-b] [in_fifo] [out_fifo]",
            appname);
    RES_MSG(SEV_INFO, "USAGE", "        -b      Enable DC blocking filter");
    exit(EXIT_SUCCESS);
}

static
void _set_options(int argc, char * const argv[])
{
    int arg = -1;
    const char *filter_file = NULL;
    struct config *cfg CAL_CLEANUP(config_delete) = NULL;
    double *filter_coeffs_f = NULL;

    while ((arg = getopt(argc, argv, "I:D:S:F:bh")) != -1) {
        switch (arg) {
        case 'I':
            interpolate = strtoll(optarg, NULL, 0);
            break;
        case 'D':
            decimate = strtoll(optarg, NULL, 0);
            break;
        case 'S':
            input_sample_rate = strtoll(optarg, NULL, 0);
            break;
        case 'F':
            filter_file = optarg;
            break;
        case 'b':
            dc_blocker = true;
            RES_MSG(SEV_INFO, "DC-BLOCKER-ENABLED", "Enabling DC Blocking Filter.");
            break;
        case 'h':
            _usage(argv[0]);
            break;
        }
    }

    if (optind > argc) {
        RES_MSG(SEV_FATAL, "MISSING-SRC-DEST", "Missing source/destination file");
        exit(EXIT_FAILURE);
    }

    if (0 == decimate) {
        RES_MSG(SEV_FATAL, "BAD-DECIMATION", "Decimation factor must be a non-zero integer.");
        exit(EXIT_FAILURE);
    }

    if (0 == decimate) {
        RES_MSG(SEV_FATAL, "BAD-INTERPOLATION", "Interpolation factor must be a non-zero integer.");
        exit(EXIT_FAILURE);
    }

    if (NULL == filter_file) {
        RES_MSG(SEV_FATAL, "BAD-FILTER-FILE", "Need to specify a filter JSON file.");
        exit(EXIT_FAILURE);
    }

    RES_MSG(SEV_INFO, "CONFIG", "Resampling: %u/%u from %u to %f", interpolate, decimate, input_sample_rate,
            ((double)interpolate/(double)decimate)*(double)input_sample_rate);
    RES_MSG(SEV_INFO, "CONFIG", "Loading filter coefficients from '%s'", filter_file);

    TSL_BUG_IF_FAILED(config_new(&cfg));

    if (FAILED(config_add(cfg, filter_file))) {
        RES_MSG(SEV_INFO, "BAD-CONFIG", "Configuration file '%s' cannot be processed, aborting.",
                filter_file);
        exit(EXIT_FAILURE);
    }

    TSL_BUG_IF_FAILED(config_get_float_array(cfg, &filter_coeffs_f, &nr_filter_coeffs, "lpfCoeffs"));
    TSL_BUG_IF_FAILED(TCALLOC((void **)&filter_coeffs, sizeof(int16_t) * nr_filter_coeffs, (size_t)1));

    for (size_t i = 0; i < nr_filter_coeffs; i++) {
        double q15 = 1 << Q_15_SHIFT;
        filter_coeffs[i] = (int16_t)(filter_coeffs_f[i] * q15);
    }

    if (0 > (in_fifo = open(argv[optind], O_RDONLY))) {
        RES_MSG(SEV_INFO, "BAD-INPUT", "Bad input - cannot open %s", argv[optind]);
        exit(EXIT_FAILURE);
    }

    if (0 > (out_fifo = open(argv[optind + 1], O_WRONLY))) {
        RES_MSG(SEV_INFO, "BAD-OUTPUT", "Bad output - cannot open %s", argv[optind + 1]);
        exit(EXIT_FAILURE);
    }
}

static
aresult_t _free_sample_buf(struct sample_buf *buf)
{
    TSL_BUG_ON(NULL == buf);
    TFREE(buf);
    return A_OK;
}

#define NR_SAMPLES                  1024

static
aresult_t _alloc_sample_buf(struct sample_buf **pbuf)
{
    aresult_t ret = A_OK;

    struct sample_buf *buf = NULL;

    TSL_ASSERT_ARG(NULL != pbuf);

    if (FAILED(ret = TCALLOC((void **)&buf, NR_SAMPLES * sizeof(int16_t) + sizeof(struct sample_buf), 1ul))) {
        goto done;
    }

    buf->refcount = 0;
    buf->sample_type = COMPLEX_INT_16;
    buf->sample_buf_bytes = NR_SAMPLES * sizeof(int16_t);
    buf->nr_samples = 0;
    buf->release = _free_sample_buf;
    buf->priv = NULL;

    *pbuf = buf;

done:
    return ret;
}

static
int16_t output_buf[NR_SAMPLES];

struct dc_blocker {
    /**
     * Filter pole coefficient for leaky integrator. In Q.15 representation.
     */
    int32_t p;

    /**
     * Prior input sample, for the differentiator. x[n-1]
     */
    int32_t x_n_1;

    /**
     * Prior output sample for feedback, in Q.15. y[n-1]
     */
    int32_t y_n_1;

    /**
     * The noise shaper filter value. This is technically in Q.30
     */
    int32_t f;

    /**
     * Accumulator
     */
    int32_t acc;
};

static inline
aresult_t dc_blocker_apply(struct dc_blocker *blocker, int16_t *samples, size_t nr_samples)
{
    aresult_t ret = A_OK;

    TSL_ASSERT_ARG(NULL != blocker);
    TSL_ASSERT_ARG(NULL != samples);
    TSL_ASSERT_ARG(0 != nr_samples);

    DIAG("DC_BLOCK(%zu samples)", nr_samples);

    for (size_t i = 0; i < nr_samples; i++) {
        blocker->acc -= blocker->x_n_1;
        blocker->x_n_1 = samples[i] << Q_15_SHIFT;
        blocker->acc += blocker->x_n_1 - blocker->p * blocker->y_n_1;
        blocker->y_n_1 = blocker->acc >> Q_15_SHIFT;
        samples[i] = blocker->y_n_1;
    }

    return ret;
}

static
aresult_t process_fir(void)
{
    int ret = A_OK;

    struct dc_blocker blck;
    memset(&blck, 0, sizeof(blck));
    blck.p = (int16_t)((1.0 - 0.9999) * (double)(1 << Q_15_SHIFT));

    do {
        int op_ret = 0;
        struct sample_buf *read_buf = NULL;
        size_t new_samples = 0;
        bool full = false;

        TSL_BUG_IF_FAILED(polyphase_fir_full(pfir, &full));

        if (false == full) {
            TSL_BUG_IF_FAILED(_alloc_sample_buf(&read_buf));

            if (0 >= (op_ret = read(in_fifo, read_buf->data_buf, read_buf->sample_buf_bytes))) {
                int errnum = errno;
                ret = A_E_INVAL;
                RES_MSG(SEV_FATAL, "READ-FIFO-FAIL", "Failed to read from input fifo: %s (%d)",
                        strerror(errnum), errnum);
                goto done;
            }

            DIAG("Read %d bytes from input FIFO", op_ret);

            TSL_BUG_ON((1 & op_ret) != 0);

            read_buf->nr_samples = op_ret/sizeof(int16_t);

            TSL_BUG_IF_FAILED(polyphase_fir_push_sample_buf(pfir, read_buf));
        }

        /* Filter the samples */
        TSL_BUG_IF_FAILED(polyphase_fir_process(pfir, output_buf, NR_SAMPLES, &new_samples));
        TSL_BUG_ON(0 == new_samples);

        /* Apply DC blocker, if asked */
        if (true == dc_blocker) {
            TSL_BUG_IF_FAILED(dc_blocker_apply(&blck, output_buf, new_samples));
        }

        /* Write them out */
        if (0 > (op_ret = write(out_fifo, output_buf, new_samples * sizeof(int16_t)))) {
            int errnum = errno;
            ret = A_E_INVAL;
            RES_MSG(SEV_FATAL, "WRITE-FIFO-FAIL", "Failed to write to output fifo: %s (%d)",
                    strerror(errnum), errnum);
            goto done;
        }

        DIAG("Wrote %d bytes to output FIFO", op_ret);
    } while (app_running());

done:
    return ret;
}

int main(int argc, char * const argv[])
{
    int ret = EXIT_FAILURE;

    TSL_BUG_IF_FAILED(app_init("resampler", NULL));
    TSL_BUG_IF_FAILED(app_sigint_catch(NULL));

    _set_options(argc, argv);
    TSL_BUG_IF_FAILED(polyphase_fir_new(&pfir, nr_filter_coeffs, filter_coeffs, interpolate, decimate));

    RES_MSG(SEV_INFO, "STARTING", "Starting polyphase resampler");

    if (FAILED(process_fir())) {
        RES_MSG(SEV_FATAL, "FIR-FAILED", "Failed during filtering.");
        goto done;
    }

    ret = EXIT_SUCCESS;

done:
    polyphase_fir_delete(&pfir);
    return ret;
}

