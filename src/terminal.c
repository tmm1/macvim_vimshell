/*
 * terminal.c
 *
 * This is the screen-like terminal emulator; it is a better vt100 and supports the
 * same commands that screen does (including color etc).
 *
 * The interface is vim_shell_terminal_input and vim_shell_terminal_output. These
 * functions get characters from the shell (or from the user) and process them
 * accordingly to the terminal rules (ESC sequences etc). They then work directly
 * on the vim_shell-window-buffer, updating its contents.
 *
 * VIMSHELL TODO: *) don't scroll scroll region if cursor is outside
 *                *) change the buffer name according to the window title
 *                *) CSI (0x9B)
 *                *) Get the ESC codes for switching charsets from terminfo/termcap
 *                *) Origin mode
 *                *) "erase with background color"
 *
 * This file is part of the VIM-Shell project. http://vimshell.wana.at
 *
 * Author: Thomas Wana <thomas@wana.at>
 *
 */

static char *RCSID="$Id$";

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>

#include "vim.h"

#ifdef FEAT_VIMSHELL
#ifdef VIMSHELL_DEBUG
#  define ESCDEBUG
/*
 * Really extreme logging
 * Log *every* character we write to the shell
 */
//#  define VERBOSE
#endif

#ifdef ESCDEBUG
#  define ESCDEBUGPRINTF(a...) if(vimshell_debug_fp) { fprintf(vimshell_debug_fp, a); fflush(vimshell_debug_fp); }
#else
#  define ESCDEBUGPRINTF(a...)
#endif

#ifdef VERBOSE
#  define VERBOSEPRINTF(a...) if(vimshell_debug_fp) { fprintf(vimshell_debug_fp, a); fflush(vimshell_debug_fp); }
#else
#  define VERBOSEPRINTF(a...)
#endif

/*
 * These are the VIM keynames.
 */
#define VIMSHELL_KEY_DOWN	K_DOWN
#define VIMSHELL_KEY_UP		K_UP
#define VIMSHELL_KEY_LEFT	K_LEFT
#define VIMSHELL_KEY_RIGHT	K_RIGHT
#define VIMSHELL_KEY_HOME	K_HOME
#define VIMSHELL_KEY_BACKSPACE	K_BS
#define VIMSHELL_KEY_F1		K_F1
#define VIMSHELL_KEY_F2		K_F2
#define VIMSHELL_KEY_F3		K_F3
#define VIMSHELL_KEY_F4		K_F4
#define VIMSHELL_KEY_F5		K_F5
#define VIMSHELL_KEY_F6		K_F6
#define VIMSHELL_KEY_F7		K_F7
#define VIMSHELL_KEY_F8		K_F8
#define VIMSHELL_KEY_F9		K_F9
#define VIMSHELL_KEY_F10	K_F10
#define VIMSHELL_KEY_F11	K_F11
#define VIMSHELL_KEY_F12	K_F12
#define VIMSHELL_KEY_DC		K_DEL
#define VIMSHELL_KEY_END	K_END
#define VIMSHELL_KEY_IC		K_INS
#define VIMSHELL_KEY_NPAGE	K_PAGEDOWN
#define VIMSHELL_KEY_PPAGE	K_PAGEUP
#define VIMSHELL_KEY_K0		K_K0
#define VIMSHELL_KEY_K1		K_K1
#define VIMSHELL_KEY_K2		K_K2
#define VIMSHELL_KEY_K3		K_K3
#define VIMSHELL_KEY_K4		K_K4
#define VIMSHELL_KEY_K5		K_K5
#define VIMSHELL_KEY_K6		K_K6
#define VIMSHELL_KEY_K7		K_K7
#define VIMSHELL_KEY_K8		K_K8
#define VIMSHELL_KEY_K9		K_K9
#define VIMSHELL_KEY_KPLUS	K_KPLUS
#define VIMSHELL_KEY_KMINUS	K_KMINUS
#define VIMSHELL_KEY_KDIVIDE	K_KDIVIDE
#define VIMSHELL_KEY_KMULTIPLY	K_KMULTIPLY
#define VIMSHELL_KEY_KENTER	K_KENTER
#define VIMSHELL_KEY_KPOINT	K_KPOINT

static void terminal_ED(struct vim_shell_window *shell, int argc, char argv[20][20]);
static void terminal_CUP(struct vim_shell_window *shell, int argc, char argv[20][20]);
static int terminal_flush_output(struct vim_shell_window *shell);

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
 * Tabulation Clear (TBC)
 *
 * 9/11 6/7
 * CSI   g
 *
 * 	Clears a horizontal tab stop at cursor position.
 *
 * 	9/11 3/0 6/7
 * 	CSI   0   g
 *
 * 		Clears a horizontal tab stop at cursor position.
 *
 * 		9/11 3/3 6/7
 * 		CSI   3   g
 *
 * 			Clears all horizontal tab stops.
 */
static void terminal_TBC(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int param;

	if(argc==0)
	{
		param=0;
	}
	else if(argc>1)
	{
		ESCDEBUGPRINTF("%s: sequence error\n", __FUNCTION__);
		return;
	}

	// VIMSHELL TODO: DRINGEND! wenn argc==0 dann ist argv[0] NULL und wir crashen
	// hier ... passiert in einigen Funktionen, dank copy+paste! Super gmocht Tom!
	param=strtol(argv[0], NULL, 10);
	switch(param)
	{
		case 0:
			shell->tabline[shell->cursor_x]=0;
			break;
		case 3:
			memset(shell->tabline, 0, shell->size_x);
			break;
		default:
			ESCDEBUGPRINTF( "%s: sequence error (2)\n", __FUNCTION__);
	}
}

/*
 * CUB . Cursor Backward . Host to VT100 and VT100 to Host
 * ESC [ Pn D 	default value: 1
 *
 * The CUB sequence moves the active position to the left. The distance moved is
 * determined by the parameter. If the parameter value is zero or one, the active
 * position is moved one position to the left. If the parameter value is n, the active
 * position is moved n positions to the left. If an attempt is made to move the cursor
 * to the left of the left margin, the cursor stops at the left margin. Editor Function
 */
static void terminal_CUB(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int distance;

	if(argc==0)
	{
		distance=1;
	}
	else if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	distance=strtol(argv[0], NULL, 10);
	if(distance==0)
		distance=1;

	if(shell->cursor_x<distance)
		shell->cursor_x=0;
	else
		shell->cursor_x-=distance;
}

/*
 * CUU . Cursor Up . Host to VT100 and VT100 to Host
 * ESC [ Pn A 	default value: 1
 *
 * Moves the active position upward without altering the column position. The
 * number of lines moved is determined by the parameter. A parameter value of zero
 * or one moves the active position one line upward. A parameter value of n moves the
 * active position n lines upward. If an attempt is made to move the cursor above the
 * top margin, the cursor stops at the top margin. Editor Function
 */
static void terminal_CUU(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int distance;

	if(argc==0)
	{
		distance=1;
	}
	else if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	distance=strtol(argv[0], NULL, 10);
	if(distance==0)
		distance=1;

	if((int)shell->cursor_y-distance<shell->scroll_top_margin)
		shell->cursor_y=shell->scroll_top_margin;
	else
		shell->cursor_y-=distance;
}


/*
 * CUD . Cursor Down . Host to VT100 and VT100 to Host
 * ESC [ Pn B 	default value: 1
 *
 * The CUD sequence moves the active position downward without altering the column
 * position. The number of lines moved is determined by the parameter. If the parameter
 * value is zero or one, the active position is moved one line downward. If the parameter
 * value is n, the active position is moved n lines downward. In an attempt is made to
 * move the cursor below the bottom margin, the cursor stops at the bottom margin. Editor Function
 */
