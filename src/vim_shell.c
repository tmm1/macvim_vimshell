/*
 * vim_shell.c
 *
 * This is the interface layer of the VIM-Shell. VIM only calls functions from this module,
 * e.g. create a new VIM-Shell, read characters from the shell or write characters to the
 * shell. The respective functions pass these things down to the terminal layer, implemented
 * in terminal.c.
 *
 * This file is part of the VIM-Shell project. http://vimshell.wana.at
 *
 * Author: Thomas Wana <thomas@wana.at>
 *
 */


static char *RCSID="$Id$";

#include "vim.h"

#ifdef FEAT_VIMSHELL
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#ifdef HAVE_PTY_H
#include <pty.h>
#endif
#ifdef HAVE_TERMIOS_H
#include <termios.h>
#endif
#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif
#include <fcntl.h>

#if !defined(TIOCSWINSZ)
#  error "VIMSHELL: needs TIOCSWINSZ at the moment, not available on your system, sorry."
#endif

#ifdef VIMSHELL_DEBUG
#  define CHILDDEBUG
#  define SIGDEBUG
#endif

#ifdef CHILDDEBUG
#  define CHILDDEBUGPRINTF(a...) if(vimshell_debug_fp) fprintf(vimshell_debug_fp, a)
#else
#  define CHILDDEBUGPRINTF(a...)
#endif

#ifdef SIGDEBUG
#  define SIGDEBUGPRINTF(a...) if(vimshell_debug_fp) fprintf(vimshell_debug_fp, a)
#else
#  define SIGDEBUGPRINTF(a...)
#endif


int vimshell_errno;

FILE *vimshell_debug_fp=NULL;

/*
 * Main initialization function. Sets up global things always needed for the VIM shell.
 */
int vim_shell_init()
{
#ifdef VIMSHELL_DEBUG
	vimshell_debug_fp=fopen("vimshell.debug", "w");
#endif

	/*
	 * Everything went OK
	 */
	return 0;
}

/*
 * A routine that prints a buffer in 'hexdump -C'-style via printf
 */
static void hexdump(FILE *fp, unsigned char *buffer, long len)
{
	unsigned long pos=0;

	while(len>0)
	{
		int i;
		fprintf(fp,"%08x  ",(unsigned int)pos);
		for(i=0;i<16 && i<len;i++)
		{
			fprintf(fp,"%02x ",buffer[pos+i]);
			if(i==7 || i==15)
				fprintf(fp," ");
		}
		fprintf(fp,"|");
		for(i=0;i<16 && i<len;i++)
		{
			unsigned char c=buffer[pos+i];
			if(isprint(c))
			{
				fprintf(fp,"%c",c);
			}
			else
			{
				fprintf(fp,".");
			}
		}
		fprintf(fp,"|\n");
		len-=16;
		pos+=16;
	}
}

/*
 * Create a new window.
 * @return: pointer to a new struct vim_shell_window on success
 *          NULL on failure
 */
struct vim_shell_window *vim_shell_new(uint16_t width, uint16_t height)
{
	struct vim_shell_window *rval;
	int i;

	rval=(struct vim_shell_window *)vim_shell_malloc(sizeof(struct vim_shell_window));
	if(rval==NULL)
	{
		vimshell_errno=VIMSHELL_OUT_OF_MEMORY;
		return NULL;
	}

	memset(rval, 0, sizeof(struct vim_shell_window));

	rval->size_x=width;
	rval->size_y=height;

	rval->fgcolor=VIMSHELL_COLOR_DEFAULT;
	rval->bgcolor=VIMSHELL_COLOR_DEFAULT;

	rval->G0_charset='B';  // United States (USASCII)
	rval->G1_charset='0';  // Special graphics characters and line drawing set
	rval->active_charset=0;

