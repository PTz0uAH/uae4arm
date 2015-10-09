 /* 
  * Minimalistic sound.c implementation for gp2x
  * (c) notaz, 2007
  */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#ifndef ANDROID
#include <sys/soundcard.h>
#endif
#include <semaphore.h>
#include <pthread.h>
#include <errno.h>
#include <sched.h>

#include "sysconfig.h"
#include "sysdeps.h"
#include "uae.h"
#include "options.h"
#include "memory-uae.h"
#include "audio.h"
#include "gensound.h"
#include "sounddep/sound.h"
#include "threaddep/thread.h"
#include "custom.h"
#include "savestate.h"
#include <SDL.h>

#ifdef ANDROIDSDL
#include <android/log.h>
#endif

int produce_sound=0;
int changed_produce_sound=0;


#define SOUND_USE_SEMAPHORES
#define SOUND_BUFFERS_COUNT 4
uae_u16 sndbuffer[SOUND_BUFFERS_COUNT][SNDBUFFER_LEN*DEFAULT_SOUND_CHANNELS+32];
unsigned n_callback_sndbuff, n_render_sndbuff;
uae_u16 *sndbufpt = sndbuffer[0];
uae_u16 *render_sndbuff = sndbuffer[0];
uae_u16 *finish_sndbuff = sndbuffer[0] + SNDBUFFER_LEN*2; 

#ifdef NO_SOUND


void finish_sound_buffer (void) {  }

int setup_sound (void) { sound_available = 0; return 0; }

void close_sound (void) { }

int init_sound (void) { return 0; }

void pause_sound (void) { }

void resume_sound (void) { }

void uae4all_init_sound(void) { }

void uae4all_play_click(void) { }

void uae4all_pause_music(void) { }

void uae4all_resume_music(void) { }

#else 


static int have_sound = 0;
static int lastfreq;

extern unsigned int new_beamcon0;

static __inline__ void sound_default_evtime(int freq)
{
	int pal = new_beamcon0 & 0x20;

  if (freq < 0)
  	freq = lastfreq;
  lastfreq = freq;

			if (pal)
				scaled_sample_evtime = (MAXHPOS_PAL * MAXVPOS_PAL * freq * CYCLE_UNIT + currprefs.sound_freq - 1) / currprefs.sound_freq;
			else
				scaled_sample_evtime = (MAXHPOS_NTSC * MAXVPOS_NTSC * freq * CYCLE_UNIT + currprefs.sound_freq - 1) / currprefs.sound_freq;
}


static int s_oldrate = 0, s_oldbits = 0, s_oldstereo = 0;
static int sound_thread_active = 0, sound_thread_exit = 0;
static uae_sem_t sound_sem, callback_sem;

#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
static int cnt = 0;

static void sound_thread_mixer(void *ud, Uint8 *stream, int len)
{
	if (sound_thread_exit) return;
	int sem_val;
	sound_thread_active = 1;
	/*
	// Sound is choppy, arrgh
	sem_getvalue(&sound_sem, &sem_val);
	while (sem_val > 1)
	{
		//printf("skip %i (%i)\n", cnt, sem_val);
		uae_sem_wait(&sound_sem);
		cnt++;
		sem_getvalue(&sound_sem, &sem_val);
	}
	*/

#ifdef SOUND_USE_SEMAPHORES
	uae_sem_wait(&sound_sem);
	uae_sem_post(&callback_sem);
#endif
	cnt++;
	//__android_log_print(ANDROID_LOG_INFO, "UAE4ALL2","Sound callback cnt %d buf %d\n", cnt, cnt%SOUND_BUFFERS_COUNT);
	if(currprefs.sound_stereo)
		memcpy(stream, sndbuffer[cnt%SOUND_BUFFERS_COUNT], MIN(SNDBUFFER_LEN*2, len));
	else
	  	memcpy(stream, sndbuffer[cnt%SOUND_BUFFERS_COUNT], MIN(SNDBUFFER_LEN, len));

}