static void terminal_CUD(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int distance;

	if(argc==0)
	{
		distance=1;
	}
	else if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	distance=strtol(argv[0], NULL, 10);
	if(distance==0)
		distance=1;

	shell->cursor_y+=distance;
	if(shell->cursor_y+1>=shell->scroll_bottom_margin)
		shell->cursor_y=shell->scroll_bottom_margin;
}

/*
 * CUF . Cursor Forward . Host to VT100 and VT100 to Host
 * ESC [ Pn C 	default value: 1
 *
 * The CUF sequence moves the active position to the right. The distance moved is
 * determined by the parameter. A parameter value of zero or one moves the active
 * position one position to the right. A parameter value of n moves the active position n
 * positions to the right. If an attempt is made to move the cursor to the right
 * of the right margin, the cursor stops at the right margin. Editor Function
 */
static void terminal_CUF(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int distance;

	if(argc==0)
	{
		distance=1;
	}
	else if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	distance=strtol(argv[0], NULL, 10);
	if(distance==0)
		distance=1;

	shell->cursor_x+=distance;
	if(shell->cursor_x+1>=shell->size_x)
		shell->cursor_x=shell->size_x+1;
}

/*
 * SGR . Select Graphic Rendition
 * ESC [ Ps ; . . . ; Ps m 	default value: 0
 *
 * Invoke the graphic rendition specified by the parameter(s). All following characters
 * transmitted to the VT100 are rendered according to the parameter(s) until the next
 * occurrence of SGR. Format Effector
 *
 * Parameter 	Parameter Meaning
 * 0 	Attributes off
 * 1 	Bold or increased intensity
 * 4 	Underscore
 * 5 	Blink
 * 7 	Negative (reverse) image
 *
 * All other parameter values are ignored.
 *
 * With the Advanced Video Option, only one type of character attribute is possible as
 * determined by the cursor selection; in that case specifying either the underscore or
 * the reverse attribute will activate the currently selected attribute. (See cursor
 * selection in Chapter 1).
 */
static void terminal_SGR(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	if(argc==0)
	{
		shell->rendition=0;
		shell->fgcolor=VIMSHELL_COLOR_DEFAULT;  // default fgcolor
		shell->bgcolor=VIMSHELL_COLOR_DEFAULT;  // default bgcolor
	}
	else
	{
		int i;
		for(i=0;i<argc;i++)
		{
			int val;
			switch((val=strtol(argv[i], NULL, 10)))
			{
				case 0:
					shell->rendition=0;
					shell->fgcolor=VIMSHELL_COLOR_DEFAULT;
					shell->bgcolor=VIMSHELL_COLOR_DEFAULT;
					break;
				case 1:
					shell->rendition|=RENDITION_BOLD;
					break;
				case 2:
					shell->rendition|=RENDITION_DIM;
					break;
				case 4:
					shell->rendition|=RENDITION_UNDERSCORE;
					break;
				case 5:
					shell->rendition|=RENDITION_BLINK;
					break;
				case 7:
					shell->rendition|=RENDITION_NEGATIVE;
					break;
				case 8:
					shell->rendition|=RENDITION_HIDDEN;
					break;
				case 22:
					shell->rendition&=~RENDITION_BOLD;
					break;
				case 24:
					shell->rendition&=~RENDITION_UNDERSCORE;
					break;
				case 25:
					shell->rendition&=~RENDITION_BLINK;
					break;
				case 27:
					shell->rendition&=~RENDITION_NEGATIVE;
					break;
				default:
					if(val>=30 && val<=37)
						shell->fgcolor=val-30;
					else if(val>=40 && val<=47)
						shell->bgcolor=val-40;
					else if(val==39)
						shell->fgcolor=VIMSHELL_COLOR_DEFAULT; // default fgcolor
					else if(val==49)
						shell->bgcolor=VIMSHELL_COLOR_DEFAULT; // default bgcolor
					else
						ESCDEBUGPRINTF( "%s: unknown rendition %s\n",
								__FUNCTION__, argv[i]);
			}
		}
	}
	ESCDEBUGPRINTF("%s: rendition is now: %04x\n", __FUNCTION__, shell->rendition);
	ESCDEBUGPRINTF("%s: foreground color: %d, background color: %d\n", __FUNCTION__,
			shell->fgcolor, shell->bgcolor);
}

/*
 * Copy the screen contents to the alternate screen.
 */
static void terminal_backup_screen(struct vim_shell_window *shell)
{
	size_t len;
	if(shell->alt!=NULL)
	{
		ESCDEBUGPRINTF( "%s: WARNING: alternate screen taken\n", __FUNCTION__);
		vim_shell_free(shell->alt->winbuf);
		vim_shell_free(shell->alt->bgbuf);
		vim_shell_free(shell->alt->fgbuf);
		vim_shell_free(shell->alt->rendbuf);
		vim_shell_free(shell->alt->tabline);
		vim_shell_free(shell->alt->charset);
		vim_shell_free(shell->alt);
		shell->alt=NULL;
	}

	shell->alt=(struct vim_shell_window *)vim_shell_malloc(sizeof(struct vim_shell_window));
	if(shell->alt==NULL)
	{
		ESCDEBUGPRINTF( "%s: ERROR: unable to allocate a new screen\n", __FUNCTION__);
		return;
	}

	*(shell->alt)=*shell;

	len=shell->size_x*shell->size_y;
	shell->alt->winbuf=(uint8_t *)vim_shell_malloc(len);
	shell->alt->fgbuf=(uint8_t *)vim_shell_malloc(len);
	shell->alt->bgbuf=(uint8_t *)vim_shell_malloc(len);
	shell->alt->rendbuf=(uint8_t *)vim_shell_malloc(len);
	shell->alt->tabline=(uint8_t *)vim_shell_malloc(shell->size_x);
	shell->alt->charset=(uint8_t *)vim_shell_malloc(len);
	if(shell->alt->winbuf==NULL || shell->alt->winbuf==NULL || shell->alt->winbuf==NULL || shell->alt->winbuf==NULL ||
			shell->alt->charset==NULL || shell->alt->tabline==NULL)
	{
		ESCDEBUGPRINTF( "%s: ERROR: unable to allocate buffers\n", __FUNCTION__);
		if(shell->alt->winbuf) vim_shell_free(shell->alt->winbuf);
		if(shell->alt->fgbuf) vim_shell_free(shell->alt->fgbuf);
		if(shell->alt->bgbuf) vim_shell_free(shell->alt->bgbuf);
		if(shell->alt->rendbuf) vim_shell_free(shell->alt->rendbuf);
		if(shell->alt->tabline) vim_shell_free(shell->alt->tabline);
		if(shell->alt->charset) vim_shell_free(shell->alt->charset);
		vim_shell_free(shell->alt);
		shell->alt=NULL;
		return;
	}

	memcpy(shell->alt->winbuf, shell->winbuf, len);
	memcpy(shell->alt->fgbuf, shell->fgbuf, len);
	memcpy(shell->alt->bgbuf, shell->bgbuf, len);
	memcpy(shell->alt->rendbuf, shell->rendbuf, len);
	memcpy(shell->alt->charset, shell->charset, len);
	memcpy(shell->alt->tabline, shell->tabline, shell->size_x);
}

/*
 * Restore the alternate screen
 */
static void terminal_restore_screen(struct vim_shell_window *shell)
{
	struct vim_shell_window *alt;
	if(shell->alt==NULL)
	{
		ESCDEBUGPRINTF( "%s: WARNING: nothing to restore\n", __FUNCTION__);
		return;
	}

	vim_shell_free(shell->winbuf);
	vim_shell_free(shell->rendbuf);
	vim_shell_free(shell->tabline);
	vim_shell_free(shell->fgbuf);
	vim_shell_free(shell->bgbuf);
	vim_shell_free(shell->charset);

	alt=shell->alt;
	*shell=*(shell->alt);
	shell->alt=NULL;

	vim_shell_free(alt);

	/*
	 * Invalidate the shell so it will be fully redrawn
	 */
	shell->force_redraw=1;
}