	rval->winbuf=(uint8_t *)vim_shell_malloc(width*height);
	rval->fgbuf=(uint8_t *)vim_shell_malloc(width*height);
	rval->bgbuf=(uint8_t *)vim_shell_malloc(width*height);
	rval->rendbuf=(uint8_t *)vim_shell_malloc(width*height);
	rval->charset=(uint8_t *)vim_shell_malloc(width*height);
	rval->tabline=(uint8_t *)vim_shell_malloc(width);
	//rval->phys_screen=(uint32_t *)vim_shell_malloc(width*height*4);
	if(rval->winbuf==NULL || rval->fgbuf==NULL || rval->bgbuf==NULL || rval->rendbuf==NULL || rval->charset==NULL ||
			rval->tabline==NULL /* || rval->phys_screen==NULL */)
	{
		vimshell_errno=VIMSHELL_OUT_OF_MEMORY;
		// VIMSHELL TODO: cleanup, free the buffers that were successfully allocated
		vim_shell_free(rval);
		return NULL;
	}
	memset(rval->winbuf, ' ', width*height);
	memset(rval->fgbuf, rval->fgcolor, width*height);
	memset(rval->bgbuf, rval->bgcolor, width*height);
	memset(rval->rendbuf, 0, width*height);
	memset(rval->charset, 0, width*height);
	memset(rval->tabline, 0, width);

#if 0
	for(i=0;i<width*height;i++)
	{
		/*
		 * This is: default foreground, default background, no rendering attributes, ' ' as character
		 */
		rval->phys_screen[i]=0x09090020;
	}
#endif

	/*
	 * Set a tab every 8 columns (default)
	 */
	for(i=1;i<width;i++)
	{
		if((i+1)%8==0 && i+1<width)
			rval->tabline[i]=1;
	}

	rval->wraparound=1;

	rval->cursor_x=0;
	rval->cursor_y=0;
	rval->cursor_visible=1;

	rval->scroll_top_margin=0;
	rval->scroll_bottom_margin=height-1;

	CHILDDEBUGPRINTF("%s: vimshell created, width = %d, height = %d\n",
			__FUNCTION__, width, height);

	vimshell_errno=VIMSHELL_SUCCESS;
	return rval;
}

/*
 * start the program in argv in the shell window.
 */
int vim_shell_start(struct vim_shell_window *shell, char *argv[])
{
	struct winsize winsize;
	struct termios termios;

	memset(&termios, 0, sizeof(struct termios));

	/*
	 * Set terminal parameters.
	 */
	termios.c_iflag=ICRNL;
	termios.c_oflag=ONLCR | OPOST;
	termios.c_cflag=CS8 | CREAD | HUPCL;
	termios.c_lflag=ECHO | ECHOE | ECHOK | ECHOKE | ISIG | ECHOCTL | ICANON;
	termios.c_cc[VMIN]=1;
	termios.c_cc[VTIME]=0;
	termios.c_cc[VINTR]=003;
	termios.c_cc[VQUIT]=034;
	termios.c_cc[VERASE]=0177;
	termios.c_cc[VKILL]=025;
	termios.c_cc[VEOF]=004;
	termios.c_cc[VSTART]=021;
	termios.c_cc[VSTOP]=023;
	termios.c_cc[VSUSP]=032;

	winsize.ws_row=shell->size_y;
	winsize.ws_col=shell->size_x;
	winsize.ws_xpixel=0;
	winsize.ws_ypixel=0;

	/*
	 * allocate a pty, fork and exec the command
	 */
	shell->pid=forkpty(&shell->fd_master, NULL, &termios, &winsize);
	if(shell->pid==0)
	{
		/*
		 * child code
		 */
		setenv("TERM", "screen", 1);
		if(execvp(*argv, argv)<0)
		{
			vimshell_errno=VIMSHELL_EXECV_ERROR;
			// VIMSHELL TODO close fds??
			return -1;
		}
	}
	else if(shell->pid<0)
	{
		vimshell_errno=VIMSHELL_FORKPTY_ERROR;
		return -1;
	}

	/*
	 * Set the file descriptor to non-blocking
	 */
	if(fcntl(shell->fd_master, F_SETFL, fcntl(shell->fd_master, F_GETFL) | O_NONBLOCK)<0)
	{
		vimshell_errno=VIMSHELL_FCNTL_ERROR;
		return -1;
	}

	/*
	 * parent code
	 */
	if(ioctl(shell->fd_master, TIOCSWINSZ, &winsize)<0)
	{
		CHILDDEBUGPRINTF( "%s: ERROR: ioctl to change window size: %s\n",
				__FUNCTION__,strerror(errno));
	}

	return 0;
}

