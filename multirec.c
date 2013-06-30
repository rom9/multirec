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
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <pthread.h>
#include <dirent.h>
#include <sched.h>
#include <alsa/asoundlib.h>

#include "multirec.h"
#include "main.h"
#include "worker.h"


// *** Global vars ***


/** Number of CPU clock cycles per second (measured via rdtsc) */
unsigned long long CPS = 0; // MUST be set to nonzero!

/** (optimization) : clock cycles per millisecond (always equal to CPS*1000) */
unsigned int CPMillis = 0; // MUST be set to nonzero!

/** Output directory where the recorded .wav files will be */
const char *outDir;


MRDevice **devices;  /** Array of active devices, read from .rc file */
size_t devCount;	     /** Number of active devices */


static MRFrame tmpBuf[BSIZ]; /** Temporary alsa buffer */


/** Sample format */
snd_pcm_format_t format = SND_PCM_FORMAT_S16_LE;

unsigned int rate = 48000;   /** Stream rate */

snd_output_t *output = NULL;  /** Alsa logging output */


/** Possible state machine events coming from the GUI */
enum Requests {
	REQ_NONE=0,
	REQ_START,
	REQ_STOP,
} request;



/**
 * Main thread: this only runs a trivial state machine waiting for
 * start / stop requests from the GUI
 */
static pthread_t mainThread;

/** pthreads barrier to start recording in sync */
static pthread_barrier_t stateBarrier;

/** pthreads mutex guarding access to master device data */
static pthread_mutex_t masterMutex = PTHREAD_MUTEX_INITIALIZER;

/** Global count of output frames for the master device. */
static unsigned long long masterFrameCount = 0;

/** Delay (in frames) of the last alsa buffer read from the master device */
static snd_pcm_sframes_t masterDelay = 0;

/** Timestamp (got from rdtsc) of the most recent read from the master device */
static unsigned long long masterTS = 0;


#include "logging.inc"


/**
 * Read the configuration file and initialize devices data structures.
 */
void readConfig()
{
	FILE *frc = fopen("multirec.rc", "r");

	char line[1024];
	char delims[] = " \t\r\n";
	devCount=0;
	while (fgets(line, 1024, frc) != NULL)
	{
		char *pound = strchr(line, '#');
		if(pound)
			*pound = '\0'; // Cuts line after #
		
		char *s = strtok(line, delims);
		if(!s)
			continue;
		char *devName = calloc(1,strlen(s));
		strcpy(devName, s);
		
		char *inv = strtok(NULL, delims);
		if(!inv)
			continue;

		char *buftm = strtok(NULL, delims);
		if(!buftm)
			continue;

		char *pertm = strtok(NULL, delims);
		if(!pertm)
			continue;
		
		// Device found in .rc file
		// Initializing new MRDevice object.
		MRDevice *crd = calloc(1, sizeof(MRDevice));
		crd->name = devName;
		crd->idx = devCount;
		crd->idxBit = 1<<devCount;

		crd->invert = (inv[0]=='0' ? 0 : 1);
		
		crd->pref_buffer_time = atoi(buftm);
		crd->pref_period_time = atoi(pertm);

		// Allocate dual queue for this device
		crd->dualQueue = create(6, sizeof(MRAlsaChunk));

		crd->partialBucket = NULL;

		// (Re)Allocate device array and add the new pointer.
		devices = (MRDevice **)realloc(devices, sizeof(MRDevice*) * (devCount+1) );
		devices[devCount] = crd;
		
		devCount++;
	}

	fclose(frc);
}


/**
 * RDTSC assembly instruction: basically returns a counter of CPU clock cycles
 * (...yes, it wraps around every few hours / days).
 */
inline unsigned long long int rdtsc()
{
	unsigned long long int x;
	__asm__ volatile (".byte 0x0f, 0x31" : "=A" (x));
	return x;
}