/*
 * Erases the whole screen and homes the cursor.
 */
static void terminal_init_screen(struct vim_shell_window *shell)
{
	char ed_argv[20][20];

	strcpy(ed_argv[0], "2");

	/*
	 * Erase all of the display.
	 */
	terminal_ED(shell, 1, ed_argv);

	/*
	 * Cursor to HOME position.
	 */
	terminal_CUP(shell, 0, NULL);
}

static void terminal_mode(struct vim_shell_window *shell, int set, int argc, char argv[20][20])
{
	int i;

	for(i=0;i<argc;i++)
	{
		if(!strcmp(argv[i], "4"))
		{
			shell->insert_mode=set;
			ESCDEBUGPRINTF( "%s: insert mode: %d\n", __FUNCTION__, set);
		}
		else if(!strcmp(argv[i], "?1"))
		{
			shell->application_cursor_mode=set;
			ESCDEBUGPRINTF( "%s: application cursor mode: %d\n", __FUNCTION__, set);
		}
		else if(!strcmp(argv[i], "?5"))
		{
			ESCDEBUGPRINTF("%s: background dark/light mode ignored\n", __FUNCTION__);
		}
		else if(!strcmp(argv[i], "?6"))
		{
			ESCDEBUGPRINTF("%s: set terminal width ignored\n", __FUNCTION__);
		}
		else if(!strcmp(argv[i], "?7"))
		{
			shell->wraparound=set;
			ESCDEBUGPRINTF( "%s: wraparound: %d\n", __FUNCTION__, set);
		}
		else if(!strcmp(argv[i], "34") || !strcmp(argv[i], "?25"))
		{
			shell->cursor_visible=set;
			ESCDEBUGPRINTF( "%s: cursor visible: %d\n", __FUNCTION__, set);
		}
		else if(!strcmp(argv[i], "?4"))
		{
			ESCDEBUGPRINTF("%s: selection between smooth and jump scrolling ignored\n",__FUNCTION__);
		}
		else if(!strcmp(argv[i], "?1049") || !strcmp(argv[i], "?1047"))
		{
			if(set==1)
			{
				terminal_backup_screen(shell);
				terminal_init_screen(shell);
				ESCDEBUGPRINTF( "%s: terminal screen backed up.\n", __FUNCTION__);
			}
			else
			{
				terminal_restore_screen(shell);
				ESCDEBUGPRINTF( "%s: terminal screen restored from backup.\n", __FUNCTION__);
			}
		}
		else
		{
			ESCDEBUGPRINTF( "%s: unimplemented terminal mode: %s\n", __FUNCTION__, argv[i]);
		}
	}
}

/*
 * EL . Erase In Line
 * ESC [ Ps K 	default value: 0
 *
 * Erases some or all characters in the active line according to the parameter. Editor Function
 *
 * Parameter 	Parameter Meaning
 * 0 	Erase from the active position to the end of the line, inclusive (default)
 * 1 	Erase from the start of the line to the active position, inclusive
 * 2 	Erase all of the line, inclusive
 */
static void terminal_EL(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int i, size, pos;

	if(argc==0)
		i=0;
	else if(argc==1)
		i=strtol(argv[0], NULL, 10);
	else
	{
		/*
		 * Error in sequence
		 */
		ESCDEBUGPRINTF( "%s: error in sequence\n", __FUNCTION__);
		return;
	}

	size=shell->size_x*shell->size_y;
	pos=shell->cursor_y*shell->size_x+shell->cursor_x;
	if(i==0)
	{
		/*
		 * erase from the active position to the end of line
		 */

		memset(shell->winbuf+pos, ' ', shell->size_x-shell->cursor_x);
		memset(shell->fgbuf+pos,  VIMSHELL_COLOR_DEFAULT, shell->size_x-shell->cursor_x);
		memset(shell->bgbuf+pos,  VIMSHELL_COLOR_DEFAULT, shell->size_x-shell->cursor_x);
		memset(shell->rendbuf+pos, 0, shell->size_x-shell->cursor_x);
		memset(shell->charset+pos, 0, shell->size_x-shell->cursor_x);
		ESCDEBUGPRINTF( "%s: erase from active position to end of line\n", __FUNCTION__);

	}
	else if(i==1)
	{
		/*
		 * erase from start of the line to the active position, inclusive
		 */

		memset(shell->winbuf+shell->cursor_y*shell->size_x, ' ', shell->cursor_x);
		memset(shell->fgbuf+ shell->cursor_y*shell->size_x, VIMSHELL_COLOR_DEFAULT, shell->cursor_x);
		memset(shell->bgbuf+ shell->cursor_y*shell->size_x, VIMSHELL_COLOR_DEFAULT, shell->cursor_x);
		memset(shell->rendbuf+shell->cursor_y*shell->size_x, 0, shell->cursor_x);
		memset(shell->charset+shell->cursor_y*shell->size_x, 0, shell->cursor_x);
		ESCDEBUGPRINTF( "%s: erase from start of line to active position\n", __FUNCTION__);
	}
	else if(i==2)
	{
		/*
		 * Erase all of the line
		 */

		memset(shell->winbuf +shell->cursor_y*shell->size_x, ' ', shell->size_x);
		memset(shell->fgbuf  +shell->cursor_y*shell->size_x, VIMSHELL_COLOR_DEFAULT, shell->size_x);
		memset(shell->bgbuf  +shell->cursor_y*shell->size_x, VIMSHELL_COLOR_DEFAULT, shell->size_x);
		memset(shell->rendbuf+shell->cursor_y*shell->size_x, 0, shell->size_x);
		memset(shell->charset+shell->cursor_y*shell->size_x, 0, shell->size_x);
		ESCDEBUGPRINTF( "%s: erase all of the line\n", __FUNCTION__);
	}
	else
	{
		ESCDEBUGPRINTF( "%s: error in sequence (2)\n", __FUNCTION__);
		return;
	}
}

/*
 * ED . Erase In Display
 * ESC [ Ps J 	default value: 0
 *
 * This sequence erases some or all of the characters in the display according
 * to the parameter. Any complete line erased by this sequence will return that
 * line to single width mode. Editor Function
 *
 * Parameter 	Parameter Meaning
 * 0 	Erase from the active position to the end of the screen, inclusive (default)
 * 1 	Erase from start of the screen to the active position, inclusive
 * 2 	Erase all of the display . all lines are erased, changed to single-width, and the cursor does not move.
 */
