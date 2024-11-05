/* Copyright (c) 2013, Bastien Dejean
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include "parse.h"
#include "grab.h"

int msleep(long msec)
{
    struct timespec ts;
    int res;

    if (msec < 0)
    {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

void grab(void)
{
	xcb_grab_keyboard_cookie_t cookie;
	xcb_grab_keyboard_reply_t *reply;
	PUTS("grab");
	int i = 0;
	bool did_grab = false;
	while (i < 20) {
		cookie = xcb_grab_keyboard(dpy, false, root, XCB_CURRENT_TIME, XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
		if ((reply = xcb_grab_keyboard_reply(dpy, cookie, NULL))) {
			did_grab = reply->status == 0;
			PRINTF("reply->status [%i], nth: [%i]\n", reply->status, i);
			free(reply);
			if (did_grab) { break; }
			xcb_flush(dpy);
			msleep(10);
			i++;
		}
	}
	xcb_flush(dpy);
	if (!did_grab) {
		printf(stderr, "Failed to grab keyboard\n");
		exit(EXIT_FAILURE);
	}
	grabbed = true;
}

void ungrab(void)
{
	PUTS("ungrab");
	xcb_ungrab_keyboard(dpy, XCB_CURRENT_TIME);
	xcb_flush(dpy);
	grabbed = false;
}