void calcPeakLevels(MRDevice *c, MRFrame *ptr, snd_pcm_sframes_t actual)
{
	// Reset peaks
	MR_SAMPLE maxL = 0,
			  maxR = 0;

	snd_pcm_sframes_t i;
	MR_SAMPLE v;
	for(i=0; i<actual; i++)
	{
		v = abs(ptr->v[0]);
		if(v > maxL)
			maxL = v;
		
		v = abs(ptr->v[1]);
		if(v > maxR)
			maxR = v;

		ptr++;
	}

	c->peaks[0] = maxL;
	c->peaks[1] = maxR;
}


int initSrc()
{
	
	MRDevice *c;
	int i, errn;
	for(i=0; i<devCount; i++)
	{
		c = devices[i];

		c->srcState = src_new(SRC_LINEAR, MR_CHANNELS, &errn);
		if(c->srcState==NULL)
		{
			printf("error: %d\n", errn);
			return -1;
		}

	}
	return 0;
}


void commitChunk(MRDevice *c, MRAlsaChunk *cnk) {
	// Read complete. Hand the buffer to the worker
	pthread_mutex_lock( &masterMutex ); // >> critical section
	cnk->masterFrameCount = masterFrameCount;
	cnk->masterTS = masterTS;
	cnk->masterDelay = masterDelay;
	pthread_mutex_unlock( &masterMutex ); //<< critical section

	log_debug("  DBG---producing %d frames...\n", cnk->len);

	prod_free(c->dualQueue);
	c->partialBucket = NULL;

	log_debug("  DBG---bucket produced ok\n");
	log_debug("  DBG---(full buckets = %d,\n", prod_len(c->dualQueue));
	log_debug("  DBG--- empty buckets = %d)\n", cons_len(c->dualQueue));
}


/**
 * Wait until data is available on the specified device, then read the captured
 * data into ptr.
 * Also, feed the audio data to the VU meters.
 */
inline int capture(MRDevice *c, MRFrame *ptr, snd_pcm_sframes_t *len,
		snd_pcm_sframes_t *delay, unsigned long long *timeStamp) {
	int err;
	snd_pcm_t *pcm_handle = c->handle;

	snd_pcm_wait(c->handle, c->act_period_time/1000);

	err = snd_pcm_delay(pcm_handle, delay);
	if (err < 0) {
		log_dev_error(c, "pcm_delay error: %s\n", snd_strerror(err));
		finish(-1);
		return -1;
	}

	*timeStamp = rdtsc();

	snd_pcm_sframes_t actual = snd_pcm_readi(pcm_handle, ptr, c->period_size);

	*len = actual;

	if (actual < 0) {
		log_dev_error(c, "snd_pcm_readi error: %s\n", snd_strerror(actual));
		// No use in recovering a xrun.
		// TODO exit gracefully, state=STOP_REQUEST and let the UI know...
		finish(-1);
		return -1;
	}

	log_dev_debug(c, "read = %lu frames, delay = %lu frames.\n", actual, *delay);

	calcPeakLevels(c, ptr, actual);

	return 0;
}


/**
 * Capture audio data and discard it (just feed the VU meters).
 */
static inline void doMonitor(MRDevice *c)
{
	snd_pcm_sframes_t len, delay;
	unsigned long long ts;

	capture(c, tmpBuf, &len, &delay, &ts);

}


/**
 * Capture audio data and append it to the current bucket. If the bucket becomes
 * full, send it to the outbound queue: a fresh one will be fetched from the
 * inbound queue on the next call.
 */