/*
 * this is much like strerror, only with vimshell_errno. Additionally, if errno is not
 * null, the formatted error message (strerror) will be appended to the output.
 */
char *vim_shell_strerror()
{
	static char errbuf[200];
	static char *errmsg[]={"Success",
		"Out of memory",
		"forkpty error",
		"read error",
		"write error",
		"execv error",
		"sigaction error",
	        "read (EOF)",
	        "fcntl error"};

	if(errno==0)
		return errmsg[vimshell_errno];

	snprintf(errbuf, sizeof(errbuf), "%s: %s", errmsg[vimshell_errno], strerror(errno));
	return errbuf;
}

/*
 * Read what is available from the master pty and tear it through the
 * terminal emulation. This will fill the window buffer.
 */
int vim_shell_read(struct vim_shell_window *shell)
{
	// for now, copy what we have into the window buffer.
	char input[2000];
	int rval;

retry:
	if((rval=read(shell->fd_master, input, sizeof(input)))<0)
	{
		if(errno==EINTR)
			goto retry;
		if(errno==EAGAIN)
		{
			/*
			 * This function should only be called when there really
			 * are bytes available to read from the fd_master. This
			 * is ensured by calling select() before this function.
			 * But it seems that there is a race with a SIGWINSZ right
			 * in the middle of a select() (reporting there are characters
			 * available) and the following read() that will block forever.
			 * It seems that SIGWINSZ flushes the processes input queue, at
			 * least on Linux.
			 *
			 * So if we get here although there is nothing to read, don't
			 * report an error, just return successfully.
			 */
			goto success;
		}

		vimshell_errno=VIMSHELL_READ_ERROR;
		return -1;
	}

	if(rval==0)
	{
		/*
		 * This means end-of-file, the subprocess exited.
		 */
		vimshell_errno=VIMSHELL_READ_EOF;
		return -1;
	}

#ifdef RAWDEBUG
	fprintf(vimshell_debug_fp, "\nincoming bytes:\n");
	hexdump(vimshell_debug_fp, input, rval);
#endif

	/*
	 * Interface to the terminal layer: give the input buffer to the
	 * terminal emulator for processing.
	 */
	vim_shell_terminal_input(shell, input, rval);

success:
	vimshell_errno=VIMSHELL_SUCCESS;
	return 0;
}

/*
 * Write a character to the VIM-Shell.
 * Blow it through terminal_output so it gets translated correctly etc...
 */
int vim_shell_write(struct vim_shell_window *shell, int c)
{
	if(vim_shell_terminal_output(shell, c)<0)
	{
		vimshell_errno=VIMSHELL_WRITE_ERROR;
		return -1;
	}

	vimshell_errno=VIMSHELL_SUCCESS;
	return 0;
}

/*
 * Free everything that is associated with this shell window.
 * Also terminates the process. The shell pointer will be set to NULL.
 * The buffer will be restored to a usable, empty buffer.
 */