static int pandora_start_sound(int rate, int bits, int stereo)
{
	int frag = 0, buffers, ret;
	unsigned int bsize;
	static int audioOpened = 0;

	if (!sound_thread_active)
	{
		// init sem, start sound thread
		printf("starting sound thread..\n");
		uae_sem_init(&sound_sem, 0, 0);
		uae_sem_init(&callback_sem, 0, 0);
//		if (ret != 0) printf("uae_sem_init() failed: %i, errno=%i\n", ret, errno);
	}

	// if no settings change, we don't need to do anything
	if (rate == s_oldrate && s_oldbits == bits && s_oldstereo == stereo)
	    return 0;

	if( audioOpened ) {
		// __android_log_print(ANDROID_LOG_INFO, "UAE4ALL2", "UAE tries to open SDL sound device 2 times, ignoring that.");
		//	SDL_CloseAudio();
		return 0;
	}

	SDL_AudioSpec as;
	memset(&as, 0, sizeof(as));

	// __android_log_print(ANDROID_LOG_INFO, "UAE4ALL2", "Opening audio: rate %d bits %d stereo %d", rate, bits, stereo);
	as.freq = rate;
	as.format = (bits == 8 ? AUDIO_S8 : AUDIO_S16);
	as.channels = (stereo ? 2 : 1);
	if(currprefs.sound_stereo)
	  as.samples = SNDBUFFER_LEN*2 / as.channels / 2;
	else
	  as.samples = SNDBUFFER_LEN / as.channels / 2;
	as.callback = sound_thread_mixer;
	SDL_OpenAudio(&as, NULL);
	audioOpened = 1;

	s_oldrate = rate; 
	s_oldbits = bits; 
	s_oldstereo = stereo;

	SDL_PauseAudio (0);

	return 0;
}


// this is meant to be called only once on exit
void pandora_stop_sound(void)
{
	if (sound_thread_exit)
		printf("don't call gp2x_stop_sound more than once!\n");
	if (sound_thread_active)
	{
		printf("stopping sound thread..\n");
		sound_thread_exit = 1;
		uae_sem_post(&sound_sem);
		//usleep(100*1000);
	}
	SDL_PauseAudio (1);
}

static int wrcnt = 0;

void finish_sound_buffer (void)
{
#ifdef SOUND_USE_SEMAPHORES
	uae_sem_post(&sound_sem);
	uae_sem_wait(&callback_sem);
#endif
	wrcnt++;
	sndbufpt = render_sndbuff = sndbuffer[wrcnt%SOUND_BUFFERS_COUNT];
	if(currprefs.sound_stereo)
	  finish_sndbuff = sndbufpt + SNDBUFFER_LEN;
	else
	  finish_sndbuff = sndbufpt + SNDBUFFER_LEN/2;	
}

void restart_sound_buffer(void)
{
	sndbufpt = render_sndbuff = sndbuffer[wrcnt&3];
	if(currprefs.sound_stereo)
	  finish_sndbuff = sndbufpt + SNDBUFFER_LEN;
	else
	  finish_sndbuff = sndbufpt + SNDBUFFER_LEN/2;	  
}


/* Try to determine whether sound is available.  This is only for GUI purposes.  */
int setup_sound (void)
{
  if (pandora_start_sound(currprefs.sound_freq, 16, currprefs.sound_stereo) != 0)
    return 0;

  sound_available = 1;
  return 1;
}

void update_sound (int freq)
{
  sound_default_evtime(freq);
}

static int open_sound (void)
{
  if (pandora_start_sound(currprefs.sound_freq, 16, currprefs.sound_stereo) != 0)
	    return 0;

  init_sound_table16 ();

  sample_handler = currprefs.sound_stereo ? sample16s_handler : sample16_handler;

  have_sound = 1;
  sound_available = 1;

  if(currprefs.sound_stereo)
    sample_handler = sample16s_handler;
  else
    sample_handler = sample16_handler;
 
  return 1;
}

void close_sound (void)
{
  if (!have_sound)
	  return;

  // testing shows that reopenning sound device is not a good idea on gp2x (causes random sound driver crashes)
  // we will close it on real exit instead
  //pandora_stop_sound();
  have_sound = 0;
}

int init_sound (void)
{
    have_sound=open_sound();
    return have_sound;
}

void pause_sound (void)
{
    /* nothing to do */
}

void resume_sound (void)
{
    /* nothing to do */
}

void reset_sound (void)
{
  if (!have_sound)
  	return;

  memset(sndbuffer, 0, 2 * 4 * (SNDBUFFER_LEN+32)*DEFAULT_SOUND_CHANNELS);
}

#endif