static void terminal_ED(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int i, size, pos;

	if(argc==0)
		i=0;
	else if(argc==1)
		i=strtol(argv[0], NULL, 10);
	else
	{
		/*
		 * Error in sequence
		 */
		ESCDEBUGPRINTF( "%s: error in sequence\n", __FUNCTION__);
		return;
	}

	size=shell->size_x*shell->size_y;
	pos=shell->cursor_y*shell->size_x+shell->cursor_x;
	if(i==0)
	{
		/*
		 * erase from the active position to the end of screen
		 */

		memset(shell->winbuf+pos, ' ', size-pos);
		memset(shell->fgbuf+pos, VIMSHELL_COLOR_DEFAULT, size-pos);
		memset(shell->bgbuf+pos, VIMSHELL_COLOR_DEFAULT, size-pos);
		memset(shell->rendbuf+pos, 0, size-pos);
		memset(shell->charset+pos, 0, size-pos);
		ESCDEBUGPRINTF( "%s: erase from active position to end of screen\n", __FUNCTION__);

	}
	else if(i==1)
	{
		/*
		 * erase from start of the screen to the active position, inclusive
		 */

		memset(shell->winbuf, ' ', pos);
		memset(shell->fgbuf, VIMSHELL_COLOR_DEFAULT, pos);
		memset(shell->bgbuf, VIMSHELL_COLOR_DEFAULT, pos);
		memset(shell->rendbuf, 0, pos);
		memset(shell->charset, 0, pos);
		ESCDEBUGPRINTF( "%s: erase from start of screen to active position\n", __FUNCTION__);
	}
	else if(i==2)
	{
		/*
		 * Erase all of the display
		 */

		memset(shell->winbuf, ' ', size);
		memset(shell->fgbuf, VIMSHELL_COLOR_DEFAULT, size);
		memset(shell->bgbuf, VIMSHELL_COLOR_DEFAULT, size);
		memset(shell->rendbuf, 0, size);
		memset(shell->charset, 0, size);
		ESCDEBUGPRINTF( "%s: erase all of the display\n", __FUNCTION__);
	}
	else
	{
		ESCDEBUGPRINTF( "%s: error in sequence (2)\n", __FUNCTION__);
		return;
	}
}

/*
 * CUP . Cursor Position
 * ESC [ Pn ; Pn H 	default value: 1
 *
 * The CUP sequence moves the active position to the position specified by
 * the parameters. This sequence has two parameter values, the first specifying
 * the line position and the second specifying the column position. A parameter
 * value of zero or one for the first or second parameter moves the active
 * position to the first line or column in the display, respectively. The default
 * condition with no parameters present is equivalent to a cursor to home action.
 * In the VT100, this control behaves identically with its format effector
 * counterpart, HVP. Editor Function
 *
 * The numbering of lines depends on the state of the Origin Mode (DECOM).
 */
static void terminal_CUP(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	if(argc==0)
	{
		/*
		 * goto home position
		 */
		shell->cursor_x=0;
		shell->cursor_y=0;
	}
	else if(argc==2)
	{
		int x, y;

		y=strtol(argv[0], NULL, 10);
		x=strtol(argv[1], NULL, 10);
		if(x==0)
			x++;
		if(y==0)
			y++;

		shell->cursor_y=y-1;
		shell->cursor_x=x-1;

		// VIMSHELL TODO: if origin mode enabled, position at the
		//                top margin or so :)

		if(shell->cursor_x>=shell->size_x)
			shell->cursor_x=shell->size_x-1;
		if(shell->cursor_y>=shell->size_y)
			shell->cursor_y=shell->size_y-1;
	}
	else
	{
		/*
		 * Error in sequence
		 */
		ESCDEBUGPRINTF( "%s: error in sequence\n", __FUNCTION__);
		return;
	}
	ESCDEBUGPRINTF( "%s: moved to X = %d, Y = %d\n", __FUNCTION__, shell->cursor_x, shell->cursor_y);
}

/*
 * DECSTBM . Set Top and Bottom Margins (DEC Private)
 * ESC [ Pn; Pn r 	default values: see below
 *
 * This sequence sets the top and bottom margins to define the scrolling region.
 * The first parameter is the line number of the first line in the scrolling region; the
 * second parameter is the line number of the bottom line in the scrolling region. Default
 * is the entire screen (no margins). The minimum size of the scrolling region allowed is
 * two lines, i.e., the top margin must be less than the bottom margin. The cursor is placed
 * in the home position (see Origin Mode DECOM).
 */
static void terminal_DECSTBM(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	if(argc!=2)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	shell->scroll_top_margin=strtol(argv[0], NULL, 10)-1;
	shell->scroll_bottom_margin=strtol(argv[1], NULL, 10)-1;

	ESCDEBUGPRINTF( "%s: top margin = %d, bottom margin = %d\n", __FUNCTION__,
			shell->scroll_top_margin, shell->scroll_bottom_margin);

	if(shell->scroll_top_margin>=shell->scroll_bottom_margin)
	{
		ESCDEBUGPRINTF( "%s: scroll margin error %d >= %d\n", __FUNCTION__,
				shell->scroll_top_margin, shell->scroll_bottom_margin);

		shell->scroll_top_margin=0;
		shell->scroll_bottom_margin=shell->size_y-1;

		return;
	}

	// Move cursor into home position
	terminal_CUP(shell, 0, NULL);
}


// Status: 100%
static void terminal_scroll_up(struct vim_shell_window *shell)
{
	// VIMSHELL TODO: maintain a scrollback buffer? copy the first line into
	// a scrollback buffer.

	/*
	 * scroll up by moving up the screen buffer a whole line. blank out
	 * the last line.
	 */
	int len, from, to;

	ESCDEBUGPRINTF( "%s: done\n", __FUNCTION__);

	len=shell->scroll_bottom_margin-shell->scroll_top_margin;
	len*=shell->size_x;

	to=shell->scroll_top_margin*shell->size_x;
	from=shell->size_x+shell->scroll_top_margin*shell->size_x;

	memmove(shell->winbuf+to, shell->winbuf+from, len);
	memmove(shell->fgbuf+to, shell->fgbuf+from, len);
	memmove(shell->bgbuf+to, shell->bgbuf+from, len);
	memmove(shell->rendbuf+to, shell->rendbuf+from, len);
	memmove(shell->charset+to, shell->charset+from, len);

	memset(shell->winbuf+shell->scroll_bottom_margin*shell->size_x, ' ', shell->size_x);
	memset(shell->fgbuf+shell->scroll_bottom_margin*shell->size_x, VIMSHELL_COLOR_DEFAULT, shell->size_x);
	memset(shell->bgbuf+shell->scroll_bottom_margin*shell->size_x, VIMSHELL_COLOR_DEFAULT, shell->size_x);
	memset(shell->rendbuf+shell->scroll_bottom_margin*shell->size_x, 0, shell->size_x);
	memset(shell->charset+shell->scroll_bottom_margin*shell->size_x, 0, shell->size_x);
}

// Status: unknown
static void terminal_scroll_down(struct vim_shell_window *shell)
{
	/*
	 * scroll down by moving the screen buffer down a whole line, overwriting
	 * the last line. Then, blank out the first line.
	 */
	size_t len;
	int from, to;

	ESCDEBUGPRINTF( "%s: done\n", __FUNCTION__);

	len=shell->scroll_bottom_margin-shell->scroll_top_margin;
	len*=shell->size_x;

	to=shell->scroll_top_margin*shell->size_x+shell->size_x;
	from=shell->scroll_top_margin*shell->size_x;

	memmove(shell->winbuf+to, shell->winbuf+from, len);
	memmove(shell->fgbuf+to, shell->fgbuf+from, len);
	memmove(shell->bgbuf+to, shell->bgbuf+from, len);
	memmove(shell->rendbuf+to, shell->rendbuf+from, len);
	memmove(shell->charset+to, shell->charset+from, len);

	memset(shell->winbuf+from, ' ', shell->size_x);
	memset(shell->fgbuf+from, VIMSHELL_COLOR_DEFAULT, shell->size_x);
	memset(shell->bgbuf+from, VIMSHELL_COLOR_DEFAULT, shell->size_x);
	memset(shell->rendbuf+from, 0, shell->size_x);
	memset(shell->charset+from, 0, shell->size_x);
}

/*
 * Insert Line(s) (ANSI).
 * ESC [ Pn L
 * Inserts Pn lines at the cursor. If fewer than Pn lines remain from the current line to the
 * end of the scrolling region, the number of lines inserted is the lesser number. Lines within
 * the scrolling region at and below the cursor move down. Lines moved past the bottom margin
 * are lost. The cursor is reset to the first column. This sequence is ignored when the
 * cursor is outside the scrolling region.
 */
