/*
 * vim_shell.h
 *
 * Global include file for VIM-Shell. Defines structures and interfaces.
 *
 * This file is part of the VIM-Shell project. http://vimshell.wana.at
 *
 * Author: Thomas Wana <thomas@wana.at>
 *
 * $Id$
 */

#ifndef __VIMSHELL_H

#define __VIMSHELL_H

#include "vim.h"

#include <stdio.h>
#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#include <sys/types.h>
#include <sys/select.h>

/*
 * Master debug flag. Disable this and no debug messages at all will
 * be written anywhere.
 */
//#define VIMSHELL_DEBUG

/*
 * Rendition constants
 */
#define RENDITION_BOLD 1
#define RENDITION_UNDERSCORE 2
#define RENDITION_BLINK 4
#define RENDITION_NEGATIVE 8
#define RENDITION_DIM 16
#define RENDITION_HIDDEN 32

/*
 * charset constants
 */
#define VIMSHELL_CHARSET_USASCII 0
#define VIMSHELL_CHARSET_DRAWING 1

/*
 * Color constants
 */
#define VIMSHELL_COLOR_BLACK 0
#define VIMSHELL_COLOR_RED 1
#define VIMSHELL_COLOR_GREEN 2
#define VIMSHELL_COLOR_YELLOW 3
#define VIMSHELL_COLOR_BLUE 4
#define VIMSHELL_COLOR_MAGENTA 5
#define VIMSHELL_COLOR_CYAN 6
#define VIMSHELL_COLOR_WHITE 7
#define VIMSHELL_COLOR_DEFAULT 9

#define vim_shell_malloc alloc
#define vim_shell_free vim_free

/*
 * The main vim_shell_window struct.
 * Holds everything that is needed to know about a single
 * vim shell. (like file descriptors, window buffers, window
 * positions, etc)
 */
struct vim_shell_window
{
	/*
	 * current dimensions of the window
	 */
	uint16_t size_x;
	uint16_t size_y;

	/*
	 * cursor position and visible flag
	 */
	uint16_t cursor_x;
	uint16_t cursor_y;
	uint16_t cursor_visible;

	/*
	 * Saved cursor positions (ESC 7, ESC 8)
	 */
	uint16_t saved_cursor_x;
	uint16_t saved_cursor_y;

	/*
	 * We support the xterm title hack and store the title in this buffer.
	 */
	char windowtitle[50];

	/*
	 * The output buffer. This is necessary because writes to the shell can be delayed,
	 * e.g. if we are waiting for an incoming ESC sequence to complete.
	 */
	uint8_t outbuf[100];
	uint8_t outbuf_pos;

	/*
	 * Pointer to the window buffer.
	 * The window buffer is the internal representation of the
	 * window's content. The vim shell receives characters from
	 * the terminal, which the terminal emulation translates into
	 * e.g. cursor positions or actual characters. These are placed
	 * here at the right screen position. Its size is size_y*size_x.
	 */
	uint8_t *winbuf;
	uint8_t *fgbuf;
	uint8_t *bgbuf;
	uint8_t *rendbuf;
	uint8_t *charset;

	/*
	 * The tabulator line. It represents a single row. Zero means no
	 * tab at this position, 1 means there is a tab.
	 */
	uint8_t *tabline;

	/*
	 * These buffers hold what's currently physical on the screen.
	 * Note, not on the "virtual" screen, that is the image of the shell,
	 * but the real screen that is printed out in vim_shell_redraw.
	 * This is mainly to implement caching features...
	 * We hold here:
	 * 1 byte foreground-color
	 * 1 byte background-color
	 * 1 byte rendering attributes
	 * 1 byte the actual character
	 */
	uint32_t *phys_screen;

	/*
	 * Flag that determines if we are right in the middle of an
	 * escape sequence coming in.
	 */
	uint8_t in_esc_sequence;

	/*
	 * Buffer for a escape sequence in progress (see in_esc_sequence).
	 */
	uint8_t esc_sequence[50];

	/*
	 * Auto-Margin enabled?
	 */
	uint8_t wraparound;

	/*
	 * Caused the last character a warp around?
	 */
	uint8_t just_wrapped_around;

	/*
	 * The currently used rendition of the shell.
	 */
	uint8_t rendition;
	uint8_t saved_rendition;

	/*
	 * The currently active colors.
	 */
	uint8_t fgcolor;
	uint8_t bgcolor;
	uint8_t saved_fgcolor;
	uint8_t saved_bgcolor;

	/*
	 * Scroll region.
	 */
	uint8_t scroll_top_margin;
	uint8_t scroll_bottom_margin;

	/*
	 * Charset configuration.
	 */
	uint8_t G0_charset;
	uint8_t G1_charset;
	uint8_t active_charset;
	uint8_t saved_G0_charset;
	uint8_t saved_G1_charset;
	uint8_t saved_active_charset;

	/*
	 * Mode switches.
	 */
	uint8_t application_keypad_mode;
	uint8_t application_cursor_mode;
	uint8_t saved_application_keypad_mode;
	uint8_t saved_application_cursor_mode;

	uint8_t insert_mode;
	uint8_t saved_insert_mode;

	/*
	 * This flag determines if the shell should be completely redrawn in the next
	 * vim_shell_redraw, regardless of what we think to know about the screen.
	 */
	uint8_t force_redraw;

	/*
	 * Pointer to the alternate screen. If NULL, there is no alternate screen.
	 * If not NULL, this holds a backup of the screen contents and properties
	 * before the screen switch. Switching back means to copy back the contents
	 * of the alternate screen to the main screen and freeing the alternate screen.
	 */
	struct vim_shell_window *alt;

	/*
	 * file descriptor of the master side of the pty
	 */
	int fd_master;

	/*
	 * pid of the subshell
	 */
	pid_t pid;

};

/*
 * This is set when something goes wrong in one of the
 * vim_shell functions.
 */
extern int vimshell_errno;

/*
 * The debug handle where debug-messages will be written
 */
extern FILE *vimshell_debug_fp;

#define VIMSHELL_SUCCESS 0
#define VIMSHELL_OUT_OF_MEMORY 1
#define VIMSHELL_FORKPTY_ERROR 2
#define VIMSHELL_READ_ERROR 3
#define VIMSHELL_WRITE_ERROR 4
#define VIMSHELL_EXECV_ERROR 5
#define VIMSHELL_SIGACTION_ERROR 6
#define VIMSHELL_READ_EOF 7
#define VIMSHELL_FCNTL_ERROR 8

/*
 * vim_shell.c
 */
extern int vim_shell_init();
extern struct vim_shell_window *vim_shell_new(uint16_t width, uint16_t height);
extern int vim_shell_start(struct vim_shell_window *shell, char *argv[]);
extern char *vim_shell_strerror();
extern int vim_shell_read(struct vim_shell_window *shell);
extern int vim_shell_write(struct vim_shell_window *shell, int c);
extern void vim_shell_redraw(struct vim_shell_window *shell, win_T *win);
extern int vim_shell_do_read_select(fd_set rfds);
extern int vim_shell_do_read_lowlevel(buf_T *buf);
extern void vim_shell_delete(buf_T *buf);
extern void vim_shell_resize(struct vim_shell_window *shell, int width, int height);

/*
 * terminal.c
 */
extern void vim_shell_terminal_input(struct vim_shell_window *shell, char *input, int len);
extern int vim_shell_terminal_output(struct vim_shell_window *shell, int c);

/*
 * screen.c
 */
extern int screen_cur_row, screen_cur_col;	/* last known cursor position */
extern void screen_start_highlight __ARGS((int attr));
extern int screen_attr;

#endif
