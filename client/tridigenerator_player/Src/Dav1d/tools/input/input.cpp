/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

//#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include <android/log.h>
#include "Core/Logging.h"

#include "Dav1d/common/attributes.h"
#include "Dav1d/common/intops.h"

#include "Dav1d/tools/input/input.h"
#include "Dav1d/tools/input/demuxer.h"

struct DemuxerContext {
    DemuxerPriv *data;
    const Demuxer *impl;
    uint64_t priv_data[];
};

extern const Demuxer ivf_demuxer;
extern const Demuxer annexb_demuxer;
extern const Demuxer section5_demuxer;
static const Demuxer *const demuxers[] = {
        &ivf_demuxer,
        &annexb_demuxer,
        &section5_demuxer,
        NULL
};

int input_open(DemuxerContext **const c_out,
               const char *const name,
               std::vector<uint8_t> sourceData,
               unsigned fps[2],
               unsigned *const num_frames,
               unsigned timebase[2])
{
    const Demuxer *impl = NULL;
    DemuxerContext *c;
    int i;

    // This function now only supports memory-based input.
    // The 'name' parameter is for selecting a specific demuxer by name.
    // If 'name' is null, we probe.

    const uint8_t *sourceData_ptr = sourceData.data();
    size_t sourceData_sz = sourceData.size();

    if (name) {
        for (i = 0; demuxers[i]; i++) {
            if (!strcmp(demuxers[i]->name, name)) {
                impl = demuxers[i];
                break;
            }
        }
        if (!demuxers[i]) {
            LOGE("Failed to find demuxer named \"%s\"", name);
            return DAV1D_ERR(ENOPROTOOPT);
        }
    } else {
        int probe_sz = 0;
        for (i = 0; demuxers[i]; i++)
            probe_sz = imax(probe_sz, demuxers[i]->probe_sz);

        if (probe_sz > 0) {
            uint8_t *const probe_data = static_cast<uint8_t*>(malloc(probe_sz));
            if (!probe_data) {
                LOGE("Failed to allocate memory for probe");
                return DAV1D_ERR(ENOMEM);
            }

            if (sourceData_sz < static_cast<size_t>(probe_sz)) {
                LOGE("Source data (%zu) is too small for Probe buffer size (%d)", sourceData_sz, probe_sz);
                free(probe_data);
                return DAV1D_ERR(ENOMEM);
            }
            memcpy(probe_data, sourceData_ptr, probe_sz);

            for (i = 0; demuxers[i]; i++) {
                if (demuxers[i]->probe(probe_data)) {
                    impl = demuxers[i];
                    break;
                }
            }
            free(probe_data);
            if (!demuxers[i]) {
                LOGE("Failed to probe demuxer");
                return DAV1D_ERR(ENOPROTOOPT);
            }
        } else {
            LOGE("No demuxers available for probing.");
            return DAV1D_ERR(ENOPROTOOPT);
        }
    }

    if (!(c = static_cast<DemuxerContext*>(calloc(1, offsetof(DemuxerContext, priv_data) + impl->priv_data_size)))) {
        LOGE("Failed to allocate memory for DemuxerContext");
        return DAV1D_ERR(ENOMEM);
    }

    c->impl = impl;
    c->data = (DemuxerPriv *) c->priv_data;

    // This is the memory path. The file path is removed.
    // Ensure your demuxer implementation (e.g., ivf.cpp) has an 'open_mem' function.
    if (impl->open_mem(c->data, sourceData_ptr, sourceData_sz, fps, num_frames, timebase)) {
        LOGE("Failed to open demuxer from memory");
        free(c);
        return -1; // Or appropriate error
    }

    *c_out = c;
    return 0;
}

int input_read(DemuxerContext *const ctx, Dav1dData *const data) {
    return ctx->impl->read(ctx->data, data);
}

int input_seek(DemuxerContext *const ctx, const uint64_t pts) {
    return ctx->impl->seek ? ctx->impl->seek(ctx->data, pts) : -1;
}

void input_close(DemuxerContext *const ctx) {
    ctx->impl->close(ctx->data);
    free(ctx);
}