// Status: unknown
static void terminal_IL(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int lines, bak_top;
	if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	if(argc==0)
		lines=1;
	else
	{
		lines=strtol(argv[0], NULL, 10);
		if(lines==0)
			lines=1;
	}

	if(shell->scroll_bottom_margin-shell->cursor_y < lines)
		lines=shell->scroll_bottom_margin-shell->cursor_y;

	bak_top=shell->scroll_top_margin;

	ESCDEBUGPRINTF( "%s: inserted %d lines\n", __FUNCTION__, lines);

	/*
	 * Set a scrolling region around the part we want to move and scroll down 'lines' times.
	 */
	shell->scroll_top_margin=shell->cursor_y;
	while(lines--)
		terminal_scroll_down(shell);

	shell->scroll_top_margin=bak_top;

	shell->cursor_x=0;
}

/*
 * Delete Line(s) (ANSI).
 * ESC [ Pn M
 * Deletes Pn lines starting at the line with the cursor. If fewer than Pn lines remain from
 * the current line to the end of the scrolling region, the number of lines deleted is the
 * lesser number. As lines are deleted, lines within the scrolling region and below the cursor move
 * up, and blank lines are added at the bottom of the scrolling region. The cursor is reset to the
 * first column. This sequence is ignored when the cursor is outside the scrolling region.
 * VIMSHELL TODO last sentence
 */
// Status: unknown
static void terminal_DL(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int lines, bak_top;
	if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	if(argc==0)
		lines=1;
	else
	{
		lines=strtol(argv[0], NULL, 10);
		if(lines==0)
			lines=1;
	}

	if(shell->scroll_bottom_margin-shell->cursor_y < lines)
		lines=shell->scroll_bottom_margin-shell->cursor_y;

	bak_top=shell->scroll_top_margin;

	ESCDEBUGPRINTF( "%s: deleted %d lines\n", __FUNCTION__, lines);

	/*
	 * Set a scrolling region around the part we want to move and scroll up 'lines' times.
	 */
	shell->scroll_top_margin=shell->cursor_y;
	while(lines--)
		terminal_scroll_up(shell);

	shell->scroll_top_margin=bak_top;

	shell->cursor_x=0;
}

/*
 * Insert Characters (ANSI)
 * ESC [ Pn @
 * Insert Pn blank characters at the cursor position, with the character attributes set to
 * normal. The cursor does not move and remains at the beginning of the inserted blank characters.
 * A parameter of 0 or 1 inserts one blank character. Data on the line is shifted forward as
 * in character insertion.
 */
static void terminal_ICH(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int chars;
	int curpos;
	size_t len;
	if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	if(argc==0)
		chars=1;
	else
	{
		chars=strtol(argv[0], NULL, 10);
		if(chars==0)
			chars=1;
	}

	curpos=shell->cursor_y*shell->size_x+shell->cursor_x;
	len=shell->size_x-shell->cursor_x-1;

	ESCDEBUGPRINTF( "%s: inserted %d characters\n", __FUNCTION__, chars);

	while(chars--)
	{
		memmove(shell->winbuf+curpos+1, shell->winbuf+curpos, len);
		memmove(shell->fgbuf+curpos+1, shell->fgbuf+curpos, len);
		memmove(shell->bgbuf+curpos+1, shell->bgbuf+curpos, len);
		memmove(shell->rendbuf+curpos+1, shell->rendbuf+curpos, len);
		memmove(shell->charset+curpos+1, shell->charset+curpos, len);

		memset(shell->winbuf+curpos, ' ', 1);
		memset(shell->fgbuf+curpos, VIMSHELL_COLOR_DEFAULT, 1);
		memset(shell->bgbuf+curpos, VIMSHELL_COLOR_DEFAULT, 1);
		memset(shell->rendbuf+curpos, 0, 1);
		memset(shell->charset+curpos, 0, 1);
	}
}

/*
 * Delete Characters (ANSI)
 * ESC [ Pn P
 * Deletes Pn characters starting with the character at the cursor position. When a character
 * is deleted, all characters to the right of the cursor move to the left. This creates a space
 * character at the right margin for each character deleted. Character attributes move with the
 * characters. The spaces created at the end of the line have all their character attributes off.
 */
static void terminal_DCH(struct vim_shell_window *shell, int argc, char argv[20][20])
{
	int chars;
	int curpos;
	size_t len;
	if(argc>1)
	{
		ESCDEBUGPRINTF( "%s: sequence error\n", __FUNCTION__);
		return;
	}

	if(argc==0)
		chars=1;
	else
	{
		chars=strtol(argv[0], NULL, 10);
		if(chars==0)
			chars=1;
	}

	curpos=shell->cursor_y*shell->size_x+shell->cursor_x;
	len=shell->size_x-shell->cursor_x-1;

	ESCDEBUGPRINTF( "%s: deleted %d characters\n", __FUNCTION__, chars);

	while(chars--)
	{
		memmove(shell->winbuf+curpos, shell->winbuf+curpos+1, len);
		memmove(shell->fgbuf+curpos, shell->fgbuf+curpos+1, len);
		memmove(shell->bgbuf+curpos, shell->bgbuf+curpos+1, len);
		memmove(shell->rendbuf+curpos, shell->rendbuf+curpos+1, len);
		memmove(shell->charset+curpos, shell->charset+curpos+1, len);

		memset(shell->winbuf+curpos+len, ' ', 1);
		memset(shell->fgbuf+curpos+len, VIMSHELL_COLOR_DEFAULT, 1);
		memset(shell->bgbuf+curpos+len, VIMSHELL_COLOR_DEFAULT, 1);
		memset(shell->rendbuf+curpos+len, 0, 1);
		memset(shell->charset+curpos+len, 0, 1);
	}
}

static void terminal_BEL(struct vim_shell_window *shell)
{
	/*
	 * "Sound bell tone from keyboard."
	 * Sure.
	 */
}

static void terminal_BS(struct vim_shell_window *shell)
{
	/*
	 * "Move the cursor to the left one character position,
	 * unless it is at the left margin, in which case no action occurs."
	 */
	if(shell->cursor_x>0)
		shell->cursor_x--;
}

static void terminal_LF(struct vim_shell_window *shell)
{
	/*
	 * "This code causes a line feed or a new line operation."
	 */

	if(shell->just_wrapped_around==1)
	{
		/*
		 * Terminals which have the xenl capability ignore a linefeed after
		 * an auto margin wrap.
		 */
		VERBOSEPRINTF( "%s: ignored LF because of earlier wrap around\n",
				__FUNCTION__);
		return;
	}

	shell->cursor_y++;
	if(shell->cursor_y-1==shell->scroll_bottom_margin)
	{
		shell->cursor_y--;
		terminal_scroll_up(shell);
	}

	VERBOSEPRINTF( "%s: did LF, cursor is now at X = %u, Y = %u\n", __FUNCTION__,
			shell->cursor_x, shell->cursor_y);
}

static void terminal_CR(struct vim_shell_window *shell)
{
	/*
	 * "Move cursor to the left margin on the current line."
	 */
	if(shell->just_wrapped_around==1)
	{
		/*
		 * Terminals which have the xenl capability ignore a linefeed after
		 * an auto margin wrap.
		 */
		VERBOSEPRINTF( "%s: ignored CR because of earlier wrap around\n",
				__FUNCTION__);
		return;
	}
	shell->cursor_x=0;
	VERBOSEPRINTF( "%s: did CR, cursor is now at X = %u, Y = %u\n", __FUNCTION__,
			shell->cursor_x, shell->cursor_y);
}

/*
 * RI . Reverse Index
 * ESC M
 *
 * Move the active position to the same horizontal position on the preceding line. If the
 * active position is at the top margin, a scroll down is performed. Format Effector
 */
