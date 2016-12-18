/*
 * Slave-clocked ALAC stream player. This file is part of Shairport.
 * Copyright (c) James Laird 2011, 2013
 * All rights reserved.
 *
 * Modifications for audio synchronisation
 * and related work, copyright (c) Mike Brady 2014
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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/syslog.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include "config.h"

#ifdef HAVE_LIBPOLARSSL
#include <polarssl/aes.h>
#include <polarssl/havege.h>
#endif

#ifdef HAVE_LIBSSL
#include <openssl/aes.h>
#endif

#ifdef HAVE_LIBSOXR
#include <soxr.h>
#endif

#include "common.h"
#include "player.h"
#include "rtp.h"
#include "rtsp.h"

#include "alac.h"

// parameters from the source
static unsigned char *aesiv;
#ifdef HAVE_LIBSSL
static AES_KEY aes;
#endif
static int sampling_rate, frame_size;
const int b16_byte_ch = 4;	// sizeof(short)*ch
const int b32_byte_ch = 8;	// sizeof(int)*ch

// The maximum frame size change there can be is +/- 1;
static int max_frame_size_change = 1;

#ifdef HAVE_LIBPOLARSSL
static aes_context dctx;
#endif

// static pthread_t player_thread = NULL;
static int please_stop;
static int encrypted;

static int connection_state_to_output;
static alac_file *decoder_info;

// debug variables
static int late_packet_message_sent;
static uint64_t packet_count = 0;
static int32_t last_seqno_read;

// interthread variables
static int fix_volume = 0x10000;
static pthread_mutex_t vol_mutex = PTHREAD_MUTEX_INITIALIZER;

// default buffer size
// needs to be a power of 2 because of the way BUFIDX(seqno) works
#define BUFFER_FRAMES 512
#define MAX_PACKET 2048

// DAC buffer occupancy stuff
#define DAC_BUFFER_QUEUE_MINIMUM_LENGTH 600

typedef struct audio_buffer_entry {	// decoded audio packets
  int ready;
  uint32_t timestamp;
  seq_t sequence_number;
  signed short *data;
  int length;			// the length of the decoded data
} abuf_t;
static abuf_t audio_buffer[BUFFER_FRAMES];
#define BUFIDX(seqno) ((seq_t)(seqno) % BUFFER_FRAMES)

// mutex-protected variables
static seq_t ab_read, ab_write;
static int ab_buffering = 1, ab_synced = 0;
static uint32_t first_packet_timestamp = 0;
static int flush_requested = 0;
static uint32_t flush_rtp_timestamp;
static uint64_t time_of_last_audio_packet;
static int shutdown_requested;

// mutexes and condition variables
static pthread_mutex_t ab_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t flush_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flowcontrol;
static int64_t first_packet_time_to_play, time_since_play_started;// nanoseconds
static audio_parameters audio_information;
static uint64_t missing_packets, late_packets, too_late_packets, resend_requests;

static void
ab_resync(void)
{
  int i;
  for (i = 0; i < BUFFER_FRAMES; i++) {
    audio_buffer[i].ready = 0;
    audio_buffer[i].sequence_number = 0;
  }
  ab_synced = 0;
  last_seqno_read = -1;
  ab_buffering = 1;
}

static inline seq_t
SUCCESSOR(seq_t x)
{
  uint32_t p = x & 0xffff;
  p++;
  return p & 0xffff;
}

static inline seq_t
PREDECESSOR(seq_t x)
{
  uint32_t p = (x & 0xffff) + 0x10000;
  p--;
  return p & 0xffff;
}

static inline int32_t
ORDINATE(seq_t x)
{
  int32_t p = x;		// int32_t from seq_t, i.e. uint16_t, so
  int32_t q = ab_read;		// int32_t from seq_t, i.e.
  int32_t t = (p + 0x10000 - q) & 0xffff;
  if (t >= 32767)
    t -= 65536;
  return t;
}

int32_t
seq_diff(seq_t a, seq_t b)
{
  int32_t diff = ORDINATE(b) - ORDINATE(a);
  return diff;
}

static inline int
seq_order(seq_t a, seq_t b)
{
  int32_t d = ORDINATE(b) - ORDINATE(a);
  return d > 0;
}

static inline seq_t
seq_sum(seq_t a, seq_t b)
{
  uint32_t r = (a + b) & 0xffff;
  return r;
}

static inline int
seq32_order(uint32_t a, uint32_t b)
{
  if (a == b)
    return 0;
  int64_t A = a & 0xffffffff;
  int64_t B = b & 0xffffffff;
  int64_t C = B - A;
  return (C & 0x80000000) == 0;
}

static int
alac_decode(short *dest, int *destlen, uint8_t * buf, int len)
{
  if (len > MAX_PACKET) {
    warn("Incoming audio packet size is too large at %d; it should not exceed %d.", len, MAX_PACKET);
    return -1;
  }
  unsigned char packet[MAX_PACKET];
  int reply = 0;
  int outsize = b16_byte_ch * (*destlen);
  int toutsize = outsize;
  if (encrypted) {
    unsigned char iv[16];
    int aeslen = len & ~0xf;
    memcpy(iv, aesiv, sizeof(iv));
#ifdef HAVE_LIBPOLARSSL
    aes_crypt_cbc(&dctx, AES_DECRYPT, aeslen, iv, buf, packet);
#endif
#ifdef HAVE_LIBSSL
    AES_cbc_encrypt(buf, packet, aeslen, &aes, iv, AES_DECRYPT);
#endif
    memcpy(packet + aeslen, buf + aeslen, len - aeslen);
    alac_decode_frame(decoder_info, packet, dest, &outsize);
  } else {
    alac_decode_frame(decoder_info, buf, dest, &outsize);
  }
  if (outsize > toutsize) {
    debug(2, "Output from alac_decode larger (%d bytes, not frames) than expected (%d bytes) -- truncated, but buffer overflow possible! Encrypted = %d.", outsize, toutsize, encrypted);
    reply = -1;
  }
  *destlen = outsize / b16_byte_ch;
  if ((outsize % b16_byte_ch) != 0)
    debug(1, "Number of audio frames (%d) does not correspond exactly to the number of bytes (%d) and the audio frame size (%d).", *destlen, outsize, b16_byte_ch);
  return reply;
}

static int
init_decoder(int32_t fmtp[12])
{
  alac_file *alac;
  frame_size = fmtp[1];
  sampling_rate = fmtp[11];
  int sample_size = fmtp[3];
  if (sample_size != 16)
    die("only 16-bit samples supported!");
  alac = alac_create(sample_size, 2);
  if (!alac)
    return 1;
  decoder_info = alac;
  alac->setinfo_max_samples_per_frame = frame_size;
  alac->setinfo_7a = fmtp[2];
  alac->setinfo_sample_size = sample_size;
  alac->setinfo_rice_historymult = fmtp[4];
  alac->setinfo_rice_initialhistory = fmtp[5];
  alac->setinfo_rice_kmodifier = fmtp[6];
  alac->setinfo_7f = fmtp[7];
  alac->setinfo_80 = fmtp[8];
  alac->setinfo_82 = fmtp[9];
  alac->setinfo_86 = fmtp[10];
  alac->setinfo_8a_rate = fmtp[11];
  alac_allocate_buffers(alac);
  return 0;
}

static void
free_decoder(void)
{
  alac_free(decoder_info);
}

static void
init_buffer(void)
{
  int i;
  for (i = 0; i < BUFFER_FRAMES; i++)
    audio_buffer[i].data = malloc(b16_byte_ch * (frame_size + max_frame_size_change));
  ab_resync();
}

static void
free_buffer(void)
{
  int i;
  for (i = 0; i < BUFFER_FRAMES; i++)
    free(audio_buffer[i].data);
}

void
player_put_packet(seq_t seqno, uint32_t timestamp, uint8_t * data, int len)
{
  if (packet_count == 0) {
    pthread_mutex_lock(&flush_mutex);
    flush_requested = 0;
    flush_rtp_timestamp = 0;
    pthread_mutex_unlock(&flush_mutex);
  }
  pthread_mutex_lock(&ab_mutex);
  packet_count++;
  time_of_last_audio_packet = get_absolute_time_in_fp();
  if (connection_state_to_output) {
    if ((flush_rtp_timestamp != 0) && ((timestamp == flush_rtp_timestamp) || seq32_order(timestamp, flush_rtp_timestamp))) {
      debug(3, "Dropping flushed packet in player_put_packet, seqno %u, timestamp %u, flushing to " "timestamp: %u.", seqno, timestamp, flush_rtp_timestamp);
    } else {
      if ((flush_rtp_timestamp != 0x0) && (!seq32_order(timestamp, flush_rtp_timestamp)))
	flush_rtp_timestamp = 0x0;
      abuf_t *abuf = 0;
      if (!ab_synced) {
	debug(2, "syncing to seqno %u.", seqno);
	ab_write = seqno;
	ab_read = seqno;
	ab_synced = 1;
      }
      if (ab_write == seqno) {
	abuf = audio_buffer + BUFIDX(seqno);
	ab_write = SUCCESSOR(seqno);
      } else if (seq_order(ab_write, seqno)) {
	int32_t gap = seq_diff(ab_write, seqno);
	if (gap <= 0)
	  debug(1, "Unexpected gap size: %d.", gap);
	int i;
	for (i = 0; i < gap; i++) {
	  abuf = audio_buffer + BUFIDX(seq_sum(ab_write, i));
	  abuf->ready = 0;	// to be sure, to be sure
	  abuf->timestamp = 0;
	  abuf->sequence_number = 0;
	}
	abuf = audio_buffer + BUFIDX(seqno);
	ab_write = SUCCESSOR(seqno);
      } else if (seq_order(ab_read, seqno)) {	// late but not
	late_packets++;
	abuf = audio_buffer + BUFIDX(seqno);
      } else {			// too late.
	too_late_packets++;
      }
      if (abuf) {
	int datalen = frame_size;
	if (alac_decode(abuf->data, &datalen, data, len) == 0) {
	  abuf->ready = 1;
	  abuf->length = datalen;
	  abuf->timestamp = timestamp;
	  abuf->sequence_number = seqno;
	  if (config.playback_mode == ST_mono) {
	    signed short *v = abuf->data;
	    int i;
	    for (i = frame_size; i; i--) {
	      int both = *v + *(v + 1);
	      if (both > INT16_MAX) {
		both = INT16_MAX;
	      } else if (both < INT16_MIN) {
		both = INT16_MIN;
	      }
	      short sboth = (short) both;
	      *v++ = sboth;
	      *v++ = sboth;
	    }
	  }
	} else {
	  debug(1, "Bad audio packet detected and discarded.");
	  abuf->ready = 0;
	  abuf->timestamp = 0;
	  abuf->sequence_number = 0;
	}
      }
    }
    int rc = pthread_cond_signal(&flowcontrol);
    if (rc)
      debug(1, "Error signalling flowcontrol.");
  }
  pthread_mutex_unlock(&ab_mutex);
}

int32_t
rand_in_range(int32_t exclusive_range_limit)
{
  static uint32_t lcg_prev = 12345;
  int64_t sp = lcg_prev;
  int64_t rl = exclusive_range_limit;
  lcg_prev = lcg_prev * 69069 + 3;
  sp = sp * rl;
  return sp >> 32;
}

static inline int
dithered_vol(short sample, int shift) //shift 0:32bit, 8:24bit, 16:16bit
{
  long out = (long) sample * fix_volume;
  if (fix_volume < 0x10000) {
    long tpdf = rand_in_range(65536 + 1) - rand_in_range(65536 + 1);
    if (tpdf >= 0)
      if (LONG_MAX - tpdf >= out)
	out += tpdf;
      else
	out = LONG_MAX;
    else if (LONG_MIN - tpdf <= out)
      out += tpdf;
    else
      out = LONG_MIN;
  }
  return out >> shift;
}

// get the next frame, when available. return 0 if underrun/stream reset.
static abuf_t *
buffer_get_frame(void)
{
  uint64_t local_time_now;
  abuf_t *abuf = 0;
  int i;
  abuf_t *curframe;
  int notified_buffer_empty = 0;
  pthread_mutex_lock(&ab_mutex);
  int wait;
  long dac_delay = 0;
  do {
    local_time_now = get_absolute_time_in_fp();
    if ((time_of_last_audio_packet != 0) && (shutdown_requested == 0) && (config.dont_check_timeout == 0)) {
      uint64_t ct = config.timeout;
      if ((local_time_now > time_of_last_audio_packet) && (local_time_now - time_of_last_audio_packet >= ct << 32)) {
	debug(1, "As Yeats almost said, \"Too long a silence / can make a stone of the heart\"");
	rtsp_request_shutdown_stream();
	shutdown_requested = 1;
      }
    }
    int rco = get_requested_connection_state_to_output();
    if (connection_state_to_output != rco) {
      connection_state_to_output = rco;
      if (connection_state_to_output == 0) {
	pthread_mutex_lock(&flush_mutex);
	flush_requested = 1;
	pthread_mutex_unlock(&flush_mutex);
      }
    }
    pthread_mutex_lock(&flush_mutex);
    if (flush_requested == 1) {
      if (config.output->flush)
	config.output->flush();
      ab_resync();
      first_packet_timestamp = 0;
      first_packet_time_to_play = 0;
      time_since_play_started = 0;
      flush_requested = 0;
    }
    pthread_mutex_unlock(&flush_mutex);
    uint32_t flush_limit = 0;
    if (ab_synced) {
      do {
	curframe = audio_buffer + BUFIDX(ab_read);
	if ((ab_read != ab_write) && (curframe->ready)) {
	  if (curframe->sequence_number != ab_read) {
	    if (BUFIDX(curframe->sequence_number) == BUFIDX(ab_read)) {
	      if (seq_order(ab_read, curframe->sequence_number)) {
		ab_read = curframe->sequence_number;
		debug(1, "Aliasing of buffer index -- reset.");
	      }
	    } else {
	      debug(1, "Inconsistent sequence numbers detected");
	    }
	  }
	  if ((flush_rtp_timestamp != 0) && ((curframe->timestamp == flush_rtp_timestamp) || seq32_order(curframe->timestamp, flush_rtp_timestamp))) {
	    debug(1, "Dropping flushed packet seqno %u, timestamp %u", curframe->sequence_number, curframe->timestamp);
	    curframe->ready = 0;
	    flush_limit++;
	    ab_read = SUCCESSOR(ab_read);
	  }
	  if ((flush_rtp_timestamp != 0) && (!seq32_order(curframe->timestamp, flush_rtp_timestamp)))
	    flush_rtp_timestamp = 0;
	}
      } while ((flush_rtp_timestamp != 0) && (flush_limit <= 8820) && (curframe->ready == 0));
      if (flush_limit == 8820) {
	debug(1, "Flush hit the 8820 frame limit!");
	flush_limit = 0;
      }
      curframe = audio_buffer + BUFIDX(ab_read);
      if (curframe->ready) {
	notified_buffer_empty = 0;
	if (ab_buffering) {
	  int have_sent_prefiller_silence;
	  uint32_t reference_timestamp;
	  uint64_t reference_timestamp_time, remote_reference_timestamp_time;
	  get_reference_timestamp_stuff(&reference_timestamp, &reference_timestamp_time, &remote_reference_timestamp_time);
	  if (first_packet_timestamp == 0) {
	    if (reference_timestamp) {
	      first_packet_timestamp = curframe->timestamp;
	      have_sent_prefiller_silence = 0;
	      int64_t delta = ((int64_t) first_packet_timestamp - (int64_t) reference_timestamp) + config.latency + config.audio_backend_latency_offset;
	      if (delta >= 0) {
		uint64_t delta_fp_sec = (delta << 32) / 44100;
		first_packet_time_to_play = reference_timestamp_time + delta_fp_sec;
	      } else {
		int64_t abs_delta = -delta;
		uint64_t delta_fp_sec = (abs_delta << 32) / 44100;
		first_packet_time_to_play = reference_timestamp_time - delta_fp_sec;
	      }
	      if (local_time_now >= first_packet_time_to_play) {
		debug(1, "First packet is late! It should have played before now. Flushing 0.1 seconds");
		player_flush(first_packet_timestamp + 4410);
	      }
	    }
	  }
	  if (first_packet_time_to_play != 0) {
	    int64_t delta = ((int64_t) first_packet_timestamp - (int64_t) reference_timestamp) + config.latency + config.audio_backend_latency_offset;
	    if (delta >= 0) {
	      uint64_t delta_fp_sec = (delta << 32) / 44100;
	      first_packet_time_to_play = reference_timestamp_time + delta_fp_sec;
	    } else {
	      int64_t abs_delta = -delta;
	      uint64_t delta_fp_sec = (abs_delta << 32) / 44100;
	      first_packet_time_to_play = reference_timestamp_time - delta_fp_sec;
	    }
	    int64_t max_dac_delay = 4410;
	    int64_t filler_size = max_dac_delay;
	    if (local_time_now >= first_packet_time_to_play) {
	      if (config.output->flush)
		config.output->flush();
	      ab_resync();
	      first_packet_timestamp = 0;
	      first_packet_time_to_play = 0;
	      time_since_play_started = 0;
	    } else {
	      if ((config.output->delay) && (have_sent_prefiller_silence != 0)) {
		int resp = config.output->delay(&dac_delay);
		if (resp != 0) {
		  debug(1, "Error %d getting dac_delay in buffer_get_frame.", resp);
		  dac_delay = 0;
		}
	      } else
		dac_delay = 0;
	      int64_t gross_frame_gap = ((first_packet_time_to_play - local_time_now) * 44100) >> 32;
	      int64_t exact_frame_gap = gross_frame_gap - dac_delay;
	      if (exact_frame_gap < 0) {
		if (config.output->flush)
		  config.output->flush();
		ab_resync();
		first_packet_timestamp = 0;
		first_packet_time_to_play = 0;
	      } else {
		int64_t fs = filler_size;
		if (fs > (max_dac_delay - dac_delay))
		  fs = max_dac_delay - dac_delay;
		if (fs < 0) {
		  debug(2, "frame size (fs) < 0 with max_dac_delay of %lld and dac_delay of %ld", max_dac_delay, dac_delay);
		  fs = 0;
		}
		if ((exact_frame_gap <= fs) || (exact_frame_gap <= frame_size << 1)) {
		  fs = exact_frame_gap;
		  ab_buffering = 0;
		}
		signed int *silence;
		if (fs != 0) {
		  silence = malloc(b32_byte_ch * fs);
		  if (silence == NULL)
		    debug(1, "Failed to allocate %d byte silence buffer.", fs);
		  else {
		    memset(silence, 0, b32_byte_ch * fs);
		    config.output->play(silence, fs);
		    free(silence);
		    have_sent_prefiller_silence = 1;
		  }
		}
	      }
	    }
	  }
	  if (ab_buffering == 0) {
	    uint64_t reference_timestamp_time;
	    get_reference_timestamp_stuff(&play_segment_reference_frame, &reference_timestamp_time, &play_segment_reference_frame_remote_time);
#ifdef CONFIG_METADATA
	    send_ssnc_metadata('prsm', NULL, 0, 0);
#endif
	  }
	}
      }
    }
    int do_wait = 0;
    if ((ab_synced) && (curframe) && (curframe->ready) && (curframe->timestamp)) {
      do_wait = 1;
      uint32_t reference_timestamp;
      uint64_t reference_timestamp_time, remote_reference_timestamp_time;
      get_reference_timestamp_stuff(&reference_timestamp, &reference_timestamp_time, &remote_reference_timestamp_time);
      if (reference_timestamp) {
	uint32_t packet_timestamp = curframe->timestamp;
	int64_t delta = (int64_t) packet_timestamp - (int64_t) reference_timestamp;
	int64_t offset = config.latency + config.audio_backend_latency_offset - config.audio_backend_buffer_desired_length;
	int64_t net_offset = delta + offset;
	uint64_t time_to_play = reference_timestamp_time;
	if (net_offset >= 0) {
	  uint64_t net_offset_fp_sec = (net_offset << 32) / 44100;
	  time_to_play += net_offset_fp_sec;
	} else {
	  int64_t abs_net_offset = -net_offset;
	  uint64_t net_offset_fp_sec = (abs_net_offset << 32) / 44100;
	  time_to_play -= net_offset_fp_sec;
	}
	if (local_time_now >= time_to_play) {
	  do_wait = 0;
	}
      }
    }
    if (do_wait == 0)
      if ((ab_synced != 0) && (ab_read == ab_write)) {
	if (notified_buffer_empty == 0) {
	  debug(1, "Buffers exhausted.");
	  notified_buffer_empty = 1;
	}
	do_wait = 1;
      }
    wait = (ab_buffering || (do_wait != 0) || (!ab_synced)) && (!please_stop);
    if (wait) {
      uint64_t time_to_wait_for_wakeup_fp = ((uint64_t) 1 << 32) / 44100;
      time_to_wait_for_wakeup_fp *= 4 * 352;
      time_to_wait_for_wakeup_fp /= 3;
#ifdef COMPILE_FOR_LINUX_AND_FREEBSD_AND_CYGWIN
      uint64_t time_of_wakeup_fp = local_time_now + time_to_wait_for_wakeup_fp;
      uint64_t sec = time_of_wakeup_fp >> 32;
      uint64_t nsec = ((time_of_wakeup_fp & 0xffffffff) * 1000000000) >> 32;
      struct timespec time_of_wakeup;
      time_of_wakeup.tv_sec = sec;
      time_of_wakeup.tv_nsec = nsec;
      pthread_cond_timedwait(&flowcontrol, &ab_mutex, &time_of_wakeup);
#endif
#ifdef COMPILE_FOR_OSX
      uint64_t sec = time_to_wait_for_wakeup_fp >> 32;
      uint64_t nsec = ((time_to_wait_for_wakeup_fp & 0xffffffff) * 1000000000) >> 32;
      struct timespec time_to_wait;
      time_to_wait.tv_sec = sec;
      time_to_wait.tv_nsec = nsec;
      pthread_cond_timedwait_relative_np(&flowcontrol, &ab_mutex, &time_to_wait);
#endif
    }
  } while (wait);
  if (please_stop) {
    pthread_mutex_unlock(&ab_mutex);
    return 0;
  }
  if (!ab_buffering) {
    for (i = 8; i < seq_diff(ab_read, ab_write) >> 1; i <<= 1) {
      seq_t next = seq_sum(ab_read, i);
      abuf = audio_buffer + BUFIDX(next);
      if (!abuf->ready) {
	rtp_request_resend(next, 1);
	resend_requests++;
      }
    }
  }
  if (!curframe->ready) {
    missing_packets++;
    memset(curframe->data, 0, b16_byte_ch * frame_size);
    curframe->timestamp = 0;
  }
  curframe->ready = 0;
  ab_read = SUCCESSOR(ab_read);
  pthread_mutex_unlock(&ab_mutex);
  return curframe;
}

static inline short
shortmean(short a, short b)
{
  long al = (long) a;
  long bl = (long) b;
  long longmean = (al + bl) >> 1;
  short r = (short) longmean;
  if (r != longmean)
    debug(1, "Error calculating average of two shorts");
  return r;
}

static void
stuff_buffer_normal(short *inptr, int length, int *outptr, int shift) //shift 24:8,32:16
{
  int i;
  for (i = 0; i < length << 1; i++)
    *outptr++ = ((long) *inptr++) << shift;
}

// stuff: 1 means add 1; 0 means do nothing; -1 means remove 1
static int
stuff_buffer_basic(short *inptr, int length, int *outptr, int stuff, int shift) //shift 16:16, 24:8, 32:0
{
  if ((stuff > 1) || (stuff < -1) || (length < 100))
    return length;
  int i, stuffsamp = length;
  if (stuff)
    stuffsamp = (rand() % (length - 2)) + 1;
  pthread_mutex_lock(&vol_mutex);
  if (shift == 16)
    for (i = 0; i < stuffsamp; i++) {
      *(short*)outptr = dithered_vol(*inptr++, shift);
      *((short*)outptr + 1) = dithered_vol(*inptr++, shift);
      outptr++;
    }
  else
    for (i = 0; i < stuffsamp << 1; i++)
      *outptr++ = dithered_vol(*inptr++, shift);
  if (stuff) {
    if (stuff == 1)
      if (shift == 16) {
	*(short*)outptr = dithered_vol(shortmean(inptr[-2], inptr[0]), shift);
	*((short*)outptr + 1) = dithered_vol(shortmean(inptr[-1], inptr[1]), shift);
	outptr++;
      } else {
	*outptr++ = dithered_vol(shortmean(inptr[-2], inptr[0]), shift);
	*outptr++ = dithered_vol(shortmean(inptr[-1], inptr[1]), shift);
      }
    else if (stuff == -1)
      inptr += 2;
    int remainder = length;
    if (stuff < 0)
      remainder += stuff;
    if (shift == 16)
      for (; i < remainder; i++) {
        *((short*)outptr) = dithered_vol(*inptr++, shift);
        *((short*)outptr + 1) = dithered_vol(*inptr++, shift);
	outptr++;
      }
    else
      for (; i < remainder << 1; i++)
        *outptr++ = dithered_vol(*inptr++, shift);
  }
  pthread_mutex_unlock(&vol_mutex);
  return length + stuff;
}

#ifdef HAVE_LIBSOXR
const soxr_io_spec_t io_spec = {SOXR_INT16_I,SOXR_INT16_I,1.0,NULL,0};
static short *otptr = NULL;
// stuff: 1 means add 1; 0 means do nothing; -1 means remove 1
static int
stuff_buffer_soxr(short *inptr, int length, int *outptr, int stuff, int shift)
{
  if ((stuff > 1) || (stuff < -1) || (length < 100))
    return length;
  int i;
  short *ip = inptr;
  if (stuff) {
    short *op = otptr;
    size_t odone;
    soxr_oneshot(length, length + stuff, 2, inptr, length, NULL, otptr, length + stuff, &odone, &io_spec, NULL, NULL);
    const int gpm = 5;
    short *ip2 = &inptr[(length - gpm) << 1];
    short *op2 = &otptr[(length + stuff - gpm) << 1];
    for (i = 0; i < gpm << 1; i++) {
      *op++ = *ip++;
      *op2++ = *ip2++;
    }
    ip = otptr;
  }
  if (shift == 16)
    for (i = 0; i < length + stuff; i++) {
      *(short*)outptr = dithered_vol(*ip++, shift);
      *((short*)outptr + 1) = dithered_vol(*ip++, shift);
      outptr++;
    }
  else
    for (i = 0; i < (length + stuff) << 1; i++)
      *outptr++ = dithered_vol(*ip++, shift);
  return length + stuff;
}
#endif

typedef struct stats {
  int64_t sync_error, correction, drift;
} stats_t;

static void *
player_thread_func(void *arg)
{
  struct inter_threads_record itr;
  itr.please_stop = 0;
  pthread_t rtp_audio_thread, rtp_control_thread, rtp_timing_thread;
  pthread_create(&rtp_audio_thread, NULL, &rtp_audio_receiver, (void *) &itr);
  pthread_create(&rtp_control_thread, NULL, &rtp_control_receiver, (void *) &itr);
  pthread_create(&rtp_timing_thread, NULL, &rtp_timing_receiver, (void *) &itr);
  session_corrections = 0;
  play_segment_reference_frame = 0;
  int maximum_latency = config.latency + config.audio_backend_latency_offset;
  if ((maximum_latency + (352 - 1)) / 352 + 10 > BUFFER_FRAMES)
    die("Not enough buffers available for a total latency of %d frames. A maximum of %d 352-frame packets may be accommodated.", maximum_latency, BUFFER_FRAMES);
  connection_state_to_output = get_requested_connection_state_to_output();
#define trend_interval 3758
  stats_t statistics[trend_interval];
  int number_of_statistics, oldest_statistic, newest_statistic;
  int at_least_one_frame_seen = 0;
  int64_t tsum_of_sync_errors, tsum_of_corrections, tsum_of_insertions_and_deletions, tsum_of_drifts;
  int64_t previous_sync_error, previous_correction;
  int64_t minimum_dac_queue_size = INT64_MAX;
  int32_t minimum_buffer_occupancy = INT32_MAX;
  int32_t maximum_buffer_occupancy = INT32_MIN;
  time_t playstart = time(NULL);
  buffer_occupancy = 0;
  int play_samples;
  int64_t current_delay;
  int play_number = 0;
  time_of_last_audio_packet = 0;
  shutdown_requested = 0;
  number_of_statistics = oldest_statistic = newest_statistic = 0;
  tsum_of_sync_errors = tsum_of_corrections = tsum_of_insertions_and_deletions = tsum_of_drifts = 0;
  const int print_interval = trend_interval;
  char rnstate[256];
  initstate(time(NULL), rnstate, 256);
  signed int *outbuf;
  outbuf = malloc(b32_byte_ch * (frame_size + max_frame_size_change));
  if (outbuf == NULL)
    debug(1, "Failed to allocate memory for an output buffer.");
#ifdef HAVE_LIBSOXR
  otptr = malloc(b16_byte_ch * (frame_size + max_frame_size_change));
  if (otptr == NULL)
    debug(1, "Failed to allocate memory for an output buffer(soxr).");
#endif
  late_packet_message_sent = 0;
  first_packet_timestamp = 0;
  missing_packets = late_packets = too_late_packets = resend_requests = 0;
  flush_rtp_timestamp = 0;
  int sync_error_out_of_bounds = 0;
  if (config.statistics_requested)
    if ((config.output->delay))
      if (config.no_sync == 0)
	inform("sync error in frames, " "net correction in ppm, " "corrections in ppm, " "total packets, " "missing packets, " "late packets, " "too late packets, " "resend requests, " "min DAC queue size, " "min buffer occupancy, " "max buffer occupancy");
      else
	inform("sync error in frames, " "total packets, " "missing packets, " "late packets, " "too late packets, " "resend requests, " "min DAC queue size, " "min buffer occupancy, " "max buffer occupancy");
    else
      inform("total packets, " "missing packets, " "late packets, " "too late packets, " "resend requests, " "min buffer occupancy, " "max buffer occupancy");
  while (!please_stop) {
    abuf_t *inframe = buffer_get_frame();
    if (inframe) {
      if (inframe->data) {
	play_number++;
	if (inframe->timestamp == 0) {
	  last_seqno_read = (SUCCESSOR(last_seqno_read) & 0xffff);
	  if (inframe->data == NULL)
	    debug(1, "NULL inframe->data to play -- skipping it.");
	  else if (inframe->length == 0)
	    debug(1, "empty frame to play -- skipping it.");
	  else if (config.format == 16)
	    config.output->play((int*)inframe->data, inframe->length);
	  else {
	    stuff_buffer_normal(inframe->data, inframe->length, outbuf, config.format - 16);
	    config.output->play(outbuf, inframe->length);
	  }
	} else {
	  at_least_one_frame_seen = 1;
	  uint32_t reference_timestamp;
	  uint64_t reference_timestamp_time, remote_reference_timestamp_time;
	  get_reference_timestamp_stuff(&reference_timestamp, &reference_timestamp_time, &remote_reference_timestamp_time);
	  int64_t rt, nt;
	  rt = reference_timestamp;	// uint32_t to int64_t
	  nt = inframe->timestamp;	// uint32_t to int64_t
	  uint64_t local_time_now = get_absolute_time_in_fp();
	  int64_t td = 0;
	  int64_t td_in_frames = 0;
	  if (local_time_now >= reference_timestamp_time) {
	    td = local_time_now - reference_timestamp_time;
	    td_in_frames = (td * 44100) >> 32;
	  } else {
	    td = reference_timestamp_time - local_time_now;
	    td_in_frames = (td * 44100) >> 32;
	    td_in_frames = -td_in_frames;
	    td = -td;
	  }
	  int64_t sync_error = 0;
	  int amount_to_stuff = 0;
	  if (last_seqno_read == -1)
	    last_seqno_read = inframe->sequence_number;
	  else {
	    last_seqno_read = SUCCESSOR(last_seqno_read);
	    if (inframe->sequence_number != last_seqno_read) {
	      debug(1, "Player: packets out of sequence: expected: %u, got: %u, with ab_read: %u and ab_write: %u.", last_seqno_read, inframe->sequence_number, ab_read, ab_write);
	      last_seqno_read = inframe->sequence_number;
	    }
	  }
	  buffer_occupancy = seq_diff(ab_read, ab_write);
	  if (buffer_occupancy < minimum_buffer_occupancy)
	    minimum_buffer_occupancy = buffer_occupancy;
	  if (buffer_occupancy > maximum_buffer_occupancy)
	    maximum_buffer_occupancy = buffer_occupancy;
	  int resp = -1;
	  current_delay = -1;
	  if (config.output->delay) {
	    long l_delay;
	    resp = config.output->delay(&l_delay);
	    current_delay = l_delay;
	    if (resp == 0) {	// no error
	      if (current_delay < 0) {
		debug(1, "Underrun of %d frames reported, but ignored.", current_delay);
		current_delay = 0;
	      }
	      if (current_delay < minimum_dac_queue_size)
		minimum_dac_queue_size = current_delay;
	    } else
	      debug(2, "Delay error %d when checking running latency.", resp);
	  }
	  if (resp >= 0) {
	    int64_t delay = td_in_frames + rt - (nt - current_delay);
	    sync_error = delay - config.latency;
	    if (sync_error > config.tolerance)
	      amount_to_stuff = -1;
	    if (sync_error < -config.tolerance)
	      amount_to_stuff = 1;
	    if (current_delay < DAC_BUFFER_QUEUE_MINIMUM_LENGTH)
	      amount_to_stuff = 0;
	    if (amount_to_stuff && local_time_now && first_packet_time_to_play && local_time_now >= first_packet_time_to_play) {
	      int64_t tp = (local_time_now - first_packet_time_to_play) >> 32;
	      if (tp < 5)
		amount_to_stuff = 0;
	      else if (tp < 30 && (random() % 1000) > 352)
		amount_to_stuff = 0;
	    }
	    if (config.no_sync != 0)
	      amount_to_stuff = 0;
	    if ((amount_to_stuff == 0) && (fix_volume == 0x10000))
	      if (inframe->data == NULL)
		debug(1, "NULL inframe->data to play -- skipping it.");
	      else if (inframe->length == 0)
	        debug(1, "empty frame to play -- skipping it (2).");
	      else if (config.format == 16)
	        config.output->play((int*)inframe->data, inframe->length);
	      else {
		stuff_buffer_normal(inframe->data, inframe->length, outbuf, config.format - 16);
		config.output->play(outbuf, inframe->length);
	      }
	    else {
#ifdef HAVE_LIBSOXR
	      switch (config.packet_stuffing) {
	      case ST_basic:
		play_samples = stuff_buffer_basic(inframe->data, inframe->length, outbuf, amount_to_stuff, 32 - config.format);
		break;
	      case ST_soxr:
		play_samples = stuff_buffer_soxr(inframe->data, inframe->length, outbuf, amount_to_stuff, 32 - config.format);
	      }
#else
	      play_samples = stuff_buffer_basic(inframe->data, inframe->length, outbuf, amount_to_stuff, 32 - config.format);
#endif
	      if (outbuf == NULL)
		debug(1, "NULL outbuf to play -- skipping it.");
	      else if (play_samples == 0)
		debug(1, "play_samples==0 skipping it (1).");
	      else
		config.output->play(outbuf, play_samples);
	    }
	    int64_t abs_sync_error = sync_error;
	    if (abs_sync_error < 0)
	      abs_sync_error = -abs_sync_error;
	    if ((config.no_sync == 0) && (inframe->timestamp != 0) && (!please_stop) && (config.resyncthreshold != 0) && (abs_sync_error > config.resyncthreshold)) {
	      sync_error_out_of_bounds++;
	      if (sync_error_out_of_bounds > 3) {
		debug(1, "Lost sync with source for %d consecutive packets -- flushing and " "resyncing. Error: %lld.", sync_error_out_of_bounds, sync_error);
		sync_error_out_of_bounds = 0;
		player_flush(nt);
	      }
	    } else
	      sync_error_out_of_bounds = 0;
	  } else if (fix_volume == 0x10000)
	    if (inframe->data == NULL)
	      debug(1, "NULL inframe->data to play -- skipping it.");
	    else if (inframe->length == 0)
	      debug(1, "empty frame to play -- skipping it (3).");
	    else if (config.format == 16)
	      config.output->play((int*)inframe->data, inframe->length);
	    else {
	      stuff_buffer_normal(inframe->data, inframe->length, outbuf, config.format - 16);
	      config.output->play(outbuf, inframe->length);
	    }
	  else {
	    play_samples = stuff_buffer_basic(inframe->data, inframe->length, outbuf, 0, 32 - config.format);
	    if (outbuf == NULL)
	      debug(1, "NULL outbuf to play -- skipping it.");
	    else if (inframe->length == 0)
	      debug(1, "empty frame to play -- skipping it (4).");
	    else
	      config.output->play(outbuf, play_samples);
	  }
	  inframe->timestamp = 0;
	  inframe->sequence_number = 0;
	  if (sync_error != -1) {
	    if (number_of_statistics == trend_interval) {
	      tsum_of_sync_errors -= statistics[oldest_statistic].sync_error;
	      tsum_of_drifts -= statistics[oldest_statistic].drift;
	      if (statistics[oldest_statistic].correction > 0)
		tsum_of_insertions_and_deletions -= statistics[oldest_statistic].correction;
	      else
		tsum_of_insertions_and_deletions += statistics[oldest_statistic].correction;
	      tsum_of_corrections -= statistics[oldest_statistic].correction;
	      oldest_statistic = (oldest_statistic + 1) % trend_interval;
	      number_of_statistics--;
	    }
	    statistics[newest_statistic].sync_error = sync_error;
	    statistics[newest_statistic].correction = amount_to_stuff;
	    if (number_of_statistics == 0)
	      statistics[newest_statistic].drift = 0;
	    else
	      statistics[newest_statistic].drift = sync_error - previous_sync_error - previous_correction;
	    previous_sync_error = sync_error;
	    previous_correction = amount_to_stuff;
	    tsum_of_sync_errors += sync_error;
	    tsum_of_drifts += statistics[newest_statistic].drift;
	    if (amount_to_stuff > 0) {
	      tsum_of_insertions_and_deletions += amount_to_stuff;
	    } else {
	      tsum_of_insertions_and_deletions -= amount_to_stuff;
	    }
	    tsum_of_corrections += amount_to_stuff;
	    session_corrections += amount_to_stuff;
	    newest_statistic = (newest_statistic + 1) % trend_interval;
	    number_of_statistics++;
	  }
	}
	if (play_number % print_interval == 0) {
	  double moving_average_sync_error = (1.0 * tsum_of_sync_errors) / number_of_statistics;
	  double moving_average_correction = (1.0 * tsum_of_corrections) / number_of_statistics;
	  double moving_average_insertions_plus_deletions = (1.0 * tsum_of_insertions_and_deletions) / number_of_statistics;
	  if (config.statistics_requested) {
	    if (at_least_one_frame_seen) {
	      if ((config.output->delay)) {
		if (config.no_sync == 0) {
		  inform("%*.1f,"	/* Sync error inf frames */
			 "%*.1f,"	/* net correction in ppm */
			 "%*.1f,"	/* corrections in ppm */
			 "%*d,"	        /* total packets */
			 "%*llu,"	/* missing packets */
			 "%*llu,"	/* late packets */
			 "%*llu,"	/* too late packets */
			 "%*llu,"	/* resend requests */
			 "%*lli,"	/* min DAC queue size */
			 "%*d,"         /* min buffer occupancy */
			 "%*d",	        /* max buffer occupancy */
			 10, moving_average_sync_error,
			 10,
			 moving_average_correction * 1000000 / 352,
			 10,
			 moving_average_insertions_plus_deletions * 1000000 / 352,
			 12,
			 play_number, 7, missing_packets,
			 7, late_packets, 7, too_late_packets, 7, resend_requests, 7, minimum_dac_queue_size, 5, minimum_buffer_occupancy, 5, maximum_buffer_occupancy);
		} else {
		  inform("%*.1f,"	/* Sync error inf frames */
			 "%*d," 	/* total packets */
			 "%*llu,"	/* missing packets */
			 "%*llu,"	/* late packets */
			 "%*llu,"	/* too late packets */
			 "%*llu,"	/* resend requests */
			 "%*lli,"	/* min DAC queue size */
			 "%*d," 	/* min buffer occupancy */
			 "%*d", 	/* max buffer occupancy */
			 10, moving_average_sync_error,
			 12, play_number,
			 7, missing_packets, 7, late_packets, 7, too_late_packets, 7, resend_requests, 7, minimum_dac_queue_size, 5, minimum_buffer_occupancy, 5, maximum_buffer_occupancy);
		}
	      } else {
		inform("%*.1f,"	/* Sync error inf frames */
		       "%*d,"	/* total packets */
		       "%*llu,"	/* missing packets */
		       "%*llu,"	/* late packets */
		       "%*llu,"	/* too late packets */
		       "%*llu,"	/* resend requests */
		       "%*d,"	/* min buffer occupancy */
		       "%*d",	/* max buffer occupancy */
		       10, moving_average_sync_error,
		       12, play_number, 7, missing_packets, 7, late_packets, 7, too_late_packets, 7, resend_requests, 5, minimum_buffer_occupancy, 5, maximum_buffer_occupancy);
	      }
	    } else {
	      inform("No frames received in the last sampling interval.");
	    }
	  }
	  minimum_dac_queue_size = INT64_MAX;
	  maximum_buffer_occupancy = INT32_MIN;
	  minimum_buffer_occupancy = INT32_MAX;
	  at_least_one_frame_seen = 0;
	}
      }
    }
  }
  if (config.statistics_requested) {
    int rawSeconds = (int) difftime(time(NULL), playstart);
    int elapsedHours = rawSeconds / 3600;
    int elapsedMin = (rawSeconds / 60) % 60;
    int elapsedSec = rawSeconds % 60;
    inform("Playback Stopped. Total playing time %02d:%02d:%02d\n", elapsedHours, elapsedMin, elapsedSec);
  }
  if (config.output->stop)
    config.output->stop();
  usleep(100000);
  free(outbuf);
