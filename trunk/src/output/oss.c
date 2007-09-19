/*
	oss: audio output via Open Sound System

	copyright ?-2006 by the mpg123 project - free software under the terms of the LGPL 2.1
	see COPYING and AUTHORS files in distribution or http://mpg123.org
	initially written by Michael Hipp
*/

#include <sys/ioctl.h>
#include <fcntl.h>

#include "mpg123.h"

#ifdef HAVE_LINUX_SOUNDCARD_H
#include <linux/soundcard.h>
#endif

#ifdef HAVE_SYS_SOUNDCARD_H
#include <sys/soundcard.h>
#endif

#ifdef HAVE_MACHINE_SOUNDCARD_H
#include <machine/soundcard.h>
#endif

#ifndef AFMT_S16_NE
# ifdef OSS_BIG_ENDIAN
#  define AFMT_S16_NE AFMT_S16_BE
# else
#  define AFMT_S16_NE AFMT_S16_LE
# endif
#endif

#ifndef AFMT_U16_NE
# ifdef OSS_BIG_ENDIAN
#  define AFMT_U16_NE AFMT_U16_BE
# else
#  define AFMT_U16_NE AFMT_U16_LE
# endif
#endif


static int rate_best_match_oss(audio_output_t *ao)
{
	int ret,dsp_rate;
	
	if(!ao || ao->fn < 0 || ao->rate < 0) return -1;
	dsp_rate = ao->rate;
	
	ret = ioctl(ao->fn, SNDCTL_DSP_SPEED,&dsp_rate);
	if(ret < 0) return ret;
	ao->rate = dsp_rate;
	return 0;
}

static int set_rate_oss(audio_output_t *ao)
{
	int dsp_rate;
	int ret = 0;
	
	if(ao->rate >= 0) {
		dsp_rate = ao->rate;
		ret = ioctl(ao->fn, SNDCTL_DSP_SPEED,&dsp_rate);
	}
	return ret;
}

static int set_channels_oss(audio_output_t *ao)
{
	int chan = ao->channels - 1;
	int ret;
	
	if(ao->channels < 0) return 0;
	
	ret = ioctl(ao->fn, SNDCTL_DSP_STEREO, &chan);
	if(chan != (ao->channels-1)) return -1;

	return ret;
}

static int set_format_oss(audio_output_t *ao)
{
	int sample_size,fmts;
	int sf,ret;
	
	if(ao->format == -1) return 0;

	switch(ao->format) {
		case AUDIO_FORMAT_SIGNED_16:
		default:
			fmts = AFMT_S16_NE;
			sample_size = 16;
			break;
		case AUDIO_FORMAT_UNSIGNED_8:
			fmts = AFMT_U8;
			sample_size = 8;
		break;
		case AUDIO_FORMAT_SIGNED_8:
			fmts = AFMT_S8;
			sample_size = 8;
		break;
		case AUDIO_FORMAT_ULAW_8:
			fmts = AFMT_MU_LAW;
			sample_size = 8;
		break;
		case AUDIO_FORMAT_ALAW_8:
			fmts = AFMT_A_LAW;
			sample_size = 8;
		break;
		case AUDIO_FORMAT_UNSIGNED_16:
			fmts = AFMT_U16_NE;
		break;
	}
	
#if 0
	if(ioctl(ao->fn, SNDCTL_DSP_SAMPLESIZE, &sample_size) < 0)
		return -1;
#endif

	sf = fmts;
	ret = ioctl(ao->fn, SNDCTL_DSP_SETFMT, &fmts);
	if(sf != fmts) return -1;

	return ret;
}


static int reset_parameters_oss(audio_output_t *ao)
{
	int ret;
	ret = ioctl(ao->fn, SNDCTL_DSP_RESET, NULL);
	if(ret < 0) error("Can't reset audio!");
	ret = set_format_oss(ao);
	if (ret == -1) goto err;
	ret = set_channels_oss(ao);
	if (ret == -1) goto err;
	ret = set_rate_oss(ao);
	if (ret == -1) goto err;

	/* Careful here.  As per OSS v1.1, the next ioctl() commits the format
	 * set above, so we must issue SNDCTL_DSP_RESET before we're allowed to
	 * change it again. [dk]
	 */
   
/*  FIXME: this needs re-enabled (but not using global variables this time):
	if (ioctl(ao->fn, SNDCTL_DSP_GETBLKSIZE, &outburst) == -1 ||
      outburst > MAXOUTBURST)
    outburst = MAXOUTBURST;
*/

err:
	return ret;
}