static void terminal_RI(struct vim_shell_window *shell)
{
	ESCDEBUGPRINTF( "%s: done\n", __FUNCTION__);
	if(shell->cursor_y==shell->scroll_top_margin)
		terminal_scroll_down(shell);
	else
		shell->cursor_y--;
}

/*
 * IND . Index
 * ESC D
 *
 * This sequence causes the active position to move downward one line without changing the
 * column position. If the active position is at the bottom margin, a scroll up is performed.
 * Format Effector
 */
static void terminal_IND(struct vim_shell_window *shell)
{
	ESCDEBUGPRINTF( "%s: done\n", __FUNCTION__);
	if(shell->cursor_y==shell->scroll_bottom_margin)
		terminal_scroll_up(shell);
	else
		shell->cursor_y++;
}

/*
 * ESC 7
 * ESC [s (ANSI)
 * Saves the current terminal rendering settings
 */
static void terminal_save_attributes(struct vim_shell_window *shell)
{
	shell->saved_cursor_x=shell->cursor_x;
	shell->saved_cursor_y=shell->cursor_y;
	shell->saved_rendition=shell->rendition;
	shell->saved_fgcolor=shell->fgcolor;
	shell->saved_bgcolor=shell->bgcolor;
	shell->saved_G0_charset=shell->G0_charset;
	shell->saved_G1_charset=shell->G1_charset;
	shell->saved_application_keypad_mode=shell->application_keypad_mode;
	shell->saved_application_cursor_mode=shell->application_cursor_mode;
	shell->saved_insert_mode=shell->insert_mode;
}

/*
 * ESC 8
 * ESC [u (ANSI)
 * Restores the current terminal rendering settings
 */
static void terminal_restore_attributes(struct vim_shell_window *shell)
{
	shell->cursor_x=shell->saved_cursor_x;
	shell->cursor_y=shell->saved_cursor_y;
	shell->rendition=shell->saved_rendition;
	shell->fgcolor=shell->saved_fgcolor;
	shell->bgcolor=shell->saved_bgcolor;
	shell->G0_charset=shell->saved_G0_charset;
	shell->G1_charset=shell->saved_G1_charset;
	shell->application_keypad_mode=shell->saved_application_keypad_mode;
	shell->application_cursor_mode=shell->saved_application_cursor_mode;
	shell->insert_mode=shell->saved_insert_mode;
}

#define ADVANCE_PSEQ { pseq++; if(*pseq==0) return; }

/*
 * Attempts to parse the escape sequence stored in shell->esc_sequence.
 * When it succeeds, it removes that escape sequence from the buffer and
 * possibly clears shell->in_esc_sequence when there are no characters left,
 * leaving ESC mode.
 */
static void terminal_parse_esc_sequence(struct vim_shell_window *shell)
{
	char *pseq;

	if(shell->in_esc_sequence==0 || shell->esc_sequence[0]!=033)
	{
		ESCDEBUGPRINTF( "%s: invalid esc sequence in esc buffer\n",
				__FUNCTION__);
		/*
		 * We would stick here forever, so flush the buffer
		 */
		shell->in_esc_sequence=0;
		terminal_flush_output(shell);

		return;
	}

	if(shell->in_esc_sequence==1)
	{
		ESCDEBUGPRINTF( "%s: not much in the buffer ...\n", __FUNCTION__);
		return;
	}

	shell->esc_sequence[shell->in_esc_sequence]=0;
	pseq=shell->esc_sequence+1;

	if(*pseq=='[')
	{
		/*
		 * Got a Control Sequence Introducer (CSI) = ESC [
		 * Parse Parameters.
		 */

		int argc=0;
		char argv[20][20];

		ADVANCE_PSEQ;

		memset(argv, 0, sizeof(argv));

		if((*pseq>='0' && *pseq<='9') || *pseq=='?' || *pseq==';')
		{
			int cont;
			do
			{
				cont=0;
				while((*pseq>='0' && *pseq<='9') || *pseq=='?')
				{
					char dummy[2];
					dummy[0]=*pseq;
					dummy[1]=0;
					strcat(argv[argc], dummy);
					ADVANCE_PSEQ;
				}
				if(argv[argc][0]==0)
				{
					strcpy(argv[argc], "0");
				}

				argc++;
				if(*pseq==';')
				{
					cont=1;
					ADVANCE_PSEQ;
				}
			} while(cont==1);
		}

#ifdef ESCDEBUG
		ESCDEBUGPRINTF("%s: sequence = '%s', argc = %d, ", __FUNCTION__, shell->esc_sequence, argc);
		{
			int i;
			for(i=0;i<argc;i++)
			{
				ESCDEBUGPRINTF("argv[%d] = '%s', ",i, argv[i]);
			}
		}
		ESCDEBUGPRINTF("\n");
#endif

		switch(*pseq)
		{
			case 'f':
			case 'H':
				terminal_CUP(shell, argc, argv);
				break;
			case 'J':
				terminal_ED(shell, argc, argv);
				break;
			case 'K':
				terminal_EL(shell, argc, argv);
				break;
			case 'C':
				terminal_CUF(shell, argc, argv);
				break;
			case 'l':
				terminal_mode(shell, 0, argc, argv);
				break;
			case 'h':
				terminal_mode(shell, 1, argc, argv);
				break;
			case 'm':
				terminal_SGR(shell, argc, argv);
				break;
			case 'r': // set scroll margins
				terminal_DECSTBM(shell, argc, argv);
				break;
			case 'B': // cursor down
				terminal_CUD(shell, argc, argv);
				break;
			case 'D': // cursor backward
				terminal_CUB(shell, argc, argv);
				break;
			case 'A': // cursor up
				terminal_CUU(shell, argc, argv);
				break;
			case 'M': // delete line
				terminal_DL(shell, argc, argv);
				break;
			case 'L': // insert line
				terminal_IL(shell, argc, argv);
				break;
			case '@': // insert characters
				terminal_ICH(shell, argc, argv);
				break;
			case 'P': // delete characters
				terminal_DCH(shell, argc, argv);
				break;
			case 'E': // new line
				terminal_CR(shell);
				terminal_LF(shell);
				break;
			case 's': // Save Cursor and attributes
				terminal_save_attributes(shell);
				break;
			case 'u': // Restore Cursor and attributes
				terminal_restore_attributes(shell);
				break;
			case 'g': // Tabulator clear
				terminal_TBC(shell, argc, argv);
				break;
			default:
				ESCDEBUGPRINTF( "%s: unimplemented CSI code: %c\n",
						__FUNCTION__, *pseq);
		}
	}
	else
	{
		ESCDEBUGPRINTF( "%s: sequence is (probably not yet complete) '%s'\n",
				__FUNCTION__, shell->esc_sequence);
		switch(*pseq)
		{
			case '#':
				ADVANCE_PSEQ;
				if(*pseq=='8') // fill screen with E's
				{
					memset(shell->winbuf, 'E', shell->size_x*shell->size_y);
				}
				break;
			case '(': // switch G0 charset
				ADVANCE_PSEQ;
				shell->G0_charset=*pseq;
				ESCDEBUGPRINTF( "%s: G0 character set is now: %c\n", __FUNCTION__, *pseq);
				break;
			case ')': // switch G1 charset
				ADVANCE_PSEQ;
				shell->G1_charset=*pseq;
				ESCDEBUGPRINTF( "%s: G1 character set is now: %c\n", __FUNCTION__, *pseq);
				break;
			case 'D': // index
				terminal_IND(shell);
				break;
			case 'M': // reverse index
				terminal_RI(shell);
				break;
			case '7': // Save Cursor
				terminal_save_attributes(shell);
				break;
			case '8': // Restore Cursor
				terminal_restore_attributes(shell);
				break;
			case '=': // Application Keypad Mode
				shell->application_keypad_mode=1;
				ESCDEBUGPRINTF( "%s: keypad switched to application mode\n", __FUNCTION__);
				break;
			case '>': // Numeric Keypad Mode
				shell->application_keypad_mode=0;
				ESCDEBUGPRINTF( "%s: keypad switched to numeric mode\n", __FUNCTION__);
				break;
			case 'H': // Set Horizontal Tab
				shell->tabline[shell->cursor_x]=1;
				break;
			case 'E': // NEL - Moves cursor to first position on next line. If cursor is
				  // at bottom margin, screen performs a scroll-up.
				terminal_IND(shell);
				shell->cursor_x=0;
				break;
			case ']': // This could be the beginning of the xterm title hack
				ADVANCE_PSEQ;
				if(*pseq=='1' || *pseq=='2' || *pseq=='0')
				{
					char title[50];
					ADVANCE_PSEQ;
					if(*pseq==';')
					{
						int end;
						title[0]=0;
						/*
						 * I know this loop is ugly but ADVANCE_PSEQ
						 * is a macro, sorry
						 */
						for(end=0;!end;)
						{
							ADVANCE_PSEQ;
							if(strlen(title)>sizeof(title)-2)
								end=1;
							if(*pseq==7)
								end=1;
							else
							{
								char dummy[2];
								dummy[0]=*pseq;
								dummy[1]=0;
								strcat(title, dummy);
							}
						}

						snprintf(shell->windowtitle, sizeof(shell->windowtitle),
								"%s", title);

						ESCDEBUGPRINTF( "%s: changing title to '%s'\n",
								__FUNCTION__, shell->windowtitle);
					}
					else
					{
						ESCDEBUGPRINTF( "%s: error in xterm title hack "
								"sequence: %c\n", __FUNCTION__, *pseq);
					}
				}
				else
				{
					ESCDEBUGPRINTF( "%s: unimplemented xterm title hack "
							"code: %d\n", __FUNCTION__, *pseq);
				}
				break;
			default:
				ESCDEBUGPRINTF( "%s: unimplemented esc code: %c\n",
						__FUNCTION__, *pseq);
		}
	}

	/*
	 * when we end up here, we successfully completed the sequence.
	 */
	shell->in_esc_sequence=0;
	terminal_flush_output(shell);

#ifdef ESCDEBUG
	pseq++;
	if(*pseq!=0)
	{
		ESCDEBUGPRINTF( "WARNING: %s: sequence is not over! left in buffer: '%s'\n",
				__FUNCTION__, pseq);
	}
#endif
}