void vim_shell_delete(buf_T *buf)
{
	struct vim_shell_window *sh=buf->shell;
	int status;
	pid_t pid;

	/*
	 * First, kill the child and wait for it.
	 * VIMSHELL TODO: this can be extended a lot. e.g. using a SIGCHLD handler
	 * or an SIGALRM handler and kill(SIGKILL) the process when the alarm fires
	 * etc. But for now (and most of all cases) this should be OK.
	 */
	pid=sh->pid;
	if(pid>0)
	{
		kill(pid, SIGTERM);
		kill(pid, SIGHUP);
		while(waitpid(pid, &status, 0)<0 && errno==EINTR);
		CHILDDEBUGPRINTF( "%s: PID %u terminated, exit status = %d\n", __FUNCTION__, pid,
			WEXITSTATUS(status));
	}

	/*
	 * The child is dead. Clean up
	 */
	close(sh->fd_master);
	if(sh->alt)
	{
		vim_shell_free(sh->alt->winbuf);
		vim_shell_free(sh->alt->fgbuf);
		vim_shell_free(sh->alt->bgbuf);
		vim_shell_free(sh->alt->rendbuf);
		vim_shell_free(sh->alt->tabline);
		vim_shell_free(sh->alt->charset);
		vim_shell_free(sh->alt);
		sh->alt=NULL;
	}
	vim_shell_free(sh->winbuf);
	vim_shell_free(sh->fgbuf);
	vim_shell_free(sh->bgbuf);
	vim_shell_free(sh->rendbuf);
	vim_shell_free(sh->tabline);
	vim_shell_free(sh->charset);
	vim_shell_free(sh);

	CHILDDEBUGPRINTF( "%s: vimshell %p freed.\n", __FUNCTION__, sh);

	buf->is_shell=0;
	buf->shell=NULL;
	buf->b_p_ro=FALSE;
}

/*
 * Does the work of actually resizing the shell's buffers. Deallocating them,
 * reallocating them, copying over the old contents to the right places, etc...
 * rval: 0 = success, <0 = error
 */
static int internal_screenbuf_resize(struct vim_shell_window *shell, int width, int height)
{
	uint8_t *owinbuf, *ofgbuf, *obgbuf, *orendbuf, *otabline, *ocharset;
	int x, y, len, vlen;
	uint16_t oldwidth, oldheight;

	oldwidth=shell->size_x;
	oldheight=shell->size_y;
	shell->size_x=(uint16_t)width;
	shell->size_y=(uint16_t)height;

	owinbuf=shell->winbuf;
	ofgbuf=shell->fgbuf;
	obgbuf=shell->bgbuf;
	orendbuf=shell->rendbuf;
	ocharset=shell->charset;
	otabline=shell->tabline;

	shell->winbuf=(uint8_t *)vim_shell_malloc(width*height);
	shell->fgbuf=(uint8_t *)vim_shell_malloc(width*height);
	shell->bgbuf=(uint8_t *)vim_shell_malloc(width*height);
	shell->rendbuf=(uint8_t *)vim_shell_malloc(width*height);
	shell->charset=(uint8_t *)vim_shell_malloc(width*height);
	shell->tabline=(uint8_t *)vim_shell_malloc(width);
	if(shell->winbuf==NULL || shell->fgbuf==NULL || shell->bgbuf==NULL || shell->rendbuf==NULL || shell->charset==NULL ||
			shell->tabline==NULL)
	{
		vimshell_errno=VIMSHELL_OUT_OF_MEMORY;
		if(shell->winbuf) vim_shell_free(shell->winbuf);
		if(shell->fgbuf) vim_shell_free(shell->fgbuf);
		if(shell->bgbuf) vim_shell_free(shell->bgbuf);
		if(shell->rendbuf) vim_shell_free(shell->rendbuf);
		if(shell->charset) vim_shell_free(shell->charset);
		if(shell->tabline) vim_shell_free(shell->tabline);

		/*
		 * Reassign the old buffers, they are still valid. And bring the shell
		 * back to a sane state.
		 */
		shell->winbuf=owinbuf;
		shell->fgbuf=ofgbuf;
		shell->bgbuf=obgbuf;
		shell->rendbuf=orendbuf;
		shell->charset=ocharset;
		shell->tabline=otabline;

		shell->size_x=oldwidth;
		shell->size_y=oldheight;

		return -1;
	}
	memset(shell->winbuf, ' ', width*height);
	memset(shell->fgbuf, shell->fgcolor, width*height);
	memset(shell->bgbuf, shell->bgcolor, width*height);
	memset(shell->rendbuf, 0, width*height);
	memset(shell->charset, 0, width*height);
	memset(shell->tabline, 0, width);

	CHILDDEBUGPRINTF( "%s: width = %d, height = %d, oldwidth = %d, oldheight = %d\n",__FUNCTION__,width,height,
			oldwidth,oldheight);

	/*
	 * copy over the old contents of the screen, line by line (!)
	 */
	len=(oldwidth<width ? oldwidth : width);
	vlen=(oldheight<height ? oldheight : height);
	for(y=0;y<vlen;y++)
	{
		int y_off;
		y_off=oldheight-vlen;
		memcpy(shell->winbuf+y*width, owinbuf+(y+y_off)*oldwidth, len);
		memcpy(shell->fgbuf+y*width, ofgbuf+(y+y_off)*oldwidth, len);
		memcpy(shell->bgbuf+y*width, obgbuf+(y+y_off)*oldwidth, len);
		memcpy(shell->rendbuf+y*width, orendbuf+(y+y_off)*oldwidth, len);
		memcpy(shell->charset+y*width, ocharset+(y+y_off)*oldwidth, len);
	}
	memcpy(shell->tabline, otabline, len);

	/*
	 * free the old contents
	 */
	vim_shell_free(owinbuf);
	vim_shell_free(ofgbuf);
	vim_shell_free(obgbuf);
	vim_shell_free(orendbuf);
	vim_shell_free(otabline);
	vim_shell_free(ocharset);

	/*
	 * Correct tabs
	 */
	if(oldwidth<width)
	{
		for(x=oldwidth;x<width;x++)
		{
			if((x+1)%8==0 && x+1<width)
				shell->tabline[x]=1;
		}
	}

	/*
	 * Correct cursor
	 */
	if(shell->cursor_x>=shell->size_x)
		shell->cursor_x=shell->size_x-1;
	if(shell->cursor_y>=shell->size_y)
		shell->cursor_y=shell->size_y-1;

	/*
	 * Update scroll region
	 */
	shell->scroll_top_margin=0;
	shell->scroll_bottom_margin=shell->size_y-1;

	/*
	 * Invalidate the vimshell screen buffer, so vim_shell_redraw redraws the whole
	 * screen.
	 */
	shell->force_redraw=1;

	return 0;
}