static int open_oss(audio_output_t *ao)
{
	char usingdefdev = 0;
	
	if(!ao) return -1;
	
	if(!ao->device) {
		ao->device = "/dev/dsp";
		usingdefdev = 1;
	}
	
	ao->fn = open(ao->device,O_WRONLY);  
	
	if(ao->fn < 0)
	{
		if(usingdefdev) {
			ao->device = "/dev/sound/dsp";
			ao->fn = open(ao->device,O_WRONLY);
			if(ao->fn < 0) {
				error("Can't open default sound device!");
				return -1;
			}
		} else {
			error1("Can't open %s!",ao->device);
			return -1;
		}
	}
	
	if(reset_parameters_oss(ao) < 0) {
		close(ao->fn);
		return -1;
	}
	
	if(ao->gain >= 0) {
		int e,mask;
		e = ioctl(ao->fn , SOUND_MIXER_READ_DEVMASK ,&mask);
		if(e < 0) {
			error("audio/gain: Can't get audio device features list.");
		}
		else if(mask & SOUND_MASK_PCM) {
			int gain = (ao->gain<<8)|(ao->gain);
			e = ioctl(ao->fn, SOUND_MIXER_WRITE_PCM , &gain);
		}
		else if(!(mask & SOUND_MASK_VOLUME)) {
			error1("audio/gain: setable Volume/PCM-Level not supported by your audio device: %#04x",mask);
		}
		else { 
			int gain = (ao->gain<<8)|(ao->gain);
			e = ioctl(ao->fn, SOUND_MIXER_WRITE_VOLUME , &gain);
		}
	}
	
	return ao->fn;
}



/*
 * get formats for specific channel/rate parameters
 */
static int get_formats_oss(audio_output_t *ao)
{
	int fmt = 0;
	int r = ao->rate;
	int c = ao->channels;
	int i;
	
	static int fmts[] = { 
		AUDIO_FORMAT_ULAW_8 , AUDIO_FORMAT_SIGNED_16 ,
		AUDIO_FORMAT_UNSIGNED_8 , AUDIO_FORMAT_SIGNED_8 ,
		AUDIO_FORMAT_UNSIGNED_16 , AUDIO_FORMAT_ALAW_8
	};
	
	/* Reset is required before we're allowed to set the new formats. [dk] */
	ioctl(ao->fn, SNDCTL_DSP_RESET, NULL);
	
	for(i=0;i<6;i++) {
		ao->format = fmts[i];
		if(set_format_oss(ao) < 0) {
			continue;
		}
		ao->channels = c;
		if(set_channels_oss(ao) < 0) {
			continue;
		}
		ao->rate = r;
		if(rate_best_match_oss(ao) < 0) {
			continue;
		}
		if( (ao->rate*100 > r*(100-AUDIO_RATE_TOLERANCE)) && (ao->rate*100 < r*(100+AUDIO_RATE_TOLERANCE)) ) {
			fmt |= fmts[i];
		}
	}


#if 0
	if(ioctl(ao->fn,SNDCTL_DSP_GETFMTS,&fmts) < 0) {
		error("Failed to get SNDCTL_DSP_GETFMTS");
		return -1;
	}

	if(fmts & AFMT_MU_LAW)
		ret |= AUDIO_FORMAT_ULAW_8;
	if(fmts & AFMT_S16_NE)
		ret |= AUDIO_FORMAT_SIGNED_16;
	if(fmts & AFMT_U8)
		ret |= AUDIO_FORMAT_UNSIGNED_8;
	if(fmts & AFMT_S8)
		ret |= AUDIO_FORMAT_SIGNED_8;
	if(fmts & AFMT_U16_NE)
		ret |= AUDIO_FORMAT_UNSIGNED_16;
	if(fmts & AFMT_A_LAW)
		ret |= AUDIO_FORMAT_ALAW_8;
#endif

	return fmt;
}

static int write_oss(audio_output_t *ao,unsigned char *buf,int len)
{
	return write(ao->fn,buf,len);
}

static int close_oss(audio_output_t *ao)
{
	close(ao->fn);
	return 0;
}

static void flush_oss(audio_output_t *ao)
{
}




static int init_oss(audio_output_t* ao)
{
	if (ao==NULL) return -1;

	/* Set callbacks */
	ao->open = open_oss;
	ao->flush = flush_oss;
	ao->write = write_oss;
	ao->get_formats = get_formats_oss;
	ao->close = close_oss;
	
	/* Success */
	return 0;
}



/* 
	Module information data structure
*/
mpg123_module_t mpg123_output_module_info = {
	/* api_version */	MPG123_MODULE_API_VERSION,
	/* name */			"oss",
	/* description */	"Output audio using OSS",
	/* revision */		"$Rev$",
	/* handle */		NULL,
	
	/* init_output */	init_oss,
};


