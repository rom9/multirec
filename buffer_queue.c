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

#include "buffer_queue.h"

static int flg_grown = 0;

// *** initialization/destruction functions ***

Bucket* createBucket(unsigned int contentSize) {
	Bucket *rv = (Bucket*)malloc(sizeof(Bucket));
	rv->ptr = calloc(1, contentSize);
	return rv;
}

void destroyBucket(Bucket* bkt) {
	// Warning: you won't have access to next and prev fields after destroying a
	// bucket!!
	free(bkt->ptr); // destroy contents of the bucket first...
	free(bkt); // destroy the bucket.
}


DualQueue* create(unsigned char bucketCount, unsigned int contentSize) {
	DualQueue *rv = (DualQueue*)malloc(sizeof(DualQueue));
	rv->contentSize = contentSize;
	pthread_mutex_init( &(rv->mutex), NULL);

	rv->empty.head = createBucket(contentSize);

	Bucket *e = rv->empty.head;
	int i;
	for(i=1; i<bucketCount; i++) {
		Bucket *n = createBucket(contentSize);
		e->next = n;
		n->prev = e;
		e = n;
	}

	rv->empty.tail = e;

	// close the circle...
	e->next = rv->empty.head;
	rv->empty.head->prev = e;

	return rv;
}


void destroy(DualQueue* dq) {

	int again;
	Bucket *bkt, *nxt;

	bkt = dq->empty.head;
	if(bkt)
		do {
			nxt = bkt->next; // Store a pointer to next bucket before destroying
							// the current one...
			again = (bkt != dq->empty.tail);
			destroyBucket(bkt);
			bkt = nxt;
		} while( again );

	bkt = dq->full.head;  // This queue should already be empty, since all buckets
						 // will have been consumed.
	if(bkt)
		do {
			nxt = bkt->next; // Store a pointer to next bucket before destroying
							// the current one...
			again = (bkt != dq->full.tail);
			destroyBucket(bkt);
			bkt = nxt;
		} while( again );

	free(dq);
}

// ***


// private funct.
Bucket * grow(DualQueue *dq, Bucket *insertionPoint) {
	Bucket *n = createBucket(dq->contentSize);

	n->next = insertionPoint->next;
	n->prev = insertionPoint;

	insertionPoint->next->prev = n;
	insertionPoint->next = n;

	return n;
}

inline Bucket* _poll(struct Queue_s *q) {
	// Return the head bucket.
	Bucket *rv = q->head;

	// If it was the tail bucket as well, then it was the only one. So, this queue
	// is now empty.
	if(rv==q->tail) {
		q->head=0;
		q->tail=0;
	}
	else // Otherwise, shift buckets and set new head.
		q->head = q->head->next;

	return rv;
}

inline void _offer(struct Queue_s *q, Bucket* b) {
	q->tail = b; // Add the newcomer at the tail.

	// If this queue has no head, then the newcomer is the only bucket in the
	// queue. So, it is both at head and tail.
	if(!q->head)
		q->head = b;
}


// *** Exposed funtions ***

// * Producer (capture device) side *

/**
 * Gets a fresh buffer for storing captured data. Locks the returned buffer until
 * prod_free() is called.
 *
 * Warning! A return value of zero is an error condition. It means "No fresh
 * buffer available!" Bad thing, queue should have grown automatically, this
 * shouldn't happen.
 */
void* prod_own(DualQueue *dq) {
	pthread_mutex_lock( &(dq->mutex) ); // >> critical section
	dq->producerOwned = _poll( &(dq->empty) );
	pthread_mutex_unlock( &(dq->mutex) ); // << exit critical section

	// If no bucket was found in the queue, return null. Otherwise, return the
	// associated buffer.
	return dq->producerOwned ?  dq->producerOwned->ptr : 0;
}

/**
 * Unlocks the buffer previously returned by prod_own(). The buffer is now ready
 * for consumption by the worker thread. Argument 'len' is the number of bytes
 * actually written by the producer.
 */
