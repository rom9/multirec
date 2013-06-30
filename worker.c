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

#include <stdio.h>
#include <pthread.h>

#include "worker.h"
#include "main.h"

#undef SHORT_CIRCUIT

pthread_mutex_t workerMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t wrk;

int finished = 0;

static float floatIn[BSIZ*MR_CHANNELS];
static float floatOut[MAXOUTFRMS*MR_CHANNELS];


MR_SAMPLE *leftData, *rightData;
static MRFrame *tmpOutBuf;

#include "logging.inc"

/**
 * Stretches the given audio chunk to align it to the audio of the master device
 */
int conve(MRDevice *c, MRAlsaChunk *chunk, int end, MRFrame *outBuf,
		long *outputLen) {
	// *** Auto-adjust algorithm : re-calculate src ratio to obtain the same number
	// *** of output frames as the master device.

	// Time difference (in frames) between now and the last read from the master
	// device.
	long long tsDiff = ((chunk->ts / (long long) CPS)
			- (chunk->masterTS / (long long) CPS));

	// Given the above time difference and the master pcm delay, estimate how many
	// frames the master device has captured in this instant.
	unsigned long long framesThatShouldHaveBeen = (chunk->masterFrameCount
			+ chunk->masterDelay + tsDiff);

	// This (slave) device should have captured the same amount of frames in this
	// same instant. Calculate the frame difference considering this pcm delay.
	long diff = framesThatShouldHaveBeen
			- (c->outputFrameCount + chunk->len + chunk->delay);

	c->srcData.data_in = floatIn;
	c->srcData.data_out = floatOut;
	c->srcData.output_frames = MAXOUTFRMS;

	// Now, calculate a proper ratio to make that difference disappear.
	// (...or, how much this input chunk has to be stretched in order to
	// have the same total output frames as master?)
	double ratio = ((double) (chunk->len + diff)) / chunk->len;
	c->srcData.src_ratio = ratio;
	int err = src_set_ratio(c->srcState, ratio);
	if (err < 0) {
		log_error("dev %d : src_set_ratio error: %s\n", c->idx,
				src_strerror(err));
		return -1;
	}

	log_debug("dev %d : outcount=%llu tsDiff=%lld diff=%ld new ratio=%f\n",
			c->idx, c->outputFrameCount, tsDiff, diff, ratio);

	// *** Convert input data to float, put the result in the SRC input buffer. ***
	src_short_to_float_array((short*) chunk->buf, c->srcData.data_in,
			chunk->len * MR_CHANNELS);

	// *** Stretch audio in the SRC input buffer ***
	c->srcData.input_frames = chunk->len;
	c->srcData.end_of_input = end;

	int errn = src_process(c->srcState, &(c->srcData));
	if (errn) {
		log_error("dev %d : src_process error : %s\n", c->idx,
				src_strerror(errn));
		return -1;
	}

	// Number of output frames to read from buf
	*outputLen = c->srcData.output_frames_gen;

	// *** Convert SRC output buffer data from float back to short. ***
	int size = c->srcData.output_frames_gen * MR_CHANNELS;
	// TODO OPTIMIZATION : i have to store each channel in a separate
	// output buffer. Would be best to do it in the same loop as the
	// float-to-short conversion.
	src_float_to_short_array(c->srcData.data_out, (short*) outBuf, size);

	return 0; // success
}


void splitStereo(MRFrame *stereoBuf, int len, int invert, MR_SAMPLE *left,
		MR_SAMPLE *right) {
	// Split each (stereo) frame into 2 mono samples, and appends them to the left
	// and right output buffers.
	// Optimization : if signal from this device has to be inverted, a
	// separate loop is used.
	// TODO : could be smarter!!
	if (invert) {
		while (len) {
			len--;
			*left = ((MR_SAMPLE) 0xFFFF) - stereoBuf->v[0]; // Append left sample value to left buffer
			*right = ((MR_SAMPLE) 0xFFFF) - stereoBuf->v[1]; // Append right sample value to right buffer
			stereoBuf++;
			left++;
			right++;
		}
	} else {
		while (len) {
			len--;
			*left = stereoBuf->v[0]; // Append left sample value to left buffer
			*right = stereoBuf->v[1]; // Append right sample value to the right buffer
			stereoBuf++;
			left++;
			right++;
		}
	}
}


