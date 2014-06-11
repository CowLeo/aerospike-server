/*
 * thr_sindex.h
 *
 * Copyright (C) 2013-2014 Aerospike, Inc.
 *
 * Portions may be licensed to Aerospike, Inc. under one or more contributor
 * license agreements.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 */

/*
 * secondary index function declarations
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>

#include "queue.h"
#include "ai_obj.h"
#include "hist.h"

typedef enum {
	AS_SINDEX_GC_VALIDATE_OBJ = 1,
	AS_SINDEX_GC_DELETE_OBJ   = 2,
	AS_SINDEX_GC_PIMD_RLOCK   = 3,
	AS_SINDEX_GC_PIMD_WLOCK   = 4 
} sindex_gc_hist;

extern pthread_rwlock_t sindex_rwlock;
extern cf_queue *g_sindex_populate_q;
extern cf_queue *g_sindex_destroy_q;
extern cf_queue *g_sindex_populateall_done_q;
extern bool      g_sindex_boot_done;

void as_sindex_thr_init();
void as_sindex_gc_histogram_dumpall();
void sindex_gc_hist_insert_data_point(sindex_gc_hist hist, uint64_t start_time);
