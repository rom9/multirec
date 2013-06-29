/*
 * Copyright 2010 Alberto Romei
 * 
 * This file is part of Multirec.
 *
 * Multirec is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Multirec is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Multirec.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef WORKER_H
#define WORKER_H

#include "multirec.h"

struct DiskJob_s
{
	struct DiskJob_s *next;

	MR_SAMPLE *leftData;
	MR_SAMPLE *rightData;
	int length;
	SNDFILE* leftFile;
	SNDFILE* rightFile;
	int isLastChunk;
};
typedef struct DiskJob_s DiskJob;


void initWorker();


/**
 * Wait for worker thread to consume all pending buckets.
 */
void waitPendingJobs();


#endif