/*
 * Resizes the shell.
 * It reallocates all the size dependant buffers and instructs the shell to change
 * its size.
 * The width and height parameters are the *desired* width and height. The actual
 * width and height is dependant on wether all windows that currently render this shell
 * are able to display this width and height.
 */
void vim_shell_resize(struct vim_shell_window *shell, int want_width, int want_height)
{
	int width, height;
	struct winsize ws;
	win_T *win;

	width=want_width;
	height=want_height;
	FOR_ALL_WINDOWS(win)
	{
		if(win->w_buffer && win->w_buffer->is_shell && win->w_buffer->shell==shell)
		{
			if(win->w_width<width)
				width=win->w_width;
			if(win->w_height<height)
				height=win->w_height;
		}
	}

	CHILDDEBUGPRINTF( "%s: resizing to %d, %d\n",__FUNCTION__,width,height);

	if(internal_screenbuf_resize(shell, width, height)<0)
	{
		CHILDDEBUGPRINTF("%s: error while resizing.\n", __FUNCTION__);
		return;
	}
	if(shell->alt!=NULL)
	{
		if(internal_screenbuf_resize(shell->alt, width, height)<0)
		{
			CHILDDEBUGPRINTF("%s: error while resizing the backup screen. Recovering...\n", __FUNCTION__);

			/*
			 * We now really have a problem. The main shell window is already
			 * resized and this one didn't work. What should we do? Just destroy the screen
			 * backup so it never gets restored. internal_screenbuf_resize already did
			 * the job to of freeing the SINGle buffers, we just have to free the remaining struct.
			 */
			vim_shell_free(shell->alt);
			shell->alt=NULL;
		}
	}

	/*
	 * Tell the shell that the size has changed.
	 */
	ws.ws_row=height;
	ws.ws_col=width;
	ws.ws_xpixel=0;
	ws.ws_ypixel=0;
	if(ioctl(shell->fd_master, TIOCSWINSZ, &ws)<0)
	{
		CHILDDEBUGPRINTF( "%s: ERROR: ioctl to change window size: %s\n",
				__FUNCTION__,strerror(errno));
	}
}

/*
 * Draws the Shell-Buffer into the VIM-Window.
 */
