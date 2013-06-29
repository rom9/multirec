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

#ifndef MULTIREC_H
#define MULTIREC_H

#include <alsa/asoundlib.h>
#include <sndfile.h>
#include <samplerate.h>

#include "buffer_queue.h"

#define MR_SAMPLE short  // Data type that will store sample data.
#define MR_CHANNELS 2    // number of channels per device

#define BSIZ 262144 // Alsa buffer size (n. of frames)


/**
 * libsamplerate output buffer size (in frames)
 * this is a bit bigger than the alsa buffer size, to allow space for
 * stretching if needed.
 */
#define MAXOUTFRMS  (BSIZ+1000)


// *** Types ***

/**
 * Frame data ( 1 frame = MR_CHANNELS samples )
 */
typedef struct MRFrame_s
{
 	MR_SAMPLE v[MR_CHANNELS];
} MRFrame;


/**
 * Audio data chunk
 */
typedef struct MRAlsaChunk_s
{
	MRFrame buf[BSIZ];
	unsigned long len;

 	// Timestamp telling when this audio chunk was read.
 	unsigned long long ts;
	// Amount of delay this PCM had when reading this audio data chunk.
 	snd_pcm_sframes_t delay;

 	// State of the master device when this chunk was read.
	unsigned long long masterFrameCount;
 	unsigned long long masterTS;
 	snd_pcm_sframes_t masterDelay;

} MRAlsaChunk;


/**
 * Per-device stuff...
 */
typedef struct MRDevice_s
{
	char idx;
	char *name;
	unsigned int idxBit;
	int invert;

	unsigned int pref_buffer_time;	/* preferred buffer time for this device, in us */
	unsigned int pref_period_time;	/* preferred period time for this device, in us */
	unsigned int act_period_time;	/* actual period time for this device, in us */

	snd_pcm_uframes_t buffer_size;
	snd_pcm_uframes_t period_size;

	// Total count of output frames written so far, for this device.
	unsigned long long outputFrameCount;

	// Alsa PCM stuff...
	snd_pcm_t *handle;


	MR_SAMPLE peaks[MR_CHANNELS];
	

	DualQueue *dualQueue;

	// libsamplerate stuff...
	SRC_STATE *srcState;
	SRC_DATA srcData;
	
	MRAlsaChunk *partialBucket;

	// libsndfile stuff...
	SNDFILE* outFile[MR_CHANNELS]; // libsndfile handle (1 file per channel !!)

	// pcm thread
	pthread_t thread;

} MRDevice;



// *** Global varz ***


enum States {
	MONITORING=0,
	RECORDING,
	STOPPING,
	SKIP
} state;


extern unsigned long long CPS;
extern unsigned int CPMillis;

extern MRDevice **devices;
extern size_t devCount;



// *** Funcz ***


unsigned long long int rdtsc();

MRDevice** getDeviceArray(size_t *n);

void init(const char *out);

void startRecording();

void stopRecording();


#endif  // MULTIREC_H
