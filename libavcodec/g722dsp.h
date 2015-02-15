/*
 * Copyright (c) 2015 Peter Meerwald <pmeerw@pmeerw.net>
 *
 * This file is part of Libav.
 *
 * Libav is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * Libav is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with Libav; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef AVCODEC_G722DSP_H
#define AVCODEC_G722DSP_H

#include <stdint.h>

typedef struct G722DSPContext {
    void (*apply_qmf)(const int16_t *prev_samples, int *xout1, int *xout2);
} G722DSPContext;

void ff_g722dsp_init(G722DSPContext *c);

#endif /* AVCODEC_G722DSP_H */
