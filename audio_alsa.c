/*
 * libalsa output driver. This file is part of Shairport.
 * Copyright (c) Muffinman, Skaman 2013
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#define ALSA_PCM_NEW_HW_PARAMS_API

#include <stdio.h>
#include <unistd.h>
#include <memory.h>
#include <math.h>
#include <pthread.h>
#include <alsa/asoundlib.h>
#include "common.h"
#include "audio.h"

static void help(void);
static int init(int argc, char **argv);
static void deinit(void);
static void start(int sample_rate);
static void play(int buf[], int samples);
static void stop(void);
static void flush(void);
int delay(long* the_delay);
static void volume(double vol);
static void linear_volume(double vol);
static void parameters(audio_parameters *info);
static void mute(int do_mute);
static double set_volume;
static int output_method_signalled = 0;

audio_output audio_alsa = {
    .name = "alsa",
    .help = &help,
    .init = &init,
    .deinit = &deinit,
    .start = &start,
    .stop = &stop,
    .flush = &flush,
    .delay = &delay,
    .play = &play,
    .mute = &mute,
    .volume = &volume,
    .parameters = &parameters
};

static pthread_mutex_t alsa_mutex = PTHREAD_MUTEX_INITIALIZER;

static unsigned int desired_sample_rate;

static snd_pcm_t *alsa_handle = NULL;
static snd_pcm_hw_params_t *alsa_params = NULL;
static snd_ctl_t *ctl = NULL;
static snd_ctl_elem_id_t *elem_id = NULL;
static snd_mixer_t *alsa_mix_handle = NULL;
static snd_mixer_elem_t *alsa_mix_elem = NULL;
static snd_mixer_selem_id_t *alsa_mix_sid = NULL;
static long alsa_mix_minv, alsa_mix_maxv;
static long alsa_mix_mindb, alsa_mix_maxdb;

static char *alsa_out_dev = "default";
static char *alsa_mix_dev = NULL;
static char *alsa_mix_ctrl = "Master";
static int alsa_mix_index = 0;
static int hardware_mixer = 0;
static int has_softvol = 0;

static snd_pcm_sframes_t (*alsa_pcm_write)(snd_pcm_t *, const void *, snd_pcm_uframes_t) = snd_pcm_writei;

static int play_number;
static int64_t accumulated_delay, accumulated_da_delay;
int alsa_characteristics_already_listed = 0;

static snd_pcm_uframes_t period_size_requested,buffer_size_requested;
static int 	set_period_size_request,set_buffer_size_request;


static void help(void) {
  printf("    -d output-device    set the output device [default*|...]\n"
         "    -m mixer-device     set the mixer device ['output-device'*|...]\n"
         "    -c mixer-control    set the mixer control [Master*|...]\n"
         "    -i mixer-index      set the mixer index [0*|...]\n"
         "    *) default option\n");
}

void open_mixer() {
  if (hardware_mixer) {
    debug(2, "Open Mixer");
    int ret = 0;
    snd_mixer_selem_id_alloca(&alsa_mix_sid);
    snd_mixer_selem_id_set_index(alsa_mix_sid, alsa_mix_index);
    snd_mixer_selem_id_set_name(alsa_mix_sid, alsa_mix_ctrl);

    if ((snd_mixer_open(&alsa_mix_handle, 0)) < 0)
      die("Failed to open mixer");
    debug(3, "Mixer device name is \"%s\".", alsa_mix_dev);
    if ((snd_mixer_attach(alsa_mix_handle, alsa_mix_dev)) < 0)
      die("Failed to attach mixer");
    if ((snd_mixer_selem_register(alsa_mix_handle, NULL, NULL)) < 0)
      die("Failed to register mixer element");

    ret = snd_mixer_load(alsa_mix_handle);
    if (ret < 0)
      die("Failed to load mixer element");
    debug(3, "Mixer Control name is \"%s\".", alsa_mix_ctrl);
    alsa_mix_elem = snd_mixer_find_selem(alsa_mix_handle, alsa_mix_sid);
    if (!alsa_mix_elem)
      die("Failed to find mixer element");
  }
}

static int init(int argc, char **argv) {
  pthread_mutex_lock(&alsa_mutex);
  // debug(2,"audio_alsa init called.");
  const char *str;
  int value;

	set_period_size_request = 0;
	set_buffer_size_request = 0;
	
  config.audio_backend_latency_offset = 0; // this is the default for ALSA
  config.audio_backend_buffer_desired_length =
      6615; // default for alsa with a software mixer

  // get settings from settings file first, allow them to be overridden by
  // command line options

  if (config.cfg != NULL) {
    /* Get the desired buffer size setting. */
    if (config_lookup_int(config.cfg,
                          "alsa.audio_backend_buffer_desired_length", &value)) {
      if ((value < 0) || (value > 66150))
        die("Invalid alsa audio backend buffer desired length \"%d\". It "
            "should be between 0 and "
            "66150, default is 6615",
            value);
      else {
        config.audio_backend_buffer_desired_length = value;
      }
    }

    /* Get the latency offset. */
    if (config_lookup_int(config.cfg, "alsa.audio_backend_latency_offset",
                          &value)) {
      if ((value < -66150) || (value > 66150))
        die("Invalid alsa audio backend buffer latency offset \"%d\". It "
            "should be between -66150 and +66150, default is 0",
            value);
      else
        config.audio_backend_latency_offset = value;
    }

    /* Get the Output Device Name. */
    if (config_lookup_string(config.cfg, "alsa.output_device", &str)) {
      alsa_out_dev = (char *)str;
    }

    /* Get the Mixer Type setting. */

    if (config_lookup_string(config.cfg, "alsa.mixer_type", &str)) {
      inform("The alsa mixer_type setting is deprecated and has been ignored. "
             "FYI, using the \"mixer_control_name\" setting automatically "
             "chooses a hardware mixer.");
    }

    /* Get the Mixer Device Name. */
    if (config_lookup_string(config.cfg, "alsa.mixer_device", &str)) {
      alsa_mix_dev = (char *)str;
    }

    /* Get the Mixer Control Name. */
    if (config_lookup_string(config.cfg, "alsa.mixer_control_name", &str)) {
      alsa_mix_ctrl = (char *)str;
      hardware_mixer = 1;
    }
  
    /* Get the disable_synchronization setting. */
    if (config_lookup_string(config.cfg, "alsa.disable_synchronization", &str)) {
      if (strcasecmp(str, "no") == 0)
        config.no_sync = 0;
      else if (strcasecmp(str, "yes") == 0)
        config.no_sync = 1;
      else
        die("Invalid disable_synchronization option choice \"%s\". It should be \"yes\" or \"no\"");
    }
    
    /* Get the use_mmap_if_available setting. */
    if (config_lookup_string(config.cfg, "alsa.use_mmap_if_available", &str)) {
      if (strcasecmp(str, "no") == 0)
        config.no_mmap = 1;
      else if (strcasecmp(str, "yes") == 0)
        config.no_mmap = 0;
      else
        die("Invalid use_mmap_if_available option choice \"%s\". It should be \"yes\" or \"no\"");
    }
    /* Get the optional period size value */
    if (config_lookup_int(config.cfg, "alsa.period_size",
                          &value)) {
      set_period_size_request = 1;
      debug(1,"Value read for period size is %d.",value);
      if (value < 0)
        die("Invalid alsa period size setting \"%d\". It "
            "must be greater than 0.",
            value);
      else
        period_size_requested = value;
    }

    /* Get the optional buffer size value */
    if (config_lookup_int(config.cfg, "alsa.buffer_size",
                          &value)) {
      set_buffer_size_request = 1;
      debug(1,"Value read for buffer size is %d.",value);
      if (value < 0)
        die("Invalid alsa buffer size setting \"%d\". It "
            "must be greater than 0.",
            value);
      else
        buffer_size_requested = value;
    }
  }
  


  optind = 1; // optind=0 is equivalent to optind=1 plus special behaviour
  argv--;     // so we shift the arguments to satisfy getopt()
  argc++;
  // some platforms apparently require optreset = 1; - which?
  int opt;
  while ((opt = getopt(argc, argv, "d:t:m:c:i:")) > 0) {
    switch (opt) {
    case 'd':
      alsa_out_dev = optarg;
      break;

    case 't':
      inform("The alsa backend -t option is deprecated and has been ignored. "
             "FYI, using the -c option automatically chooses a hardware "
             "mixer.");
      break;

    case 'm':
      alsa_mix_dev = optarg;
      break;
    case 'c':
      alsa_mix_ctrl = optarg;
      hardware_mixer = 1;
      break;
    case 'i':
      alsa_mix_index = strtol(optarg, NULL, 10);
      break;
    default:
      help();
      die("Invalid audio option -%c specified", opt);
    }
  }

  if (optind < argc)
    die("Invalid audio argument: %s", argv[optind]);

  debug(1, "Output device name is \"%s\".", alsa_out_dev);

  if (hardware_mixer) {
  
    if (alsa_mix_dev == NULL)
      alsa_mix_dev = alsa_out_dev;

    // Open mixer

    open_mixer();

    if (snd_mixer_selem_get_playback_volume_range(alsa_mix_elem, &alsa_mix_minv,
                                                  &alsa_mix_maxv) < 0)
      debug(1, "Can't read mixer's [linear] min and max volumes.");
    else {
      if (snd_mixer_selem_get_playback_dB_range(alsa_mix_elem, &alsa_mix_mindb,
                                                &alsa_mix_maxdb) == 0) {

        audio_alsa.volume =
            &volume; // insert the volume function now we know it can do dB stuff
        audio_alsa.parameters = &parameters; // likewise the parameters stuff
        if (alsa_mix_mindb == SND_CTL_TLV_DB_GAIN_MUTE) {
          // Raspberry Pi does this
          debug(1, "Lowest dB value is a mute -- try minimum volume +1");
          if (snd_mixer_selem_ask_playback_vol_dB(
                  alsa_mix_elem, alsa_mix_minv + 1, &alsa_mix_mindb) != 0)
            debug(1, "Can't get dB value corresponding to a minimum volume + 1.");
        }
        debug(1, "Hardware mixer has dB volume from %f to %f.",
              (1.0 * alsa_mix_mindb) / 100.0, (1.0 * alsa_mix_maxdb) / 100.0);
      } else {
        // use the linear scale and do the db conversion ourselves
        debug(1, "note: the hardware mixer specified -- \"%s\" -- does not have "
                 "a dB volume scale, so it can't be used. Trying software "
                 "volume control.",
              alsa_mix_ctrl);

        if (snd_ctl_open(&ctl, alsa_mix_dev, 0) < 0)
          die("Cannot open control \"%s\"", alsa_mix_dev);
        if (snd_ctl_elem_id_malloc(&elem_id) < 0)
          die("Cannot allocate memory for control \"%s\"", alsa_mix_dev);
        snd_ctl_elem_id_set_interface(elem_id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(elem_id, alsa_mix_ctrl);

        if (snd_ctl_get_dB_range(ctl, elem_id, &alsa_mix_mindb,
                                               &alsa_mix_maxdb) == 0) {
          debug(1, "Volume control \"%s\" has dB volume from %f to %f.",
                    alsa_mix_ctrl,
                    (1.0 * alsa_mix_mindb) / 100.0,
                    (1.0 * alsa_mix_maxdb) / 100.0);
          has_softvol = 1;
        } else {
          debug(1, "Cannot get the dB range from the volume control \"%s\"",
                    alsa_mix_ctrl);
        }

        /*
        debug(1, "Min and max volumes are %d and
        %d.",alsa_mix_minv,alsa_mix_maxv);
        alsa_mix_maxdb = 0;
        if ((alsa_mix_maxv!=0) && (alsa_mix_minv!=0))
          alsa_mix_mindb =
        -20*100*(log10(alsa_mix_maxv*1.0)-log10(alsa_mix_minv*1.0));
        else if (alsa_mix_maxv!=0)
          alsa_mix_mindb = -20*100*log10(alsa_mix_maxv*1.0);
        audio_alsa.volume = &linear_volume; // insert the linear volume function
        audio_alsa.parameters = &parameters; // likewise the parameters stuff
        debug(1,"Max and min dB calculated are %d and
        %d.",alsa_mix_maxdb,alsa_mix_mindb);
        */
      }
    }
    if (snd_mixer_selem_has_playback_switch(alsa_mix_elem)) {
      audio_alsa.mute =
          &mute; // insert the mute function now we know it can do muting stuff
      debug(1, "Has mute ability.");
    }

    snd_mixer_close(alsa_mix_handle);
  }
  
  alsa_mix_handle = NULL;
  pthread_mutex_unlock(&alsa_mutex);
  return 0;
}

