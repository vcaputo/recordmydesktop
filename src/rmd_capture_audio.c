/******************************************************************************
*                            recordMyDesktop                                  *
*******************************************************************************
*                                                                             *
*            Copyright (C) 2006,2007,2008 John Varouhakis                     *
*                                                                             *
*                                                                             *
*   This program is free software; you can redistribute it and/or modify      *
*   it under the terms of the GNU General Public License as published by      *
*   the Free Software Foundation; either version 2 of the License, or         *
*   (at your option) any later version.                                       *
*                                                                             *
*   This program is distributed in the hope that it will be useful,           *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of            *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the             *
*   GNU General Public License for more details.                              *
*                                                                             *
*   You should have received a copy of the GNU General Public License         *
*   along with this program; if not, write to the Free Software               *
*   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA  *
*                                                                             *
*                                                                             *
*                                                                             *
*   For further information contact me at johnvarouhakis@gmail.com            *
******************************************************************************/

#include "config.h"
#include "rmd_capture_audio.h"

#include "rmd_jack.h"
#include "rmd_opendev.h"
#include "rmd_threads.h"
#include "rmd_types.h"

#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>


void *rmdCaptureAudio(ProgData *pdata)
{

#ifdef HAVE_LIBASOUND
	int frames = pdata->periodsize;
#endif
	rmdThreadsSetName("rmdCaptureAudio");

	//start capturing only after first frame is taken
	nanosleep(&(struct timespec){ .tv_nsec = pdata->frametime_us * 1000 }, NULL);

	while (pdata->running) {
		int		sret = 0;
		SndBuffer	*newbuf, *tmp;

		if (pdata->paused) {
#ifdef HAVE_LIBASOUND
			if (!pdata->hard_pause) {
				snd_pcm_pause(pdata->sound_handle, 1);
				pthread_mutex_lock(&pdata->pause_mutex);
				pthread_cond_wait(&pdata->pause_cond, &pdata->pause_mutex);
				pthread_mutex_unlock(&pdata->pause_mutex);
				snd_pcm_pause(pdata->sound_handle, 0);
			} else {//device doesn't support pause(is this the norm?mine doesn't)
				snd_pcm_close(pdata->sound_handle);
				pthread_mutex_lock(&pdata->pause_mutex);
				pthread_cond_wait(&pdata->pause_cond, &pdata->pause_mutex);
				pthread_mutex_unlock(&pdata->pause_mutex);
				pdata->sound_handle =
					rmdOpenDev(pdata->args.device,
							   &pdata->args.channels,
							   &pdata->args.frequency,
							   &pdata->args.buffsize,
							   NULL,
							   NULL,
							   NULL//let's hope that the device capabilities
								   //didn't magically change
							   );
				if (pdata->sound_handle == NULL) {
					fprintf(stderr, "Couldn't reopen audio device. Exiting\n");
					pdata->running = FALSE;
					errno = 3;
					pthread_exit(&errno);
				}
			}
#else
			close(pdata->sound_handle);
			pthread_mutex_lock(&pdata->pause_mutex);
			pthread_cond_wait(&pdata->pause_cond, &pdata->pause_mutex);
			pthread_mutex_unlock(&pdata->pause_mutex);
			pdata->sound_handle = rmdOpenDev(	pdata->args.device,
								pdata->args.channels,
								pdata->args.frequency);
			if (pdata->sound_handle < 0) {
				fprintf(stderr, "Couldn't reopen audio device. Exiting\n");
				pdata->running = FALSE;
				errno = 3;
				pthread_exit(&errno);
			}
#endif
		}

		//create new buffer
		newbuf = (SndBuffer *)malloc(sizeof(SndBuffer));
#ifdef HAVE_LIBASOUND
		newbuf->data = (signed char *)malloc(frames * pdata->sound_framesize);
#else
		newbuf->data = (signed char *)malloc((pdata->args.buffsize << 1) * pdata->args.channels);
#endif
		newbuf->next = NULL;

		//read data into new buffer
#ifdef HAVE_LIBASOUND
		while (sret < frames) {
			int temp_sret = snd_pcm_readi(	pdata->sound_handle,
							newbuf->data + pdata->sound_framesize * sret,
							frames-sret);
			if (temp_sret == -EPIPE) {
				fprintf(stderr,	"%s: Overrun occurred.\n",
						snd_strerror(temp_sret));
				snd_pcm_prepare(pdata->sound_handle);
			} else if (temp_sret < 0) {
				fprintf(stderr,	"An error occured while reading audio data:\n"
						" %s\n",
						snd_strerror(temp_sret));
				snd_pcm_prepare(pdata->sound_handle);
			} else
				sret += temp_sret;
		}
#else
		sret = 0;
		//oss recording loop
		do {
			int temp_sret = read(	pdata->sound_handle,
						&newbuf->data[sret],
						(pdata->args.buffsize << 1) *
						pdata->args.channels)-sret;
			if (temp_sret < 0) {
				fprintf(stderr,	"An error occured while reading from soundcard"
						"%s\nError description:\n%s\n",
						pdata->args.device, strerror(errno));
			} else
				sret += temp_sret;
		} while (sret < (pdata->args.buffsize << 1) * pdata->args.channels);
#endif

		/* Since sound buffers are queued here by allocating buffers, their "stream time"
		 * can be accounted for here instead of after encoding, since encoding isn't lossy
		 * in the time domain there shouldn't be any disparity.  Infact, buffer underruns
		 * should get accounted for too FIXME TODO
		 */
		pthread_mutex_lock(&pdata->avd_mutex);
		pdata->avd -= pdata->periodtime_us;
		pthread_mutex_unlock(&pdata->avd_mutex);

		//queue the new buffer
		pthread_mutex_lock(&pdata->sound_buffer_mutex);
		tmp = pdata->sound_buffer;
		if (!tmp)
			pdata->sound_buffer = newbuf;
		else {
			while (tmp->next != NULL)
				tmp = tmp->next;

			tmp->next = newbuf;
		}
		pthread_cond_signal(&pdata->sound_data_read);
		pthread_mutex_unlock(&pdata->sound_buffer_mutex);
	}
#ifdef HAVE_LIBASOUND
	snd_pcm_close(pdata->sound_handle);
#else
	close(pdata->sound_handle);
#endif
	pthread_exit(&errno);
}