void prod_free(DualQueue *dq) {
	pthread_mutex_lock( &(dq->mutex) ); // >> critical section

	_offer( &(dq->full), dq->producerOwned);

	// Preparation: if no empty bucket is available for polling at the next loop,
	// create a new one.
	if(!dq->empty.head) {
		// insert the newly created bucket after the one produced right now.
		dq->empty.head = grow(dq, dq->producerOwned);
		dq->empty.tail = dq->empty.head;
		flg_grown = 1;
	}


	dq->producerOwned = 0;
	pthread_mutex_unlock( &(dq->mutex) ); // << exit critical section
}


int prod_len(DualQueue *dq) {
	pthread_mutex_lock( &(dq->mutex) ); // >> critical section
	Bucket *b = dq->full.head;

	int rv;
	for(rv=0; b && b!=dq->full.tail; rv++)
		b = b->next;

	pthread_mutex_unlock( &(dq->mutex) ); // << exit critical section
	return rv;
}

/**
 * Tells whether the producer queue has grown since last call to this function,
 * and lowers the flag.
 */
int has_grown() {
	int rv = flg_grown;
	flg_grown = 0;
	return rv;
}


// * Consumer (worker) side *

/**
 * Gets the next buffer full of captured data from the queue. Locks the returned
 * buffer until cons_free() is called.
 * Called by the worker thread when it's ready to process data from this queue.
 */
void *cons_own(DualQueue *dq) {
	pthread_mutex_lock( &(dq->mutex) ); // >> critical section
	dq->consumerOwned = _poll( &(dq->full) );
	pthread_mutex_unlock( &(dq->mutex) ); // << exit critical section

	// If no bucket was found in the queue, return null. Otherwise, return the
	// associated buffer.
	if (dq->consumerOwned)
		return dq->consumerOwned->ptr;
	else
		return 0;
}

/**
 * Unlocks the buffer previously returned by cons_own(). The buffer has already
 * been consumed and is now ready to be picked up by the producer for storing
 * more data.
 */
void cons_free(DualQueue *dq) {
	pthread_mutex_lock( &(dq->mutex) ); // >> critical section
	_offer( &(dq->empty), dq->consumerOwned);
	dq->consumerOwned = 0;
	pthread_mutex_unlock( &(dq->mutex) ); // << exit critical section
}


int cons_len(DualQueue *dq) {
	pthread_mutex_lock( &(dq->mutex) ); // >> critical section
	Bucket *b = dq->empty.head;

	int rv;
	for(rv=0; b && b!=dq->empty.tail; rv++)
		b = b->next;

	pthread_mutex_unlock( &(dq->mutex) ); // << exit critical section
	return rv;
}


// ***

//#define qdebug
#ifdef qdebug
#include <stdio.h>

void dump(DualQueue *dq) {
	Bucket *b = dq->empty.head;
	int rv;
	for(rv=0; b;) {
		printf("%x; ", (unsigned long)b);
		if(b == dq->empty.tail)
			break;
		b = b->next;
	}
	printf("\n", b);
}

int main(int argc, char **argv) {

	DualQueue *dq = create(3, 16);
	printf("INIT - ");
	dump(dq);

	void *p, *c;

	int i, j;
	for(i=0; i<30; i++) {
		p = prod_own(dq);
		prod_free(dq);
		printf("PROD < ");
		dump(dq);

		if(random() % 2)
			for(j=0; j<2; j++) {
				c = cons_own(dq);
				if(c) {
					cons_free(dq);
					printf("CONS > ");
					dump(dq);
				}
				else
					printf("CONS (idle)\n");
			}


	}

	for(j=0; j<10; j++) {
		c = cons_own(dq);
		if(c) {
			cons_free(dq);
			printf("CONS > ");
			dump(dq);
		}
		else {
			printf("CONS (finish)\n");
			break;
		}
	}


	destroy(dq);

	return 0;
}
#endif