static inline void doRecord(MRDevice *c)
{
	// FIXME partialBucket is redundant! Could be done through producerOwned.
	MRAlsaChunk *cnk = c->partialBucket;
	if(cnk==NULL) {
		// Get a fresh buffer from the dualQueue.
		cnk = (MRAlsaChunk*)prod_own(c->dualQueue);
		cnk->len = 0;
		c->partialBucket = cnk;
	}

	MRFrame *ptr = cnk->buf + cnk->len;

	if(!ptr) {
		log_dev_error(c, "FATAL : queue returned null pointer.");
		finish(-1);
	}

	if(has_grown())
		log_dev_debug(c, "** queue has grown! ** ");

	snd_pcm_sframes_t len = 0;
	capture(c, ptr, &len, &(cnk->delay), &(cnk->ts) );
	if(len<=0)
		return;

	cnk->len += len;

	// If this is the master device, update the related global vars (within a
	// critical section)
	if (c->idx == 0) {
		pthread_mutex_lock( &masterMutex ); // >> critical section
		masterFrameCount += len;
		masterDelay = cnk->delay;
		masterTS = cnk->ts;
		pthread_mutex_unlock( &masterMutex ); //<< critical section
	}

	// Current bucket is full. Release it to the queue.
	if(cnk->len > 50000)
		commitChunk(c, cnk);

}


static inline void barrier() {
	if (pthread_barrier_wait(&stateBarrier) == EINVAL) {
		log_error("FATAL : barrier wait error.");
		finish(-1);
	}
}


/**
 * Device thread code. Continuously read data from the specified device, and send
 * it to the disk worker or just to the VU-meters, according to the current state.
 * If the state becomes STOPPING, wait on the barrier and exit.
 */
void deviceLoop(MRDevice *c) {
	while (1) {
		switch (state) {
		case RECORDING:
			doRecord(c);
			break;
		case MONITORING:
			doMonitor(c);
			break;
		case SKIP:
			// Hold on until state changes...
			log_dev_debug(c, "barrier wait...\n");
			barrier();
			log_dev_debug(c, " ...go!!\n");
			break;
		case STOPPING:
			// flush the last partialBucket
			if (c->partialBucket && c->partialBucket->len > 0)
				commitChunk(c, c->partialBucket);
			barrier();
			log_dev_debug(c, "stopped.\n");
			return;
		}
	}
}


int fileNameFilter(const struct dirent *fname)
{
	// file name pattern is "trk_NNNN_D.wav"
	//   N = trk number (zero padded)
	//   D = soundcard id (A = hw:0, B = hw:1, ...)
	return (strlen(fname->d_name)==8 &&
	        fname->d_name[0]>='0' && fname->d_name[1]<='9' &&
	        fname->d_name[2]=='_' &&
	        fname->d_name[3]>='a' && fname->d_name[3]<='z' &&
	        strncmp( &fname->d_name[4], ".wav", 4)==0 ) ? 1 : 0;
}


/**
 * Create the output .wav files for the session to be recorded. If needed, also
 * create the output directory.
 * File name pattern is "./DIR/NN_c.wav"
 *   DIR = track name (passed in as program argument)
 *   N = session number starting from 1 (zero padded)
 *   c = channel id (a = hw:0/left, b = hw:0/right, c = hw:1/left, ...)
 */
int openFiles() {
	// Attempt to create outDir. If it already exists, go on...
	if(mkdir(outDir, 0777)==-1 && errno!=EEXIST)
		return -1;

	int lastNum = 0;

	// Scan dir contents to find the next available session number.
	struct dirent **namelist;
	int n;
	n = scandir(outDir, &namelist, fileNameFilter, alphasort);
	if (n < 0) {
		perror("scandir");
	} else {
		while(n--) {
			if(lastNum==0)
				lastNum = atoi( namelist[n]->d_name );
			free(namelist[n]);
		}
		free(namelist);
	}

	int session = lastNum +1;

	// First available session number found. Go on and actually open new files...
	char fname[256];
	int i, chan;
	for(i=0; i<devCount; i++) {
		MRDevice *c	= devices[i];
		for(chan=0; chan<MR_CHANNELS; chan++) {
			sprintf(fname, "./%s/%02d_%c.wav", outDir, session, 'a'+((i*MR_CHANNELS)+chan) );
			
			// Open one mono wav file per channel per device.
			SF_INFO sfi;
			sfi.samplerate = rate;
			sfi.channels = 1;
			sfi.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16 | SF_ENDIAN_LITTLE;
			
			log_debug("Trying to open %s ... ", fname);
			c->outFile[chan] = sf_open(fname, SFM_WRITE, &sfi);

			if(c->outFile[chan]==NULL) {
				log_debug("  %s\n", sf_strerror (NULL));
				return -1;
			}
			
			log_debug("  OK.\n");
		}
	}
	return 0;
}


