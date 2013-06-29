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

#ifndef BUFFER_QUEUE_H_
#define BUFFER_QUEUE_H_


#include <stdlib.h>
#include <pthread.h>


/**
 * buffer_queue.c
 * Dual bucket queue.
 * Remember: TAIL-FIRST queues! element enters at the tail, exits at the head.
 *  Created on: Jan 8, 2010
 *      Author: rom
 */


typedef struct Bucket_s {
	struct Bucket_s *prev, *next;
	void *ptr;
} Bucket;

struct Queue_s {
	Bucket *head, *tail;
};

typedef struct DualQueue_s {
	unsigned int contentSize;
	struct Queue_s empty, full;
	Bucket *producerOwned, *consumerOwned;
	pthread_mutex_t mutex;
} DualQueue;



DualQueue* create(unsigned char bucketCount, unsigned int contentSize);

void *prod_own(DualQueue *dq);
void prod_free(DualQueue *dq);
int prod_len(DualQueue *dq);

int has_grown();

void *cons_own(DualQueue *dq);
void cons_free(DualQueue *dq);
int cons_len(DualQueue *dq);



#endif /* BUFFER_QUEUE_H_ */