void vim_shell_redraw(struct vim_shell_window *shell, win_T *win)
{
	int x, y;
	int win_row, win_col;
	int off;
	int last_set_fg, last_set_bg;
	int cs_state;
	int term_is_bold, term_is_underline, term_is_negative;
	int saved_screen_cur_row, saved_screen_cur_col;
	int force_redraw;
	int using_gui=0;
	int t_colors_original=t_colors;

	if(t_colors>15)
	{
		t_colors = 15;
	}

#ifdef FEAT_GUI
	if(gui.in_use)
	{
		using_gui=1;
	}
#endif

	win_row=W_WINROW(win);
	win_col=W_WINCOL(win);

	force_redraw=shell->force_redraw;

	// invalidate the color cache
	last_set_fg=last_set_bg=-1;
	cs_state=VIMSHELL_CHARSET_USASCII;

	saved_screen_cur_row=screen_cur_row;
	saved_screen_cur_col=screen_cur_col;

	// go to normal mode
	term_is_bold=term_is_underline=term_is_negative=0;
	screen_stop_highlight();

	for(y=0;y<shell->size_y;y++)
	{
		size_t index=y*shell->size_x;
		int skipped, y_reposition_necessary;

		off=LineOffset[win_row+y]+win_col;
		skipped=0;
		y_reposition_necessary=1;
		for(x=0;x<shell->size_x;x++)
		{
			uint8_t c=shell->winbuf[index];
			sattr_T r=(sattr_T)shell->rendbuf[index];
			uint8_t fg=shell->fgbuf[index];
			uint8_t bg=shell->bgbuf[index];
			uint8_t cs=shell->charset[index];
			uint8_t fg_color=fg&0xF;
			uint8_t bg_color=bg&0xF;
			if(t_colors > 15)
			{
				bg_color=0x00;
				fg_color=0x03;
			}

			/*
			 * Switch terminal charset if necessary
			 */
			if(cs_state!=cs)
			{
				cs_state=cs;
				if(cs==VIMSHELL_CHARSET_USASCII)
				{
					// VIMSHELL TODO: make a term code out of this hack
					out_str_nf("\033(B");
					CHILDDEBUGPRINTF( "%s: switched terminal to normal charset\n",__FUNCTION__);
				}
				else if(cs==VIMSHELL_CHARSET_DRAWING)
				{
					// VIMSHELL TODO: make a term code out of this hack
					out_str_nf("\033(0");
					CHILDDEBUGPRINTF( "%s: switched terminal to alternate charset\n",__FUNCTION__);
				}
			}

			/*
			 * Store the foreground and background color along with the rendering attributes.
			 */
			r |= (fg&0x0F)<<12 | (bg&0x0F)<<8;

			/*
			 * Only do an update if render attributes or the character
			 * has changed at this position.
			 */
			if(ScreenLines[off]!=c || ScreenAttrs[off]!=r || force_redraw)
			{
				// render attributes
				if( ((r & RENDITION_BOLD)==0) == (term_is_bold==0) &&
						((r & RENDITION_UNDERSCORE)==0) == (term_is_underline==0) &&
						((r & RENDITION_NEGATIVE)==0) == (term_is_negative==0))
				{
					/*
					 * already in the right rendition mode ...
					 */
				}
				else if(using_gui==0)
				{
					out_str_nf(T_ME);
					term_is_bold=term_is_underline=term_is_negative=0;
					last_set_fg=last_set_bg=-1;
					if ((r & RENDITION_BOLD) && !term_is_bold)
					{
						if(T_MD!=NULL)
							out_str_nf(T_MD);
						term_is_bold=1;
					}
					if ((r & RENDITION_UNDERSCORE) && !term_is_underline)
					{
						if(T_US != NULL)
							out_str_nf(T_US);
						term_is_underline=1;
					}
					if ((r & RENDITION_NEGATIVE) && !term_is_negative)
					{
						if(T_MR!=NULL)
							out_str_nf(T_MR);
						term_is_negative=1;
					}
				}

				// colors
				if(t_colors > 1 && using_gui==0)
				{
					// VIMSHELL TODO: not every terminal will understand these colors ...
					// look at tag:cterm_normal_fg_color
					if(last_set_fg!=fg_color)
					{
						term_fg_color(fg_color);
						last_set_fg=fg_color;
					}
					if(last_set_bg!=bg_color)
					{
						term_bg_color(bg_color);
						last_set_bg=bg_color;
					}
				}

				ScreenLines[off]=c;
				ScreenAttrs[off]=r;

				if(y_reposition_necessary || skipped>0)
				{
					/*
					 * Bring the cursor to where we need it.
					 */
					term_windgoto(win_row+y, win_col+x);
					skipped=0;
					y_reposition_necessary=0;
				}

				// print it
				out_char(c);
			}
			else
			{
				skipped++;
			}

			off++;
			index++;
		}
	}
	/*
	 * Always leave this function with the normal ASCII charset enabled and
	 * with sane rendering attributes (normal mode).
	 */
	if(cs_state!=VIMSHELL_CHARSET_USASCII)
	{
		// VIMSHELL TODO: make a term code out of this hack
		out_str_nf("\033(B");
		CHILDDEBUGPRINTF( "%s: switched terminal to normal charset\n",__FUNCTION__);
	}

	/*
	 * Move the cursor to where VIM thinks it is :)
	 */
	term_windgoto(saved_screen_cur_row, saved_screen_cur_col);

	/*
	 * Position the cursor.
	 * VIMSHELL TODO: we could cache that, e.g. when the cursor didn't move don't turn
	 * it on again etc.
	 */
	win->w_wrow=shell->cursor_y;
	win->w_wcol=shell->cursor_x;
	setcursor();
	cursor_on();

	/*
	 * Restore the rendering attributes
	 */
	out_str_nf(T_ME);
	screen_start_highlight(screen_attr);
	out_flush();

	if(shell->force_redraw)
		shell->force_redraw=0;

	t_colors = t_colors_original;
}

