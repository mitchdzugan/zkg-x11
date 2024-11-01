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

#include <xcb/xcb_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include "parse.h"
#include "grab.h"

xcb_connection_t *dpy;
xcb_window_t root;
xcb_key_symbols_t *symbols;

int timeout;

char zkg_pid[MAXLEN];

bool running, grabbed, bell, chained, locked;
xcb_keysym_t abort_keysym;

uint16_t num_lock;
uint16_t caps_lock;
uint16_t scroll_lock;

int main(int argc, char *argv[])
{
	int opt;
	timeout = 0;
	grabbed = false;
	abort_keysym = ESCAPE_KEYSYM;

	while ((opt = getopt(argc, argv, "hvm:t:c:r:s:a:")) != -1) {
		switch (opt) {
			case 'v':
				printf("%s\n", VERSION);
				exit(EXIT_SUCCESS);
				break;
			case 'h':
				printf("zkg [-h|-v|-t TIMEOUT|-a ABORT_KEYSYM]\n");
				exit(EXIT_SUCCESS);
				break;
			case 't':
				timeout = atoi(optarg);
				break;
			case 'a':
				if (!parse_keysym(optarg, &abort_keysym)) {
					warn("Invalid keysym name: %s.\n", optarg);
				}
				break;
		}
	}

	if (timeout > 0) {
		alarm(timeout);
	}

	signal(SIGINT, hold);
	signal(SIGHUP, hold);
	signal(SIGTERM, hold);
	signal(SIGUSR1, hold);
	signal(SIGUSR2, hold);
	signal(SIGALRM, hold);

	setup();
	get_standard_keysyms();
	get_lock_fields();
	grab();

	xcb_generic_event_t *evt;
	int fd = xcb_get_file_descriptor(dpy);

	fd_set descriptors;

	bell = chained = locked = false;
	running = true;

	xcb_flush(dpy);

	while (running) {
		FD_ZERO(&descriptors);
		FD_SET(STDIN_FILENO, &descriptors);
		FD_SET(fd, &descriptors);

		if (select(fd + 1, &descriptors, NULL, NULL, NULL) > 0) {
			if (FD_ISSET(STDIN_FILENO, &descriptors)) {
				running = false;
			} else {
				while ((evt = xcb_poll_for_event(dpy)) != NULL) {
					uint8_t event_type = XCB_EVENT_RESPONSE_TYPE(evt);
					switch (event_type) {
						case XCB_KEY_PRESS:
						case XCB_KEY_RELEASE:
							key_event(evt, event_type);
							break;
						case XCB_MAPPING_NOTIFY:
							mapping_notify(evt);
							break;
						default:
							PRINTF("received event %u\n", event_type);
							break;
					}
					free(evt);
				}
			}
		}

		if (bell) {
			signal(SIGALRM, hold);
			bell = false;
		}

		if (xcb_connection_has_error(dpy)) {
			warn("The server closed the connection.\n");
			running = false;
		}
	}

	ungrab();
	xcb_key_symbols_free(symbols);
	xcb_disconnect(dpy);
	return EXIT_SUCCESS;
}

void key_event(xcb_generic_event_t *evt, uint8_t event_type)
{
	xcb_keysym_t keysym = XCB_NO_SYMBOL;
	uint16_t modfield = 0;
	uint16_t lockfield = num_lock | caps_lock | scroll_lock;
	parse_event(evt, event_type, &keysym, &modfield);
	modfield &= ~lockfield & MOD_STATE_FIELD;
	if (keysym == abort_keysym && event_type == XCB_KEY_RELEASE) {
		running = false;
	} else if (keysym != XCB_NO_SYMBOL) {
		printf(
			"%u %s %u\n",
			keysym,
			event_type == XCB_KEY_RELEASE ? "release" : "press",
			modfield
		);
		fflush(stdout);
	}

	xcb_flush(dpy);
}

void mapping_notify(xcb_generic_event_t *evt)
{
	xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *) evt;
	PRINTF("mapping notify %u %u\n", e->request, e->count);
	if (e->request == XCB_MAPPING_POINTER)
		return;
	if (xcb_refresh_keyboard_mapping(symbols, e) == 1) {
		get_lock_fields();
	}
}

void setup(void)
{
	int screen_idx;
	dpy = xcb_connect(NULL, &screen_idx);
	if (xcb_connection_has_error(dpy))
		err("Can't open display.\n");
	xcb_screen_t *screen = NULL;
	xcb_screen_iterator_t screen_iter = xcb_setup_roots_iterator(xcb_get_setup(dpy));
	for (; screen_iter.rem; xcb_screen_next(&screen_iter), screen_idx--) {
		if (screen_idx == 0) {
			screen = screen_iter.data;
			break;
		}
	}
	if (screen == NULL)
		err("Can't acquire screen.\n");
	root = screen->root;
	symbols = xcb_key_symbols_alloc(dpy);

	snprintf(zkg_pid, MAXLEN, "%i", getpid());
	setenv("ZKG_PID", zkg_pid, 1);
}

void hold(int sig)
{
	if (sig == SIGHUP || sig == SIGINT || sig == SIGTERM || sig == SIGALRM) {
		running = false;
	}
}