#ifdef HAVE_LIBSOXR
  free(otptr);
#endif
  debug(1, "Shut down audio, control and timing threads");
  itr.please_stop = 1;
  pthread_kill(rtp_audio_thread, SIGUSR1);
  pthread_kill(rtp_control_thread, SIGUSR1);
  pthread_kill(rtp_timing_thread, SIGUSR1);
  pthread_join(rtp_timing_thread, NULL);
  debug(1, "timing thread joined");
  pthread_join(rtp_audio_thread, NULL);
  debug(1, "audio thread joined");
  pthread_join(rtp_control_thread, NULL);
  debug(1, "control thread joined");
  debug(1, "Player thread exit");
  return 0;
}

// takes the volume as specified by the airplay protocol
void
player_volume(double airplay_volume)
{
  int32_t hw_min_db, hw_max_db, hw_range_db, min_db, max_db;
  int32_t sw_min_db = -9630;
  int32_t sw_max_db = 0;
  int32_t sw_range_db = sw_max_db - sw_min_db;
  int32_t desired_range_db;
  if (config.volume_range_db)
    desired_range_db = (int32_t) trunc(config.volume_range_db * 100);
  else
    desired_range_db = 0;
  if (config.output->parameters) {
    config.output->parameters(&audio_information);
    hw_max_db = audio_information.maximum_volume_dB;
    hw_min_db = audio_information.minimum_volume_dB;
    hw_range_db = hw_max_db - hw_min_db;
  } else {
    hw_max_db = hw_min_db = hw_range_db = 0;
  }
  if (desired_range_db) {
    if (hw_range_db) {
      if (hw_range_db >= desired_range_db) {
	max_db = hw_max_db;
	min_db = max_db - desired_range_db;
      } else {
	if ((hw_range_db + sw_range_db) < desired_range_db) {
	  inform("The volume attenuation range %f is greater than can be accommodated by the hardware and software -- set to %f.", config.volume_range_db, hw_range_db + sw_range_db);
	  desired_range_db = hw_range_db + sw_range_db;
	}
	min_db = hw_min_db;
	max_db = min_db + desired_range_db;
      }
    } else {
      if (sw_range_db < desired_range_db) {
	inform("The volume attenuation range %f is greater than can be accommodated by the software -- set to %f.", config.volume_range_db, sw_range_db);
	desired_range_db = sw_range_db;
      }
      max_db = sw_max_db;
      min_db = max_db - desired_range_db;
    }
  } else {
    if (hw_range_db) {
      min_db = hw_min_db;
      max_db = hw_max_db;
    } else {
      min_db = sw_min_db;
      max_db = sw_max_db;
    }
  }
  double hardware_attenuation, software_attenuation;
  double scaled_attenuation = hw_min_db + sw_min_db;
  if (airplay_volume == -144.0) {
    hardware_attenuation = hw_min_db;
    software_attenuation = sw_min_db;
    if (config.output->mute)
      config.output->mute(1);	// use real mute if it's there
  } else {
    if (config.output->mute)
      config.output->mute(0);	// unmute mute if it's there 
    scaled_attenuation = vol2attn(airplay_volume, max_db, min_db);
    if (hw_range_db) {
      if (scaled_attenuation <= hw_max_db) {
	hardware_attenuation = scaled_attenuation;
	software_attenuation = sw_max_db - (max_db - hw_max_db);
      } else {
	hardware_attenuation = hw_max_db;
	software_attenuation = sw_max_db - (max_db - scaled_attenuation);
      }
    } else {
      software_attenuation = scaled_attenuation;
    }
  }
  if ((config.output->volume) && (hw_range_db)) {
    config.output->volume(hardware_attenuation);
  }
  double temp_fix_volume = 65536.0 * pow(10, software_attenuation / 2000);
  pthread_mutex_lock(&vol_mutex);
  fix_volume = temp_fix_volume;
  pthread_mutex_unlock(&vol_mutex);
#ifdef CONFIG_METADATA
  char *dv = malloc(128);
  if (dv) {
    memset(dv, 0, 128);
    snprintf(dv, 127, "%.2f,%.2f,%.2f,%.2f", airplay_volume, scaled_attenuation / 100.0, min_db / 100.0, max_db / 100.0);
    send_ssnc_metadata('pvol', dv, strlen(dv), 1);
  }
#endif
}