static void deinit(void) {
  // debug(2,"audio_alsa deinit called.");
  stop();
  if (hardware_mixer && alsa_mix_handle) {
    snd_mixer_close(alsa_mix_handle);
  }
}

int open_alsa_device(void) {
  
  const snd_pcm_uframes_t minimal_buffer_headroom =
      352 * 2; // we accept this much headroom in the hardware buffer, but we'll
               // accept less
  const snd_pcm_uframes_t requested_buffer_headroom =
      minimal_buffer_headroom + 2048; // we ask for this much headroom in the
                                      // hardware buffer, but we'll accept less
  int ret, dir = 0;
  unsigned int my_sample_rate = desired_sample_rate;
  // snd_pcm_uframes_t frames = 441 * 10;
  snd_pcm_uframes_t buffer_size, actual_buffer_length;
  snd_pcm_access_t access;
  
  // ensure no calls are made to the alsa device enquiring about the buffer length if synchronisation is disabled.
  if (config.no_sync!=0)
  	audio_alsa.delay = NULL;

  ret = snd_pcm_open(&alsa_handle, alsa_out_dev, SND_PCM_STREAM_PLAYBACK, 0);
  if (ret < 0)
    return (ret);
  // die("Alsa initialization failed: unable to open pcm device: %s.",
  // snd_strerror(ret));

  snd_pcm_hw_params_alloca(&alsa_params);

  ret = snd_pcm_hw_params_any(alsa_handle, alsa_params);
  if (ret < 0) {
    die("audio_alsa: Broken configuration for device \"%s\": no configurations "
        "available",
        alsa_out_dev);
  }

  if ((config.no_mmap == 0) && (snd_pcm_hw_params_set_access(alsa_handle, alsa_params, SND_PCM_ACCESS_MMAP_INTERLEAVED) >= 0)) {
  	if (output_method_signalled==0) {
  		debug(1,"Output written using MMAP");
  		output_method_signalled=1;
  	}
    access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    alsa_pcm_write = snd_pcm_mmap_writei;
  } else {
  	if (output_method_signalled==0) {
  		debug(1,"Output written with RW");
  		output_method_signalled=1;
  	}
    access = SND_PCM_ACCESS_RW_INTERLEAVED;
    alsa_pcm_write = snd_pcm_writei;
  }

  ret = snd_pcm_hw_params_set_access(alsa_handle, alsa_params, access);
  if (ret < 0) {
    die("audio_alsa: Access type not available for device \"%s\": %s",
        alsa_out_dev, snd_strerror(ret));
  }

  switch (config.format) {
  case 24:
    ret = snd_pcm_hw_params_set_format(alsa_handle, alsa_params, SND_PCM_FORMAT_S24);
    break;
  case 32:
    ret = snd_pcm_hw_params_set_format(alsa_handle, alsa_params, SND_PCM_FORMAT_S32);
    break;
  default:
    ret = snd_pcm_hw_params_set_format(alsa_handle, alsa_params, SND_PCM_FORMAT_S16);
  }

  if (ret < 0) {
    die("audio_alsa: Sample format not available for device \"%s\": %s",
        alsa_out_dev, snd_strerror(ret));
  }

  ret = snd_pcm_hw_params_set_channels(alsa_handle, alsa_params, 2);
  if (ret < 0) {
    die("audio_alsa: Channels count (2) not available for device \"%s\": %s",
        alsa_out_dev, snd_strerror(ret));
  }

  ret = snd_pcm_hw_params_set_rate_near(alsa_handle, alsa_params,
                                        &my_sample_rate, &dir);
  if (ret < 0) {
    die("audio_alsa: Rate %iHz not available for playback: %s",
        desired_sample_rate, snd_strerror(ret));
  }

	if (set_period_size_request!=0) {
		debug(1,"Attempting to set the period size");
		ret = snd_pcm_hw_params_set_period_size_near(alsa_handle, alsa_params, &period_size_requested, &dir);
		if (ret < 0) {
			die("audio_alsa: cannot set period size of %lu: %s",
					period_size_requested, snd_strerror(ret));
		snd_pcm_uframes_t actual_period_size;
		snd_pcm_hw_params_get_period_size(alsa_params, &actual_period_size, &dir);
		if (actual_period_size!=period_size_requested)
			inform("Actual period size set to a different value than requested. Requested: %lu, actual setting: %lu",period_size_requested,actual_period_size);
		}
  }

	if (set_buffer_size_request!=0) {
		debug(1,"Attempting to set the buffer size to %lu",buffer_size_requested);
		ret = snd_pcm_hw_params_set_buffer_size_near(alsa_handle, alsa_params, &buffer_size_requested);
		if (ret < 0) {
			die("audio_alsa: cannot set buffer size of %lu: %s",
					buffer_size_requested, snd_strerror(ret));
		snd_pcm_uframes_t actual_buffer_size;
		snd_pcm_hw_params_get_buffer_size(alsa_params, &actual_buffer_size);
		if (actual_buffer_size!=buffer_size_requested)
			inform("Actual period size set to a different value than requested. Requested: %lu, actual setting: %lu",buffer_size,actual_buffer_size);
		}
  }

  ret = snd_pcm_hw_params(alsa_handle, alsa_params);
  if (ret < 0) {
    die("audio_alsa: Unable to set hw parameters for device \"%s\": %s.",
        alsa_out_dev, snd_strerror(ret));
  }

  if (my_sample_rate != desired_sample_rate) {
    die("Can't set the D/A converter to %d.", desired_sample_rate);
  }

  ret = snd_pcm_hw_params_get_buffer_size(alsa_params, &actual_buffer_length);
  if (ret < 0) {
    die("audio_alsa: Unable to get hw buffer length for device \"%s\": %s.",
        alsa_out_dev, snd_strerror(ret));
  }

  if (actual_buffer_length <
      config.audio_backend_buffer_desired_length + minimal_buffer_headroom) {
    /*
    // the dac buffer is too small, so let's try to set it
    buffer_size =
        config.audio_backend_buffer_desired_length + requested_buffer_headroom;
    ret = snd_pcm_hw_params_set_buffer_size_near(alsa_handle, alsa_params,
                                                 &buffer_size);
    if (ret < 0)
      die("audio_alsa: Unable to set hw buffer size to %lu for device \"%s\": "
          "%s.",
          config.audio_backend_buffer_desired_length +
              requested_buffer_headroom,
          alsa_out_dev, snd_strerror(ret));
    if (config.audio_backend_buffer_desired_length + minimal_buffer_headroom >
        buffer_size) {
      die("audio_alsa: Can't set hw buffer size to %lu or more for device "
          "\"%s\". Requested size: %lu, granted size: %lu.",
          config.audio_backend_buffer_desired_length + minimal_buffer_headroom,
          alsa_out_dev, config.audio_backend_buffer_desired_length +
                            requested_buffer_headroom,
          buffer_size);
    }
    */
    debug(1,"The alsa buffer is to small (%lu bytes) to accommodate the desired backend buffer length (%ld) you have chosen.",actual_buffer_length,config.audio_backend_buffer_desired_length);
  }
  
  if (alsa_characteristics_already_listed==0) {
  		alsa_characteristics_already_listed=1;
  		int log_level = 2; // the level at which debug information should be output
  		int rc;
  		snd_pcm_access_t access_type;
  		snd_pcm_format_t format_type;
  		snd_pcm_subformat_t subformat_type;
			unsigned int val, val2;
			unsigned int uval, uval2;
			int sval;
			int dir;
			snd_pcm_uframes_t frames;

			debug(log_level,"PCM handle name = '%s'",
						 snd_pcm_name(alsa_handle));
			
//			ret = snd_pcm_hw_params_any(alsa_handle, alsa_params);
//			if (ret < 0) {
//				die("audio_alsa: Cannpot get configuration for device \"%s\": no configurations "
//						"available",
//						alsa_out_dev);
//			}

			debug(log_level,"alsa device parameters:");

			snd_pcm_hw_params_get_access(alsa_params,&access_type);			
			debug(log_level,"  access type = %s", snd_pcm_access_name(access_type));

			snd_pcm_hw_params_get_format(alsa_params,&format_type);
			debug(log_level,"  format = '%s' (%s)",snd_pcm_format_name(format_type),snd_pcm_format_description(format_type));

			snd_pcm_hw_params_get_subformat(alsa_params,&subformat_type);
			debug(log_level,"  subformat = '%s' (%s)",snd_pcm_subformat_name(subformat_type),snd_pcm_subformat_description(subformat_type));

			snd_pcm_hw_params_get_channels(alsa_params, &uval);
			debug(log_level,"  number of channels = %u", uval);

			sval = snd_pcm_hw_params_get_sbits(alsa_params);
			debug(log_level,"  number of significant bits = %d", sval);
			
			snd_pcm_hw_params_get_rate(alsa_params, &uval, &dir);
 			switch (dir) {
			  case -1:
			    debug(log_level,"  rate = %u frames per second (<).", uval);
			    break;
			  case 0:
			    debug(log_level,"  rate = %u frames per second (precisely).", uval);
			    break;
			  case 1:
			    debug(log_level,"  rate = %u frames per second (>).", uval);
			    break;
			}

			if (snd_pcm_hw_params_get_rate_numden(alsa_params,&uval, &uval2)==0)
				debug(log_level,"  precise (rational) rate = %.3f frames per second (i.e. %u/%u).", uval, uval2, ((double)uval)/uval2);
			else
				debug(log_level,"  precise (rational) rate information unavailable.");

			snd_pcm_hw_params_get_period_time(alsa_params,&uval, &dir);			
			switch (dir) {
			  case -1:
			    debug(log_level,"  period_time = %u us (<).", uval);
			    break;
			  case 0:
			    debug(log_level,"  period_time = %u us (precisely).", uval);
			    break;
			  case 1:
			    debug(log_level,"  period_time = %u us (>).", uval);
			    break;
			}

			snd_pcm_hw_params_get_period_size(alsa_params,&frames, &dir);
			switch (dir) {
			  case -1:
			    debug(log_level,"  period_size = %lu frames (<).", frames);
			    break;
			  case 0:
			    debug(log_level,"  period_size = %lu frames (precisely).", frames);
			    break;
			  case 1:
			    debug(log_level,"  period_size = %lu frames (>).", frames);
			    break;
			}

			snd_pcm_hw_params_get_buffer_time(alsa_params,&uval, &dir);
			switch (dir) {
			  case -1:
			    debug(log_level,"  buffer_time = %u us (<).", uval);
			    break;
			  case 0:
			    debug(log_level,"  buffer_time = %u us (precisely).", uval);
			    break;
			  case 1:
			    debug(log_level,"  buffer_time = %u us (>).", uval);
			    break;
			}

			snd_pcm_hw_params_get_buffer_size(alsa_params,&frames);
			switch (dir) {
			  case -1:
			    debug(log_level,"  buffer_size = %lu frames (<).", frames);
			    break;
			  case 0:
			    debug(log_level,"  buffer_size = %lu frames (precisely).", frames);
			    break;
			  case 1:
			    debug(log_level,"  buffer_size = %lu frames (>).", frames);
			    break;
			}
			
			snd_pcm_hw_params_get_periods(alsa_params, &uval, &dir);
			switch (dir) {
			  case -1:
			    debug(log_level,"  periods_per_buffer = %u (<).", uval);
			    break;
			  case 0:
			    debug(log_level,"  periods_per_buffer = %u (precisely).", uval);
			    break;
			  case 1:
			    debug(log_level,"  periods_per_buffer = %u (>).", uval);
			    break;
			}
		}
  
  return (0);
}