/*
 * Really do the read, finally :)
 * Returns 1 if the contents of the window are VALID (in VIM speak)
 * Returns 2 if the contents have to be CLEARed (after the shell has died)
 */
int vim_shell_do_read_lowlevel(buf_T *buf)
{
	int rval=1;
	if(vim_shell_read(buf->shell)<0)
	{
		/*
		 * Shell died? Cleanup. Also remove the RO attribute from the
		 * buffer.
		 */
		vim_shell_delete(buf);
		rval=2;
	}

	return rval;
}

/*
 * This function is called from two places: os_unix.c and ui.c, and handles
 * shell reads that are necessary because a select() became ready. This function
 * is here to avoid identical code in both places.
 * It returns the number of shell-reads.
 * If there was no activity in any of the shells, it returns 0.
 */
int vim_shell_do_read_select(fd_set rfds)
{
	/*
	 * Loop through all buffers and see if they are vimshells.
	 * If yes, check if there are read events ready for the appropriate
	 * fds. If so, call the shell's read handler.
	 */
	buf_T *buf;
	int did_redraw=0;
	int rval=0;

	for(buf=firstbuf;buf!=NULL;buf=buf->b_next)
	{
		if(buf->is_shell != 0)
		{
			if(FD_ISSET(buf->shell->fd_master, &rfds))
			{
				int r;

				r=vim_shell_do_read_lowlevel(buf);
				if(r>did_redraw)
					did_redraw=r;

				rval++;

				if(r==1 && updating_screen==FALSE)
					redraw_buf_later(buf, VALID);
				else if(r==2 && updating_screen==FALSE)
					redraw_buf_later(buf, CLEAR);
			}
		}
	}

	/*
	 * Only redraw if we aren't currently redrawing, to avoid endless recursions.
	 * update_screen calls win_update, which calls win_line, which calls breakcheck,
	 * which again calls RealWaitForChar which calls this function ...
	 */
	if(updating_screen==FALSE)
	{
		if(did_redraw==1)
		{
			update_screen(VALID);
		}
		else if(did_redraw==2 || did_redraw==3)
		{
			update_screen(CLEAR);
			out_flush();
		}
	}

	return rval;
}
#endif