void
player_flush(uint32_t timestamp)
{
  debug(3, "Flush requested up to %u. It seems as if 0 is special.", timestamp);
  pthread_mutex_lock(&flush_mutex);
  flush_requested = 1;
  flush_rtp_timestamp = timestamp;
  pthread_mutex_unlock(&flush_mutex);
  play_segment_reference_frame = 0;
#ifdef CONFIG_METADATA
  send_ssnc_metadata('pfls', NULL, 0, 1);
#endif
}

int
player_play(stream_cfg * stream, pthread_t * player_thread)
{
  packet_count = 0;
  encrypted = stream->encrypted;
  if (config.buffer_start_fill > BUFFER_FRAMES)
    die("specified buffer starting fill %d > buffer size %d", config.buffer_start_fill, BUFFER_FRAMES);
  if (encrypted) {
#ifdef HAVE_LIBPOLARSSL
    memset(&dctx, 0, sizeof(aes_context));
    aes_setkey_dec(&dctx, stream->aeskey, 128);
#endif

#ifdef HAVE_LIBSSL
    AES_set_decrypt_key(stream->aeskey, 128, &aes);
#endif
    aesiv = stream->aesiv;
  }
  init_decoder(stream->fmtp);
  init_buffer();
  please_stop = 0;
  command_start();
#ifdef CONFIG_METADATA
  send_ssnc_metadata('pbeg', NULL, 0, 1);
#endif
#ifdef COMPILE_FOR_LINUX_AND_FREEBSD_AND_CYGWIN
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  int rc = pthread_cond_init(&flowcontrol, &attr);
#endif
#ifdef COMPILE_FOR_OSX
  int rc = pthread_cond_init(&flowcontrol, NULL);
#endif
  if (rc)
    debug(1, "Error initialising condition variable.");
  config.output->start(sampling_rate);
  size_t size = (PTHREAD_STACK_MIN + 256 * 1024);
  pthread_attr_t tattr;
  pthread_attr_init(&tattr);
  rc = pthread_attr_setstacksize(&tattr, size);
  if (rc)
    debug(1, "Error setting stack size for player_thread: %s", strerror(errno));
  pthread_create(player_thread, &tattr, player_thread_func, NULL);
  pthread_attr_destroy(&tattr);
  return 0;
}

void
player_stop(pthread_t * player_thread)
{
  please_stop = 1;
  pthread_cond_signal(&flowcontrol);
  pthread_join(*player_thread, NULL);
#ifdef CONFIG_METADATA
  send_ssnc_metadata('pend', NULL, 0, 1);
#endif
  command_stop();
  free_buffer();
  free_decoder();
  int rc = pthread_cond_destroy(&flowcontrol);
  if (rc)
    debug(1, "Error destroying condition variable.");
}