static void start(int sample_rate) {
  // debug(2,"audio_alsa start called.");
  if (sample_rate != 44100)
    die("Unexpected sample rate %d -- only 44,100 supported!", sample_rate);
  desired_sample_rate = sample_rate; // must be a variable
}

int delay(long* the_delay) {
  //snd_pcm_sframes_t is a signed long -- hence the return of a "long"
	int reply;
  // debug(3,"audio_alsa delay called.");
  if (alsa_handle == NULL) {
    return -ENODEV;
  } else {
    pthread_mutex_lock(&alsa_mutex);
    int derr, ignore;
    if (snd_pcm_state(alsa_handle) == SND_PCM_STATE_RUNNING) {
      *the_delay = 0; // just to see what happens
      reply = snd_pcm_delay(alsa_handle, the_delay);
      if (reply != 0) {
        debug(1, "Error %d in delay(): \"%s\". Delay reported is %d frames.", reply,
              snd_strerror(reply), *the_delay);
        ignore = snd_pcm_recover(alsa_handle, reply, 1);
      }
    } else if (snd_pcm_state(alsa_handle) == SND_PCM_STATE_PREPARED) {
      *the_delay = 0;
      reply = 0; // no error
    } else {
      if (snd_pcm_state(alsa_handle) == SND_PCM_STATE_XRUN) {
				*the_delay = 0;
      	reply = 0; // no error
      } else {
        reply = -EIO;
        debug(1, "Error -- ALSA delay(): bad state: %d.",
              snd_pcm_state(alsa_handle));
      }
      if ((derr = snd_pcm_prepare(alsa_handle))) {
        ignore = snd_pcm_recover(alsa_handle, derr, 1);
        debug(1, "Error preparing after delay error: \"%s\".", snd_strerror(derr));
      }
    }
    pthread_mutex_unlock(&alsa_mutex);
    // here, occasionally pretend there's a problem with pcm_get_delay()
		//if ((random() % 100000) < 3) // keep it pretty rare
		//	reply = -EPERM; // pretend something bad has happened
    return reply;
  }
}