/*
 * Main character write part.
 * This here runs most of the time, just writing the character to the
 * right cursor position.
 */
static void terminal_normal_char(struct vim_shell_window *shell, char input)
{
	int pos;
	uint8_t charset;

	shell->just_wrapped_around=0;
	/*
	 * If the cursor is currently in the 'virtual' column (that is the column
	 * after the physical line end), we wrap around now.
	 */
	if(shell->cursor_x==shell->size_x)
	{
		if(shell->wraparound==1)
		{
			terminal_CR(shell);
			terminal_LF(shell);
			shell->just_wrapped_around=1;
			VERBOSEPRINTF( "%s: auto margin - wrapped around!\n",__FUNCTION__);
		}
		else
		{
			shell->cursor_x--;
		}
	}

	/*
	 * write the character at its right position.
	 */
	if(shell->insert_mode!=0)
	{
		/*
		 * If insert mode is on, move all characters from the current position+1 to
		 * the end of the row. The last character on the row falls out.
		 * Fortunately, we already implemented this kind of operation :)
		 */
                terminal_ICH(shell, 0, NULL);
	}
	pos=shell->cursor_y*shell->size_x+shell->cursor_x;

	/*
	 * Select which character to display.
	 */
	if(shell->active_charset==1)
		charset=shell->G1_charset=='0' ? VIMSHELL_CHARSET_DRAWING : VIMSHELL_CHARSET_USASCII;
	else
		charset=shell->G0_charset=='0' ? VIMSHELL_CHARSET_DRAWING : VIMSHELL_CHARSET_USASCII;

	shell->winbuf[pos]=input;
	shell->fgbuf[pos]=shell->fgcolor;
	shell->bgbuf[pos]=shell->bgcolor;
	shell->rendbuf[pos]=shell->rendition;
	shell->charset[pos]=charset;
	VERBOSEPRINTF( "%s: writing char '%c' to position X = %u, Y = %u (col: 0x%02x,0x%02x)\n", __FUNCTION__,
			input, shell->cursor_x, shell->cursor_y, current_fg, current_bg);
	shell->cursor_x++;
}

/*
 * Here, all characters between 000 and 037 are processed. This is in a
 * separate function because control characters can be in the normal
 * flow of characters or in the middle of escape sequences.
 */
static void terminal_process_control_char(struct vim_shell_window *shell, char input)
{
	switch(input)
	{
		case 007: // BEL, Bell, 0x07
			terminal_BEL(shell);
			break;
		case 010: // BS, Backspace, 0x08
			terminal_BS(shell);
			break;
		case 011: // TAB
			{
				/*
				 * Move to the next tabstop or stop at the right margin if no
				 * tabstop found.
				 */
				int i, found;
				for(found=0, i=shell->cursor_x+1;i<shell->size_x && !found;i++)
				{
					if(shell->tabline[i]==1)
					{
						shell->cursor_x=i;
						found=1;
					}
				}
				if(found==0)
				{
					shell->cursor_x=shell->size_x-1;
				}
			}
			break;
		case 013:
		case 014:
		case 012: // LF, Line Feed, 0x0a, \n
			terminal_LF(shell);
			break;
		case 015: // CR, Carriage Return, 0x0d, \r
			terminal_CR(shell);
			break;
		case 016: // SO, Invoke G1 character set, as designated by SCS control sequence.
			shell->active_charset=1;
			break;
		case 017: // SI, Select G0 character set, as selected by ESC ( sequence.
			shell->active_charset=0;
			break;
		case 030:
		case 032: // CAN or SUB - cancel escape sequence
			shell->in_esc_sequence=0;
			// VIMSHELL TODO: display substitution character?
			ESCDEBUGPRINTF("%s: WARNING: possible source of rendering faults: "
					"substitution characters after CAN or SUB?\n", __FUNCTION__);
			break;
		case 033: // ESC, Escape
			/*
			 * Note: This also fulfills the requirement that a ESC occuring while processing
			 *       an escape sequence should restart the sequence.
			 */
			shell->in_esc_sequence=1;
			shell->esc_sequence[0]=033;
			break;
		default:
			ESCDEBUGPRINTF("%s: unimplemented control character: %u\n", __FUNCTION__, input);
			break;
	}
}

static void terminal_input_char(struct vim_shell_window *shell, char input)
{
	if(shell->in_esc_sequence==0)
	{
		if(input>=0 && input<=037)
		{
			/*
			 * A control character, process it
			 */
			terminal_process_control_char(shell, input);
		}
		else
		{
			// all normal characters are processed here
			terminal_normal_char(shell, input);
		}
	}
	else
	{
		if(input>=0 && input<=037)
		{
			/*
			 * That's right, control characters can appear even in
			 * the middle of escape sequences.
			 */
			terminal_process_control_char(shell, input);
			return;
		}

		/*
		 * Aha, we are right in the middle of an escape sequence.
		 * Add this character and attempt to parse the sequence.
		 */
		shell->esc_sequence[shell->in_esc_sequence]=input;
		shell->in_esc_sequence++;

		if(shell->in_esc_sequence>=sizeof(shell->esc_sequence))
		{
			/*
			 * about to overrun esc sequence buffer ...
			 * Should never happen, but still: kill the sequence, or we'd loop
			 * forever.
			 */

			shell->in_esc_sequence=0;
			terminal_flush_output(shell);
			return;
		}

		terminal_parse_esc_sequence(shell);
	}
}

