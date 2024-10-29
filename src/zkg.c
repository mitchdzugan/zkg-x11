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

char *shell;
char config_file[MAXLEN];
char *config_path;
char **extra_confs;
int num_extra_confs;
int redir_fd;
FILE *status_fifo;
char progress[3 * MAXLEN];
int mapping_count;
int timeout;

char zkg_pid[MAXLEN];

hotkey_t *hotkeys_head, *hotkeys_tail;
bool running, grabbed, toggle_grab, reload, bell, chained, locked;
xcb_keysym_t abort_keysym;
chord_t *abort_chord;

uint16_t num_lock;
uint16_t caps_lock;
uint16_t scroll_lock;

int main(int argc, char *argv[])
{
	int opt;
	char *fifo_path = NULL;
	status_fifo = NULL;
	config_path = NULL;
	mapping_count = 0;
	timeout = 0;
	grabbed = false;
	redir_fd = -1;
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

	reload = toggle_grab = bell = chained = locked = false;
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

		if (reload) {
			signal(SIGUSR1, hold);
			reload_cmd();
			reload = false;
		}

		if (toggle_grab) {
			signal(SIGUSR2, hold);
			toggle_grab_cmd();
			toggle_grab = false;
		}

		if (bell) {
			signal(SIGALRM, hold);
			put_status(TIMEOUT_PREFIX, "Timeout reached");
			abort_chain();
			bell = false;
		}

		if (xcb_connection_has_error(dpy)) {
			warn("The server closed the connection.\n");
			running = false;
		}
	}

	if (redir_fd != -1) {
		close(redir_fd);
	}

	if (status_fifo != NULL) {
		fclose(status_fifo);
	}

	ungrab();
	cleanup();
	xcb_key_symbols_free(symbols);
	xcb_disconnect(dpy);
	return EXIT_SUCCESS;
}

void key_event(xcb_generic_event_t *evt, uint8_t event_type)
{
	xcb_keysym_t keysym = XCB_NO_SYMBOL;
	xcb_button_t button = XCB_NONE;
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
	}

	xcb_flush(dpy);
}

void mapping_notify(xcb_generic_event_t *evt)
{
	if (!mapping_count)
		return;
	xcb_mapping_notify_event_t *e = (xcb_mapping_notify_event_t *) evt;
	PRINTF("mapping notify %u %u\n", e->request, e->count);
	if (e->request == XCB_MAPPING_POINTER)
		return;
	if (xcb_refresh_keyboard_mapping(symbols, e) == 1) {
		destroy_chord(abort_chord);
		get_lock_fields();
		reload_cmd();
		abort_chord = make_chord(abort_keysym, XCB_NONE, 0, XCB_KEY_PRESS, false, false);
		if (mapping_count > 0)
			mapping_count--;
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
	if ((shell = getenv(ZKG_SHELL_ENV)) == NULL && (shell = getenv(SHELL_ENV)) == NULL)
		err("The '%s' environment variable is not defined.\n", SHELL_ENV);
	symbols = xcb_key_symbols_alloc(dpy);
	hotkeys_head = hotkeys_tail = NULL;
	progress[0] = '\0';

	snprintf(zkg_pid, MAXLEN, "%i", getpid());
	setenv("ZKG_PID", zkg_pid, 1);
}

void cleanup(void)
{
	PUTS("cleanup");
	hotkey_t *hk = hotkeys_head;
	while (hk != NULL) {
		hotkey_t *next = hk->next;
		destroy_chain(hk->chain);
		free(hk->cycle);
		free(hk);
		hk = next;
	}
	hotkeys_head = hotkeys_tail = NULL;
}

void reload_cmd(void)
{
	PUTS("reload");
	cleanup();
	load_config(config_file);
	for (int i = 0; i < num_extra_confs; i++)
		load_config(extra_confs[i]);
	ungrab();
	grab();
}

void toggle_grab_cmd(void)
{
	PUTS("toggle grab");
	if (grabbed) {
		ungrab();
	} else {
		grab();
	}
}

void hold(int sig)
{
	if (sig == SIGHUP || sig == SIGINT || sig == SIGTERM)
		running = false;
	else if (sig == SIGUSR1)
		reload = true;
	else if (sig == SIGUSR2)
		toggle_grab = true;
	else if (sig == SIGALRM) {
		bell = true;
		running = false;
	}
}

void put_status(char c, const char *s)
{
	if (status_fifo == NULL) {
		return;
	}
	fprintf(status_fifo, "%c%s\n", c, s);
	fflush(status_fifo);
}