static void play(int buf[], int samples) {
  // debug(3,"audio_alsa play called.");
  int ret = 0;
  if (alsa_handle == NULL) {
    pthread_mutex_lock(&alsa_mutex);
    ret = open_alsa_device();
    if (hardware_mixer)
    	open_mixer();
    pthread_mutex_unlock(&alsa_mutex);
    if ((hardware_mixer) && (ret == 0) && (audio_alsa.volume))
      audio_alsa.volume(set_volume);
  }
  if (ret == 0) {
    pthread_mutex_lock(&alsa_mutex);
    snd_pcm_sframes_t current_delay = 0;
    int err, ignore;
    if ((snd_pcm_state(alsa_handle) == SND_PCM_STATE_PREPARED) ||
        (snd_pcm_state(alsa_handle) == SND_PCM_STATE_RUNNING)) {
      if (buf==NULL)
      	debug(1,"NULL buffer passed to pcm_writei -- skipping it");
      if (samples==0)
      	debug(1,"empty buffer being passed to pcm_writei -- skipping it");
      if ((samples!=0) && (buf!=NULL)) {
				err = alsa_pcm_write(alsa_handle, (char *)buf, samples);
				if (err < 0) {
					debug(1, "Error %d writing %d samples in play(): \"%s\".", err, samples,
								snd_strerror(err));
					ignore = snd_pcm_recover(alsa_handle, err, 1);
				}
      }
    } else {
      debug(1, "Error -- ALSA device in incorrect state (%d) for play.",
            snd_pcm_state(alsa_handle));
      if ((err = snd_pcm_prepare(alsa_handle))) {
        ignore = snd_pcm_recover(alsa_handle, err, 1);
        debug(1, "Error preparing after play error: \"%s\".", snd_strerror(err));
      }
    }
    pthread_mutex_unlock(&alsa_mutex);
  }
}