void *diskWorker(void *arg) {
	MRDevice *currentDev;
	while (1) {
		// Consume data from all the queues, starting from the one associated with
		// the 1st device. Exit when all devices' queues are empty.
		int i;
		for (i = 0; i < devCount; i++) {
			currentDev = devices[i];

			MRAlsaChunk *cnk = (MRAlsaChunk*) cons_own(currentDev->dualQueue);

			if (cnk == NULL)
				continue;

			// FIXME FIXME
			if (cnk->len == 0) {
				log_debug("\nDBG---discarding empty bucket from dev %d\n",
						currentDev->idx);
				cons_free(currentDev->dualQueue);
				continue;
			}

			log_debug("\nDBG---gotit (len= %d)\n", cnk->len);

			MRFrame* outBuf;
			long outLen;

			// don't stretch audio coming from dev 0
			// don't stretch if no data has been read from master device yet.
			if (i == 0 || cnk->masterFrameCount == 0) {
				outBuf = cnk->buf;
				outLen = cnk->len;
			} else {
				outBuf = tmpOutBuf;

				int end = (state == STOPPING
						&& currentDev->dualQueue->full.head != NULL) ? 1 : 0;

				unsigned long long t = rdtsc();
				if (conve(currentDev, cnk, end, outBuf, &outLen)) {
					log_error("Error stretching audio from dev %d",
							currentDev->idx);
					finish(-1);
				}
				log_debug("conversion time =%llu us\n",
						((rdtsc() - t) / (CPMillis / 1000)));

			}

#ifdef MRSLOW
			// Fuzzy "handbrake" just for stress testing...
			unsigned long long zz = rdtsc();
			unsigned long long diff = CPMillis* (abs(random()) % 85);
			while(rdtsc()-zz < diff);
#endif

			// Update the total frame count recorded by this device.
			currentDev->outputFrameCount += outLen;

			// *** split stereo audio to dual mono ***
			splitStereo(outBuf, outLen, currentDev->invert, leftData,
					rightData);

			// *** write output to audio file ***
			sf_writef_short(currentDev->outFile[0], leftData, outLen);
			sf_writef_short(currentDev->outFile[1], rightData, outLen);

			// *** release the audio chunk to its queue ***
			cons_free(currentDev->dualQueue);

		} // end for

		// No more jobs pending.
		// Check if we are stopping: if so, finalize and truncate the output files.
		// Otherwise, sleep for 10millis
		// then check again for new jobs.
		if (finished) {
			// TODO perform files truncation...
			break;
		} else {
			usleep(500);
		}

	} // end while
	return NULL;
}


/**
 * Allocate buffers needed for conversion & output, and create the "consumer" thread.
 */
void initWorker() {
	FILE* log = fopen("output.log", "w+");
	initLogging(log, DEBUG);

	// allocate 2 mono output buffers
	tmpOutBuf = (MRFrame*) malloc(sizeof(MRFrame) * MAXOUTFRMS);

	// allocate 2 mono output buffers
	leftData = (MR_SAMPLE*) malloc(sizeof(MR_SAMPLE) * MAXOUTFRMS);
	rightData = (MR_SAMPLE*) malloc(sizeof(MR_SAMPLE) * MAXOUTFRMS);

#ifndef SHORT_CIRCUIT
	if (pthread_create(&wrk, NULL, diskWorker, NULL)) {
		printf("error creating thread.");
		finish(-1);
	}
#endif
}


void waitPendingJobs() {
	// No more jobs will be committed.
	finished = 1;
	if (pthread_join(wrk, NULL)) {
		printf("error joining thread.");
	}
}