/*
 * If we are ready, flush the outbuf into the shell.
 * CHECK: should always be called when shell->in_esc_sequence goes back to zero,
 *        so characters that are waiting for a sequence to become complete can be
 *        flushed out.
 */
static int terminal_flush_output(struct vim_shell_window *shell)
{
	if(/*shell->in_esc_sequence==0 && */ shell->outbuf_pos>0)
	{
		int len;
#ifdef ESCDEBUG
		ESCDEBUGPRINTF( "%s: sending:\n",__FUNCTION__);
		hexdump(vimshell_debug_fp, shell->outbuf, shell->outbuf_pos);
#endif
		len=write(shell->fd_master, shell->outbuf, shell->outbuf_pos);
		if(len<0)
		{
			ESCDEBUGPRINTF( "%s: ERROR: write failed: %s\n",
					__FUNCTION__,strerror(errno));
			shell->outbuf_pos=0;
			return -1;
		}

		if(shell->outbuf_pos!=len)
			memmove(shell->outbuf, shell->outbuf+len, len);
		shell->outbuf_pos-=len;

		return len;
	}
	return 0;
}

/*
 * Main Terminal processing method (VIM <- Shell).
 * Gets a buffer with input data from the shell, interprets it and updates
 * the shell window's windowbuffer accordingly.
 */
void vim_shell_terminal_input(struct vim_shell_window *shell, char *input, int len)
{
	int i;
	for(i=0;i<len;i++)
	{
		terminal_input_char(shell, input[i]);
	}
}

/*
 * Main Terminal output method (VIM -> Shell).
 * Translates the character 'c' into an appropriate escape sequence (if necessary)
 * and puts it into the out buffer.
 */
int vim_shell_terminal_output(struct vim_shell_window *shell, int c)
{
	char outbuf[50];
	size_t written;

	outbuf[1]=0;

	switch(c)
	{
		case VIMSHELL_KEY_BACKSPACE:
			sprintf(outbuf, "\177");
			ESCDEBUGPRINTF( "%s: key is backspace\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_UP:
			sprintf(outbuf, "\033%cA", shell->application_cursor_mode ? 'O' : '[');
			ESCDEBUGPRINTF( "%s: key is cursor up\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_DOWN:
			sprintf(outbuf, "\033%cB", shell->application_cursor_mode ? 'O' : '[');
			ESCDEBUGPRINTF( "%s: key is cursor down\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_LEFT:
			sprintf(outbuf, "\033%cD", shell->application_cursor_mode ? 'O' : '[');
			ESCDEBUGPRINTF( "%s: key is cursor left\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_RIGHT:
			sprintf(outbuf, "\033%cC", shell->application_cursor_mode ? 'O' : '[');
			ESCDEBUGPRINTF( "%s: key is cursor right\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_HOME:
			sprintf(outbuf, "\033[1~");
			ESCDEBUGPRINTF( "%s: key is home\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F1:
			sprintf(outbuf, "\033OP");
			ESCDEBUGPRINTF( "%s: key is F1\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F2:
			sprintf(outbuf, "\033OQ");
			ESCDEBUGPRINTF( "%s: key is F2\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F3:
			sprintf(outbuf, "\033OR");
			ESCDEBUGPRINTF( "%s: key is F3\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F4:
			sprintf(outbuf, "\033OS");
			ESCDEBUGPRINTF( "%s: key is F4\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F5:
			sprintf(outbuf, "\033[15~");
			ESCDEBUGPRINTF( "%s: key is F5\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F6:
			sprintf(outbuf, "\033[17~");
			ESCDEBUGPRINTF( "%s: key is F6\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F7:
			sprintf(outbuf, "\033[18~");
			ESCDEBUGPRINTF( "%s: key is F7\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F8:
			sprintf(outbuf, "\033[19~");
			ESCDEBUGPRINTF( "%s: key is F8\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F9:
			sprintf(outbuf, "\033[20~");
			ESCDEBUGPRINTF( "%s: key is F9\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F10:
			sprintf(outbuf, "\033[21~");
			ESCDEBUGPRINTF( "%s: key is F10\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F11:
			sprintf(outbuf, "\033[23~");
			ESCDEBUGPRINTF( "%s: key is F11\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_F12:
			sprintf(outbuf, "\033[24~");
			ESCDEBUGPRINTF( "%s: key is F12\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_DC:
			sprintf(outbuf, "\033[3~");
			ESCDEBUGPRINTF( "%s: key is delete character\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_END:
			sprintf(outbuf, "\033[4~");
			ESCDEBUGPRINTF( "%s: key is end\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_IC:
			sprintf(outbuf, "\033[2~");
			ESCDEBUGPRINTF( "%s: key is insert character\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_NPAGE:
			sprintf(outbuf, "\033[6~");
			ESCDEBUGPRINTF( "%s: key is page down\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_PPAGE:
			sprintf(outbuf, "\033[5~");
			ESCDEBUGPRINTF( "%s: key is page up\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K0:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Op" : "0");
			ESCDEBUGPRINTF( "%s: key is keypad 0\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K1:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Oq" : "1");
			ESCDEBUGPRINTF( "%s: key is keypad 1\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K2:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Or" : "2");
			ESCDEBUGPRINTF( "%s: key is keypad 2\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K3:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Os" : "3");
			ESCDEBUGPRINTF( "%s: key is keypad 3\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K4:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Ot" : "4");
			ESCDEBUGPRINTF( "%s: key is keypad 4\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K5:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Ou" : "5");
			ESCDEBUGPRINTF( "%s: key is keypad 5\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K6:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Ov" : "6");
			ESCDEBUGPRINTF( "%s: key is keypad 6\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K7:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Ow" : "7");
			ESCDEBUGPRINTF( "%s: key is keypad 7\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K8:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Ox" : "8");
			ESCDEBUGPRINTF( "%s: key is keypad 8\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_K9:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Oy" : "9");
			ESCDEBUGPRINTF( "%s: key is keypad 9\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_KPLUS:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Ok" : "+");
			ESCDEBUGPRINTF( "%s: key is keypad plus\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_KMINUS:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Om" : "-");
			ESCDEBUGPRINTF( "%s: key is keypad minus\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_KDIVIDE:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Oo" : "/");
			ESCDEBUGPRINTF( "%s: key is keypad divide\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_KMULTIPLY:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033Oj" : "*");
			ESCDEBUGPRINTF( "%s: key is keypad multiply\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_KENTER:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033OM" : "\015");
			ESCDEBUGPRINTF( "%s: key is keypad enter\n", __FUNCTION__);
			break;
		case VIMSHELL_KEY_KPOINT:
			sprintf(outbuf, "%s", shell->application_keypad_mode ? "\033On" : ".");
			ESCDEBUGPRINTF( "%s: key is keypad point\n", __FUNCTION__);
			break;
		default:
			outbuf[0]=(char)c;
	}

	if(shell->outbuf_pos+strlen(outbuf)>=sizeof(shell->outbuf))
	{
		written=sizeof(shell->outbuf)-shell->outbuf_pos;
		ESCDEBUGPRINTF( "%s: WARNING: prevented from overflowing the outbuf, help!\n",
				__FUNCTION__);
	}
	else
		written=strlen(outbuf);

	memcpy(shell->outbuf+shell->outbuf_pos, outbuf, written);
	shell->outbuf_pos+=written;

	if(terminal_flush_output(shell)<0)
		return -1;

	return written;
}
#endif