// TODO
int closeFile(MRDevice *c) {
	int n;
	for(n=0; n<MR_CHANNELS; n++)
		if(c->outFile[n] )
			sf_close(c->outFile[n]);
		
	return 0;
}


/**
 * Restart audio capture in-sync, and start actually write audio data to disk!
 */
void initRecording() {
	int i, err;

	initWorker(devices, devCount);

	// Unlink PCMs, so we can drop them individually.
	for(i=1; i<devCount; i++) {
		MRDevice *c = devices[i];
		if ( (err=snd_pcm_unlink(c->handle)) <0 ) {
			log_dev_error(c, "Unlink failed: %s\n", snd_strerror(err));
			finish(-1);
		}
	}

	// *** Re-initializing devices ***
	for(i=0; i<devCount; i++) {
		MRDevice *c = devices[i];
		// *** Stop previous capture to restart in-sync...
		err = snd_pcm_drop(c->handle);
		if (err < 0) {
			log_dev_error(c, "Drop error: %s\n", snd_strerror(err));
			finish(-1);
		}

		err = snd_pcm_prepare(c->handle);
        if (err < 0) {
        	log_dev_error(c, "Prepare failed: %s\n", snd_strerror(err));
			finish(-1);
		}

        // Link all other devices to the master device
		if (i>0) {
			if ( (err=snd_pcm_link(devices[0]->handle, c->handle)) <0 ) {
				log_dev_error(c, "Link failed: %s\n", snd_strerror(err));
				finish(-1);
			}
			log_dev_debug(c, "Linked devz 0 and %d\n", i);
		}

		// Reset the total output frame count for this device
		c->outputFrameCount = 0L;

	}

	// *** Open output files ***
	if(openFiles()<0) {
		log_error("Error opening files\n");
		finish(-1);
	}


	// *** Initialize sample rate converter
	initSrc();

	// *** Start capturing ! ***
	err = snd_pcm_start(devices[0]->handle);
	if (err < 0) {
		log_error("Start error: %s\n", snd_strerror(err));
		finish(-1);
	}

}


/**
 * Main thread code. Starts audio capture on all devices, without writing data to
 * disk.
 * Then, just loop forever waiting for start / stop events coming from
 * the GUI.
 */
void *mainLoop(void *arg)
{
	int err;

	state = SKIP;

	// *** Re-initializing devices ***
	MRDevice *c;
	int i;
	for(i=0; i<devCount; i++) {
		c = devices[i];

		err = snd_pcm_prepare(c->handle);
        if (err < 0) {
        	log_dev_error(c, "Prepare failed: %s\n", snd_strerror(err));
        	finish(-1);
		}

		if (i>0) {
			if ( (err=snd_pcm_link(devices[0]->handle, c->handle)) <0 ) {
				log_dev_error(c, "Link failed: %s\n", snd_strerror(err));
				finish(-1);
			}
			log_dev_debug(c, "Linked devz 0 and %d\n", i);
		}

		// Create device thread
		if (pthread_create(&(c->thread), NULL, (void *) deviceLoop, (void *) c))
		{
			log_dev_error(c, "error creating thread %d.\n", i);
			finish(-1);
		}
	}


	// *** Start capturing ! ***
	log_debug("Start !!\n");
	err = snd_pcm_start(devices[0]->handle);
	if (err < 0) {
		log_debug("Start error: %s\n", snd_strerror(err));
		finish(-1);
	}

	state = MONITORING;
	barrier();

	int run = 1;
	while(run) {
		switch(state) {
		case MONITORING:
			if(request == REQ_START) {
				state = SKIP;
				barrier();

				initRecording();

				state = RECORDING;
				barrier();
			}
			else if(request == REQ_STOP) {
				state = STOPPING;
				barrier();
				run=0;
			}
			break;
		case RECORDING:
			if(request == REQ_STOP) {
				state = STOPPING;
				barrier();
				// TODO finalization!!!
				waitPendingJobs();
				run=0;
			}
			break;
		default:
			break;
		}

		usleep(100000);
	}
	
//	for(i=0; i<devCount; i++) {
//		c = devices[i];
//		pthread_join(c->thread, NULL);
//	}


	return (void*)0;
}