static void flush(void) {
  // debug(2,"audio_alsa flush called.");
  pthread_mutex_lock(&alsa_mutex);
  int derr;
  if (hardware_mixer && alsa_mix_handle) {
    snd_mixer_close(alsa_mix_handle);
    alsa_mix_handle = NULL;
  }
  if (alsa_handle) {
    // debug(1,"Dropping frames for flush...");
    if ((derr = snd_pcm_drop(alsa_handle)))
      debug(1, "Error dropping frames: \"%s\".", snd_strerror(derr));
    // debug(1,"Dropped frames ok. State is %d.",snd_pcm_state(alsa_handle));
    if ((derr = snd_pcm_prepare(alsa_handle)))
      debug(1, "Error preparing after flush: \"%s\".", snd_strerror(derr));
    // debug(1,"Frames successfully dropped.");
    /*
    if (snd_pcm_state(alsa_handle)==SND_PCM_STATE_PREPARED)
      debug(1,"Flush returns to SND_PCM_STATE_PREPARED state.");
    if (snd_pcm_state(alsa_handle)==SND_PCM_STATE_RUNNING)
      debug(1,"Flush returns to SND_PCM_STATE_RUNNING state.");
    */
    if (!((snd_pcm_state(alsa_handle) == SND_PCM_STATE_PREPARED) ||
          (snd_pcm_state(alsa_handle) == SND_PCM_STATE_RUNNING)))
      debug(1, "Flush returning unexpected state -- %d.",
            snd_pcm_state(alsa_handle));

    // flush also closes the device
    snd_pcm_close(alsa_handle);
    alsa_handle = NULL;
  }
  pthread_mutex_unlock(&alsa_mutex);
}

static void stop(void) {
  // debug(2,"audio_alsa stop called.");
  // when we want to stop, we want the alsa device
  // to be closed immediately -- we may even be killing the thread, so we
  // don't wish to wait
  // so we should flush first
  flush(); // flush will also close the device
           // close_alsa_device();
}

static void parameters(audio_parameters *info) {
  info->minimum_volume_dB = alsa_mix_mindb;
  info->maximum_volume_dB = alsa_mix_maxdb;
}

static void volume(double vol) {
  pthread_mutex_lock(&alsa_mutex);
	debug(2, "Setting volume db to %f.", vol);
  set_volume = vol;
  if (hardware_mixer && alsa_mix_handle) {
    if (has_softvol) {
      if (ctl && elem_id) {
        snd_ctl_elem_value_t *value;
        long raw;

        if (snd_ctl_convert_from_dB(ctl, elem_id, (long) vol, &raw, 0) < 0)
        debug(1, "Failed converting dB gain to raw volume value for the "
                 "software volume control.");

        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, elem_id);
        snd_ctl_elem_value_set_integer(value, 0, raw);
        snd_ctl_elem_value_set_integer(value, 1, raw);
        if (snd_ctl_elem_write(ctl, value) < 0)
        debug(1, "Failed to set playback dB volume for the software volume "
                 "control.");
      }
    } else {
      if (snd_mixer_selem_set_playback_dB_all(alsa_mix_elem, vol, 0) != 0) {
        debug(1, "Can't set playback volume accurately to %f dB.", vol);
        if (snd_mixer_selem_set_playback_dB_all(alsa_mix_elem, vol, -1) != 0)
          if (snd_mixer_selem_set_playback_dB_all(alsa_mix_elem, vol, 1) != 0)
            die("Failed to set playback dB volume");
      }
    }
  }
  pthread_mutex_unlock(&alsa_mutex);
}

static void linear_volume(double vol) {
  debug(2, "Setting linear volume to %f.", vol);
  set_volume = vol;
  if (hardware_mixer && alsa_mix_handle) {
    double linear_volume = pow(10, vol);
    // debug(1,"Linear volume is %f.",linear_volume);
    long int_vol =
        alsa_mix_minv + (alsa_mix_maxv - alsa_mix_minv) * linear_volume;
    // debug(1,"Setting volume to %ld, for volume input of %f.",int_vol,vol);
    if (alsa_mix_handle) {
      if (snd_mixer_selem_set_playback_volume_all(alsa_mix_elem, int_vol) != 0)
        die("Failed to set playback volume");
    }
  }
}

static void mute(int do_mute) {
  pthread_mutex_lock(&alsa_mutex);
  // debug(2,"audio_alsa mute called.");
  if (hardware_mixer && alsa_mix_handle) {
    if (do_mute) {
      // debug(1,"Mute");
      snd_mixer_selem_set_playback_switch_all(alsa_mix_elem, 0);
    } else {
      // debug(1,"Unmute");
      snd_mixer_selem_set_playback_switch_all(alsa_mix_elem, 1);
    }
  }
  pthread_mutex_unlock(&alsa_mutex);
}