/**
 * Start recording captured audio to disk
 */
void startRecording() {
	request = RECORDING;
}



/**
 * Stop recording and close output files
 */
void stopRecording() {
	// Set the request flag and wait for the main thread to terminate.
	request = STOPPING;
	pthread_join(mainThread, NULL);
}



/**
 * Returns the array of active audio devices (and its size)
 */
MRDevice** getDeviceArray(size_t *n)
{
	*n = devCount;
	return devices;
}


/*
 * Tedious ALSA stuff, mostly copied from the docs...
 */
static int set_hwparams(MRDevice *card, snd_pcm_hw_params_t *params)
{
	snd_pcm_t *handle = card->handle;
	int err, dir;

	/* choose all parameters */
	err = snd_pcm_hw_params_any(handle, params);
	if (err < 0) {
		printf("Broken configuration for capture: no configurations available: %s\n", snd_strerror(err));
		return err;
	}
	/* set hardware resampling */
	err = snd_pcm_hw_params_set_rate_resample(handle, params, 0);  // no resampl
	if (err < 0) {
		printf("Resampling setup failed for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* set the interleaved read/write format */
	err = snd_pcm_hw_params_set_access(handle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		printf("Access type not available for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* set the sample format */
	err = snd_pcm_hw_params_set_format(handle, params, format);
	if (err < 0) {
		printf("Sample format not available for capture: %s\n", snd_strerror(err));
		return err;
	}
	/* set the count of channels */
	err = snd_pcm_hw_params_set_channels(handle, params, MR_CHANNELS);
	if (err < 0) {
		printf("Channels count (%i) not available for captures: %s\n", MR_CHANNELS, snd_strerror(err));
		return err;
	}

	/* set the stream rate */
	unsigned int rrate = rate;
	err = snd_pcm_hw_params_set_rate_near(handle, params, &rrate, 0);
	if (err < 0) {
		printf("Rate %iHz not available for capture: %s\n", rate, snd_strerror(err));
		return err;
	}
	if (rrate != rate)
		log_dev_debug(card, "Rate doesn't match (requested %iHz, get %iHz)\n", rate, rrate);

	/* set the buffer time */
	err = snd_pcm_hw_params_set_buffer_time_near(handle, params, &card->pref_buffer_time, &dir);
	if (err < 0) {
		printf("Unable to set buffer time %i for capture: %s\n",
				card->pref_buffer_time, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_get_buffer_size(params, &card->buffer_size);
	if (err < 0) {
		printf("Unable to get buffer size for capture: %s\n", snd_strerror(err));
		return err;
	}

	/* set the period time */
	err = snd_pcm_hw_params_set_period_time_near(handle, params, &card->pref_period_time, &dir);
	if (err < 0) {
		printf("Unable to set period time %i for capture: %s\n",
				card->pref_period_time, snd_strerror(err));
		return err;
	}
	err = snd_pcm_hw_params_get_period_size(params, &card->period_size, &dir);
	if (err < 0) {
		printf("Unable to get period size for capture: %s\n", snd_strerror(err));
		return err;
	}

	err = snd_pcm_hw_params_get_period_time(params, &card->act_period_time, &dir);
	if (err < 0) {
		printf("Unable to get period time for capture: %s\n", snd_strerror(err));
		return err;
	}

	/* write the parameters to device */
	err = snd_pcm_hw_params(handle, params);
	if (err < 0) {
		printf("Unable to set hw params for capture: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}


/*
 * ...some more tedious ALSA stuff ...
 */
static int set_swparams(MRDevice *card, snd_pcm_sw_params_t *swparams)
{
	snd_pcm_t *handle = card->handle;
	int err;

	/* get the current swparams */
	err = snd_pcm_sw_params_current(handle, swparams);
	if (err < 0) {
		printf("Unable to determine current swparams for capture: %s\n", snd_strerror(err));
		return err;
	}

	/* allow the transfer when at least period_size samples can be processed */
	err = snd_pcm_sw_params_set_avail_min(handle, swparams, card->period_size);
	if (err < 0) {
		printf("Unable to set avail min for capture: %s\n", snd_strerror(err));
		return err;
	}


	/* write the parameters to the capture device */
	err = snd_pcm_sw_params(handle, swparams);
	if (err < 0) {
		printf("Unable to set sw params for capture: %s\n", snd_strerror(err));
		return err;
	}
	return 0;
}


/*
 * Initializes the specified device, preparing it to start capture
 */
static int cardInit(MRDevice *card)
{
	int err;

	if ((err = snd_pcm_open(&card->handle, card->name, SND_PCM_STREAM_CAPTURE, 0)) < 0)
	{
		printf("Capture open error: %s\n", snd_strerror(err));
		finish(-1);
	}

	snd_pcm_hw_params_t *hwparams;
	snd_pcm_sw_params_t *swparams;

	snd_pcm_hw_params_alloca(&hwparams);
	snd_pcm_sw_params_alloca(&swparams);

	if ((err = set_hwparams(card, hwparams)) < 0) {
		printf("Setting of hwparams failed: %s\n", snd_strerror(err));
		finish(-1);
	}
	if ((err = set_swparams(card, swparams)) < 0) {
		printf("Setting of swparams failed: %s\n", snd_strerror(err));
		finish(-1);
	}


	snd_pcm_dump(card->handle, output);


	return 0;
}


/**
 * Initializes the entire program:
 * - open the log file
 * - read config file
 * - perform clock calibration
 * - initialize all audio devices
 * - Start audio capture. Do not actually write data to disk, just monitor through
 *   the VU-meters
 */
void init(const char* out)
{
	outDir = out;

    FILE* lf = fopen("out.log", "w+");
	initLogging(lf, ERROR);

	log_debug("sof = %lu\n\n", sizeof(MRFrame));
	
	readConfig();
	log_debug("DevCount = %lu\n", devCount);
	
	// Calc. cycles per sample
	unsigned long long int t1, t2;
	t1 = rdtsc();
	sleep(1);
	t2 = rdtsc();
	CPS = (t2-t1)/rate;
	CPMillis = (t2-t1)/1000;
	log_debug("CPSample = %u\n\n", CPS);

	int err = snd_output_stdio_attach(&output, logFile, 0);
	if (err < 0) {
		log_debug("Output failed: %s\n", snd_strerror(err));
		return;
	}


	// Initialize audio devices
	int i;
	for(i=0; i<devCount; i++)
		cardInit(devices[i]);

	fflush(logFile);


	// Reset state and event vars
	request = REQ_NONE;
	state = SKIP;


	// Initialize a barrier with devCount+1 participants (one for the mainThread
	// and one for each device)
	err = pthread_barrier_init(&stateBarrier, NULL, devCount + 1);
	if (err != 0) {
		log_error("FATAL : error initializing state barrier.");
		finish(-1);
	}

	pthread_attr_t attr;

    err = pthread_attr_init(&attr);
    if (err != 0) {
		log_error("FATAL : error initializing main thread attributes.");
		finish(-1);
    }

//    struct sched_param param;
//    param.sched_priority = 0;
//    pthread_attr_setschedparam(&attr, &param);

    // Create and start the main thread
	if ( pthread_create( &mainThread, NULL, mainLoop, NULL) ) {
		printf("error creating thread.");
		finish(-1);
	}

}


/*
 * Testing main for filename generation machinery
int main (int argc, char *argv[])
{
	if(argc<2)
	{
		printf("Output dir not specified!\n");
		return -1;
	}
	
	outDir = argv[1];
	devCount = 3;
	
	openFiles();
	
	return 0;
}
*/
