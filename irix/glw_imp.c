/*
** glw_imp.c — IRIX fxMesa backend for Quake2 (MesaFX / GL_ARB_multitexture)
**
** When an X11 display is available a fullscreen override-redirect window is
** opened on the SGI desktop.  It covers the entire screen, captures keyboard
** and mouse input, and displays a status message so the user knows the game
** is running on the Voodoo card's separate monitor output.
**
** If XOpenDisplay fails (no X session) the old stdin-terminal path is used
** as a fallback so the binary still works in console-only setups.
*/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include "../ref_gl/gl_local.h"
#include "../client/keys.h"
#include "../linux/rw_linux.h"

#include <GL/fxmesa.h>

/* =========================================================================
** fxMesa context
** ========================================================================= */

static fxMesaContext fc = NULL;

/* =========================================================================
** X11 overlay state
** ========================================================================= */

static Display        *x11_dpy;
static Window          x11_win;
static GC              x11_gc;
static Cursor          x11_null_cursor;
static int             x11_screen;
static int             x11_win_w, x11_win_h;
static int             x11_cx,    x11_cy;      /* window centre */

static qboolean        x11_active;
static qboolean        x11_skip_warp;          /* ignore the next MotionNotify after warp */

static int             x11_mouse_dx;           /* accumulated relative delta this frame */
static int             x11_mouse_dy;

/* =========================================================================
** Terminal (stdin) fallback state
** ========================================================================= */

static in_state_t    *glw_in_state;
static Key_Event_fp_t glw_key_event_fp;
static int            glw_stdin_fd = 0;
static int            glw_stdin_flags = -1;
static qboolean       glw_terminal_active;
static qboolean       glw_terminal_available;
static struct termios glw_terminal_mode;
static unsigned char  glw_input_buffer[64];
static int            glw_input_length;
static unsigned       glw_escape_time;

#ifndef O_NONBLOCK
#define O_NONBLOCK FNDELAY
#endif

#define GLW_ESCAPE_TIMEOUT_MS 75u

/* =========================================================================
** KeySym → Quake2 key
** ========================================================================= */

static int X11_KeysymToQuake(KeySym ks)
{
	switch (ks)
	{
	case XK_Tab:            return K_TAB;
	case XK_Return:         return K_ENTER;
	case XK_Escape:         return K_ESCAPE;
	case XK_space:          return ' ';
	case XK_BackSpace:      return K_BACKSPACE;

	case XK_Up:             return K_UPARROW;
	case XK_Down:           return K_DOWNARROW;
	case XK_Left:           return K_LEFTARROW;
	case XK_Right:          return K_RIGHTARROW;

	case XK_Home:           return K_HOME;
	case XK_End:            return K_END;
	case XK_Prior:          return K_PGUP;
	case XK_Next:           return K_PGDN;
	case XK_Insert:         return K_INS;
	case XK_Delete:         return K_DEL;
	case XK_Pause:          return K_PAUSE;

	case XK_Shift_L:
	case XK_Shift_R:        return K_SHIFT;
	case XK_Control_L:
	case XK_Control_R:      return K_CTRL;
	case XK_Alt_L:
	case XK_Alt_R:
	case XK_Meta_L:
	case XK_Meta_R:         return K_ALT;

	case XK_F1:             return K_F1;
	case XK_F2:             return K_F2;
	case XK_F3:             return K_F3;
	case XK_F4:             return K_F4;
	case XK_F5:             return K_F5;
	case XK_F6:             return K_F6;
	case XK_F7:             return K_F7;
	case XK_F8:             return K_F8;
	case XK_F9:             return K_F9;
	case XK_F10:            return K_F10;
	case XK_F11:            return K_F11;
	case XK_F12:            return K_F12;

	case XK_KP_Enter:       return K_KP_ENTER;
	case XK_KP_Home:        return K_KP_HOME;
	case XK_KP_Up:          return K_KP_UPARROW;
	case XK_KP_Prior:       return K_KP_PGUP;
	case XK_KP_Left:        return K_KP_LEFTARROW;
	case XK_KP_Begin:       return K_KP_5;
	case XK_KP_Right:       return K_KP_RIGHTARROW;
	case XK_KP_End:         return K_KP_END;
	case XK_KP_Down:        return K_KP_DOWNARROW;
	case XK_KP_Next:        return K_KP_PGDN;
	case XK_KP_Insert:      return K_KP_INS;
	case XK_KP_Delete:      return K_KP_DEL;
	case XK_KP_Divide:      return K_KP_SLASH;
	case XK_KP_Subtract:    return K_KP_MINUS;
	case XK_KP_Add:         return K_KP_PLUS;

	default:
		if (ks >= ' ' && ks < 127)
		{
			if (ks >= 'A' && ks <= 'Z')
				ks = ks - 'A' + 'a';
			return (int)ks;
		}
		return 0;
	}
}

/* =========================================================================
** X11 overlay helpers
** ========================================================================= */

static Cursor X11_MakeNullCursor(void)
{
	Pixmap  pm;
	XColor  col;
	Cursor  cur;
	char    data[1] = { 0 };

	pm  = XCreateBitmapFromData(x11_dpy, x11_win, data, 1, 1);
	memset(&col, 0, sizeof(col));
	cur = XCreatePixmapCursor(x11_dpy, pm, pm, &col, &col, 0, 0);
	XFreePixmap(x11_dpy, pm);
	return cur;
}

static void X11_DrawOverlay(void)
{
	const char *l1 = "Quake 2  [ MesaFX / GL_ARB_multitexture ]";
	const char *l2 = "Rendering on Voodoo card  --  keyboard and mouse captured";
	const char *l3 = "Press ESC, then use the menu to quit";
	int         ty = x11_cy - 20;

	XSetForeground(x11_dpy, x11_gc, 0xA0A0A0);
	XFillRectangle(x11_dpy, x11_win, x11_gc, 0, 0, x11_win_w, x11_win_h);

	XSetForeground(x11_dpy, x11_gc, 0xFFFFFF);
	XDrawString(x11_dpy, x11_win, x11_gc,
	            x11_cx - (int)strlen(l1) * 3, ty,
	            l1, (int)strlen(l1));
	XDrawString(x11_dpy, x11_win, x11_gc,
	            x11_cx - (int)strlen(l2) * 3, ty + 20,
	            l2, (int)strlen(l2));
	XDrawString(x11_dpy, x11_win, x11_gc,
	            x11_cx - (int)strlen(l3) * 3, ty + 40,
	            l3, (int)strlen(l3));
	XFlush(x11_dpy);
}

/* =========================================================================
** Shutdown — idempotent; safe to call from signal handlers and normal exit
** ========================================================================= */

static void X11_Shutdown(void)
{
	if (!x11_active)
		return;

	x11_active = false;

	XUngrabPointer (x11_dpy, CurrentTime);
	XUngrabKeyboard(x11_dpy, CurrentTime);
	XFreeCursor    (x11_dpy, x11_null_cursor);
	XFreeGC        (x11_dpy, x11_gc);
	XDestroyWindow (x11_dpy, x11_win);
	XCloseDisplay  (x11_dpy);
	x11_dpy = NULL;
}

/* =========================================================================
** Terminal raw mode helpers (fallback when X11 unavailable)
** ========================================================================= */

static void GLW_TerminalRestore(void)
{
	if (glw_terminal_active)
	{
		tcsetattr(glw_stdin_fd, TCSANOW, &glw_terminal_mode);
		if (glw_stdin_flags != -1)
			fcntl(glw_stdin_fd, F_SETFL, glw_stdin_flags);
	}

	glw_terminal_active = false;
	glw_input_length    = 0;
	glw_escape_time     = 0;
}

static qboolean GLW_TerminalInit(void)
{
	struct termios raw_mode;

	glw_terminal_available = false;
	glw_terminal_active    = false;
	glw_input_length       = 0;
	glw_escape_time        = 0;

	if (!isatty(glw_stdin_fd))
		return false;

	if (tcgetattr(glw_stdin_fd, &glw_terminal_mode) == -1)
		return false;

	raw_mode          = glw_terminal_mode;
	raw_mode.c_lflag &= ~(ICANON | ECHO);
	raw_mode.c_iflag &= ~(ICRNL | IXON);
	raw_mode.c_cc[VMIN]  = 0;
	raw_mode.c_cc[VTIME] = 0;

	glw_stdin_flags = fcntl(glw_stdin_fd, F_GETFL, 0);
	if (glw_stdin_flags == -1)
		glw_stdin_flags = 0;

	if (tcsetattr(glw_stdin_fd, TCSANOW, &raw_mode) == -1)
		return false;

	if (fcntl(glw_stdin_fd, F_SETFL, glw_stdin_flags | O_NONBLOCK) == -1)
	{
		tcsetattr(glw_stdin_fd, TCSANOW, &glw_terminal_mode);
		return false;
	}

	glw_terminal_available = true;
	glw_terminal_active    = true;
	return true;
}

/* =========================================================================
** Signal handling / atexit
** ========================================================================= */

static void signal_handler(int sig)
{
	fprintf(stderr, "Received signal %d, shutting down.\n", sig);
	X11_Shutdown();
	GLimp_Shutdown();
	_exit(0);
}

static void InitSig(void)
{
	signal(SIGINT,  signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGQUIT, signal_handler);
	signal(SIGHUP,  signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGBUS,  signal_handler);
	signal(SIGFPE,  signal_handler);
	signal(SIGILL,  signal_handler);
	signal(SIGTRAP, signal_handler);
}

static void atexit_handler(void)
{
	X11_Shutdown();
	GLW_TerminalRestore();
	if (fc) {
		fxMesaDestroyContext(fc);
		fc = NULL;
	}
}

/* =========================================================================
** X11 initialisation
** ========================================================================= */

static qboolean X11_Init(void)
{
	const char          *display_name;
	XSetWindowAttributes swa;
	unsigned long        swa_mask;

	if (x11_active)
		return true;

	display_name = getenv("DISPLAY");
	if (!display_name || display_name[0] == '\0')
		display_name = ":0";

	x11_dpy = XOpenDisplay(display_name);
	if (!x11_dpy)
	{
		fprintf(stderr, "glw_imp: cannot open X display '%s'; using terminal input\n",
		        display_name);
		return false;
	}

	x11_screen = DefaultScreen(x11_dpy);
	x11_win_w  = DisplayWidth (x11_dpy, x11_screen);
	x11_win_h  = DisplayHeight(x11_dpy, x11_screen);
	x11_cx     = x11_win_w / 2;
	x11_cy     = x11_win_h / 2;

	/* Fullscreen override-redirect window — bypasses the window manager */
	swa.background_pixel  = 0xA0A0A0;
	swa.border_pixel      = 0;
	swa.override_redirect = True;
	swa.event_mask        = KeyPressMask | KeyReleaseMask |
	                        ButtonPressMask | ButtonReleaseMask |
	                        PointerMotionMask;
	swa_mask = CWBackPixel | CWBorderPixel | CWOverrideRedirect | CWEventMask;

	x11_win = XCreateWindow(
	    x11_dpy, RootWindow(x11_dpy, x11_screen),
	    0, 0, x11_win_w, x11_win_h, 0,
	    DefaultDepth(x11_dpy, x11_screen),
	    InputOutput,
	    DefaultVisual(x11_dpy, x11_screen),
	    swa_mask, &swa);

	XStoreName(x11_dpy, x11_win, "Quake2");
	x11_gc          = XCreateGC(x11_dpy, x11_win, 0, NULL);
	x11_null_cursor = X11_MakeNullCursor();
	XDefineCursor(x11_dpy, x11_win, x11_null_cursor);

	XMapRaised(x11_dpy, x11_win);
	XFlush(x11_dpy);

	if (XGrabKeyboard(x11_dpy, x11_win, True,
	                  GrabModeAsync, GrabModeAsync, CurrentTime) != GrabSuccess)
		fprintf(stderr, "glw_imp: XGrabKeyboard failed\n");

	if (XGrabPointer(x11_dpy, x11_win, True,
	                 ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
	                 GrabModeAsync, GrabModeAsync,
	                 x11_win, x11_null_cursor, CurrentTime) != GrabSuccess)
		fprintf(stderr, "glw_imp: XGrabPointer failed\n");

	/* Warp to centre; skip the resulting MotionNotify */
	XWarpPointer(x11_dpy, None, x11_win, 0, 0, 0, 0, x11_cx, x11_cy);
	x11_skip_warp = true;

	x11_mouse_dx = 0;
	x11_mouse_dy = 0;
	x11_active   = true;

	X11_DrawOverlay();

	fprintf(stderr, "glw_imp: X11 overlay active on %s (%dx%d)\n",
	        display_name, x11_win_w, x11_win_h);
	return true;
}

/* =========================================================================
** X11 per-frame event pump
** ========================================================================= */

static void X11_PumpEvents(void)
{
	XEvent ev;

	while (XPending(x11_dpy))
	{
		XNextEvent(x11_dpy, &ev);

		switch (ev.type)
		{
		case KeyPress:
		case KeyRelease:
		{
			KeySym ks = XLookupKeysym(&ev.xkey, 0);
			int    qk = X11_KeysymToQuake(ks);
			if (qk && glw_key_event_fp)
				glw_key_event_fp(qk, (ev.type == KeyPress) ? true : false);
			break;
		}

		case ButtonPress:
		case ButtonRelease:
		{
			qboolean down = (ev.type == ButtonPress);
			int      qk   = 0;
			switch (ev.xbutton.button)
			{
			case 1: qk = K_MOUSE1; break;
			case 2: qk = K_MOUSE3; break;  /* middle */
			case 3: qk = K_MOUSE2; break;
			case 4:
				if (down && glw_key_event_fp)
				{
					glw_key_event_fp(K_MWHEELUP, true);
					glw_key_event_fp(K_MWHEELUP, false);
				}
				break;
			case 5:
				if (down && glw_key_event_fp)
				{
					glw_key_event_fp(K_MWHEELDOWN, true);
					glw_key_event_fp(K_MWHEELDOWN, false);
				}
				break;
			}
			if (qk && glw_key_event_fp)
				glw_key_event_fp(qk, down);
			break;
		}

		case MotionNotify:
		{
			if (x11_skip_warp)
			{
				x11_skip_warp = false;
				break;
			}
			x11_mouse_dx += ev.xmotion.x - x11_cx;
			x11_mouse_dy += ev.xmotion.y - x11_cy;
			if (ev.xmotion.x != x11_cx || ev.xmotion.y != x11_cy)
			{
				XWarpPointer(x11_dpy, None, x11_win, 0, 0, 0, 0, x11_cx, x11_cy);
				x11_skip_warp = true;
			}
			break;
		}

		default:
			break;
		}
	}
}

/* =========================================================================
** Terminal fallback input helpers
** ========================================================================= */

static void GLW_SendKey(int key)
{
	if (!glw_key_event_fp || key <= 0 || key >= 256)
		return;

	glw_key_event_fp(key, true);
	glw_key_event_fp(key, false);
}

static int GLW_MapAsciiKey(int ch)
{
	switch (ch)
	{
	case '\r':
	case '\n': return K_ENTER;
	case '\t': return K_TAB;
	case 8:
	case 127:  return K_BACKSPACE;
	default:
		if (ch >= 32 && ch <= 126)
		{
			if (isupper(ch)) ch = tolower(ch);
			return ch;
		}
		break;
	}
	return 0;
}

static int GLW_ParseEscapeSequence(const unsigned char *buffer, int length,
                                   int *key, int *consumed)
{
	if (!buffer || length <= 0 || buffer[0] != 27)
		return 0;

	if (length == 1)
		return -1;

	if (length >= 3 && (buffer[1] == '[' || buffer[1] == 'O'))
	{
		switch (buffer[2])
		{
		case 'A': *key = K_UPARROW;    *consumed = 3; return 1;
		case 'B': *key = K_DOWNARROW;  *consumed = 3; return 1;
		case 'C': *key = K_RIGHTARROW; *consumed = 3; return 1;
		case 'D': *key = K_LEFTARROW;  *consumed = 3; return 1;
		case 'H': *key = K_HOME;       *consumed = 3; return 1;
		case 'F': *key = K_END;        *consumed = 3; return 1;
		default:  break;
		}
	}
	else if (length == 2 && (buffer[1] == '[' || buffer[1] == 'O'))
		return -1;

	if (length >= 4 && buffer[1] == '[' && buffer[3] == '~')
	{
		switch (buffer[2])
		{
		case '1': *key = K_HOME; *consumed = 4; return 1;
		case '2': *key = K_INS;  *consumed = 4; return 1;
		case '3': *key = K_DEL;  *consumed = 4; return 1;
		case '4': *key = K_END;  *consumed = 4; return 1;
		case '5': *key = K_PGUP; *consumed = 4; return 1;
		case '6': *key = K_PGDN; *consumed = 4; return 1;
		default:  break;
		}
	}
	else if (length == 3 && buffer[1] == '[' &&
	         buffer[2] >= '0' && buffer[2] <= '9')
		return -1;

	return 0;
}

static void GLW_ProcessInputBuffer(void)
{
	int offset = 0;

	while (offset < glw_input_length)
	{
		int key = 0, consumed = 0, parse_result;

		if (glw_input_buffer[offset] != 27)
		{
			key = GLW_MapAsciiKey(glw_input_buffer[offset]);
			if (key) GLW_SendKey(key);
			offset++;
			continue;
		}

		parse_result = GLW_ParseEscapeSequence(glw_input_buffer + offset,
		                                       glw_input_length - offset,
		                                       &key, &consumed);
		if (parse_result > 0)
		{
			GLW_SendKey(key);
			offset += consumed;
			continue;
		}

		if (parse_result < 0 &&
		    ((unsigned)Sys_Milliseconds() - glw_escape_time) < GLW_ESCAPE_TIMEOUT_MS)
			break;

		GLW_SendKey(K_ESCAPE);
		offset++;
	}

	if (offset > 0)
	{
		glw_input_length -= offset;
		if (glw_input_length > 0)
			memmove(glw_input_buffer, glw_input_buffer + offset, glw_input_length);
	}

	if (glw_input_length > 0 && glw_input_buffer[0] == 27 && glw_escape_time == 0)
		glw_escape_time = (unsigned)Sys_Milliseconds();
	else if (glw_input_length == 0)
		glw_escape_time = 0;
}

/* =========================================================================
** GL implementation functions
** ========================================================================= */

int GLimp_SetMode(int *pwidth, int *pheight, int mode, qboolean fullscreen)
{
	GLint attribs[6];

	ri.Con_Printf(PRINT_ALL, "Initializing OpenGL display\n");
	ri.Con_Printf(PRINT_ALL, "...setting mode %d:", mode);

	/* Voodoo1/SST1: fixed 640x480 — ignore requested mode */
	*pwidth  = 640;
	*pheight = 480;
	ri.Con_Printf(PRINT_ALL, " 640x480 (fixed for Voodoo1)\n");

	GLimp_Shutdown();

	attribs[0] = FXMESA_DOUBLEBUFFER;
	attribs[1] = FXMESA_DEPTH_SIZE;
	attribs[2] = 1;
	attribs[3] = FXMESA_ALPHA_SIZE;
	attribs[4] = 1;
	attribs[5] = FXMESA_NONE;

	fc = fxMesaCreateContext(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz, attribs);
	if (!fc) {
		ri.Con_Printf(PRINT_ALL, "fxMesaCreateContext failed\n");
		return rserr_invalid_mode;
	}

	fxMesaMakeCurrent(fc);
	ri.Vid_NewWindow(*pwidth, *pheight);
	return rserr_ok;
}

void GLimp_Shutdown(void)
{
	X11_Shutdown();
	GLW_TerminalRestore();
	if (fc) {
		fxMesaDestroyContext(fc);
		fc = NULL;
	}
}

int GLimp_Init(void *hinstance, void *wndproc)
{
	(void)hinstance;
	(void)wndproc;

	atexit(atexit_handler);
	InitSig();
	return true;
}

void GLimp_BeginFrame(float camera_separation)
{
	(void)camera_separation;
}

void GLimp_EndFrame(void)
{
	glFlush();
	fxMesaSwapBuffers();
}

void GLimp_AppActivate(qboolean active)
{
	(void)active;
}

/* =========================================================================
** Palette extension (Voodoo hardware palette)
** ========================================================================= */

extern void gl3DfxSetPaletteEXT(GLuint *pal);

void Fake_glColorTableEXT(GLenum target, GLenum internalformat,
                           GLsizei width, GLenum format, GLenum type,
                           const GLvoid *table)
{
	byte temptable[256][4];
	byte *intbl;
	int i;

	for (intbl = (byte *)table, i = 0; i < 256; i++) {
		temptable[i][2] = *intbl++;
		temptable[i][1] = *intbl++;
		temptable[i][0] = *intbl++;
		temptable[i][3] = 255;
	}
	gl3DfxSetPaletteEXT((GLuint *)temptable);
}

/* =========================================================================
** Keyboard
** ========================================================================= */

void KBD_Init(Key_Event_fp_t fp)
{
	glw_key_event_fp = fp;

	if (!X11_Init())
	{
		/* X11 unavailable — fall back to stdin terminal */
		GLW_TerminalInit();
	}
}

void KBD_Update(void)
{
	if (x11_active)
	{
		X11_PumpEvents();
		return;
	}

	/* Terminal fallback */
	{
		unsigned char read_buffer[32];
		int bytes_read;

		if (!glw_terminal_active)
			return;

		for (;;)
		{
			bytes_read = read(glw_stdin_fd, read_buffer, sizeof(read_buffer));
			if (bytes_read <= 0)
			{
				if (bytes_read == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				GLW_TerminalRestore();
				return;
			}

			if ((glw_input_length + bytes_read) > (int)sizeof(glw_input_buffer))
			{
				glw_input_length = 0;
				glw_escape_time  = 0;
			}

			memcpy(glw_input_buffer + glw_input_length, read_buffer, bytes_read);
			if (glw_input_length == 0 && read_buffer[0] == 27)
				glw_escape_time = (unsigned)Sys_Milliseconds();
			glw_input_length += bytes_read;
		}

		GLW_ProcessInputBuffer();
	}
}

void KBD_Close(void)
{
	X11_Shutdown();
	GLW_TerminalRestore();
	glw_key_event_fp = NULL;
}

/* =========================================================================
** Mouse / input
** ========================================================================= */

static void RW_IN_ForceCenterView_f(void)
{
	if (glw_in_state)
		glw_in_state->viewangles[PITCH] = 0;
}

static void RW_IN_MLookDown(void) { }

static void RW_IN_MLookUp(void)
{
	if (glw_in_state)
		glw_in_state->IN_CenterView_fp();
}

void RW_IN_Init(in_state_t *in_state_p)
{
	glw_in_state = in_state_p;

	ri.Cvar_Get("_windowed_mouse", "1", CVAR_ARCHIVE);
	ri.Cvar_Get("in_mouse",        "1", CVAR_ARCHIVE);
	ri.Cvar_Get("m_filter",        "0", 0);
	ri.Cvar_Get("freelook",        "1", 0);
	ri.Cvar_Get("lookstrafe",      "0", 0);
	ri.Cvar_Get("sensitivity",     "3", 0);
	ri.Cvar_Get("m_pitch",       "0.022", 0);
	ri.Cvar_Get("m_yaw",         "0.022", 0);
	ri.Cvar_Get("m_forward",       "1",   0);
	ri.Cvar_Get("m_side",          "0.8", 0);

	ri.Cmd_AddCommand("+mlook",           RW_IN_MLookDown);
	ri.Cmd_AddCommand("-mlook",           RW_IN_MLookUp);
	ri.Cmd_AddCommand("force_centerview", RW_IN_ForceCenterView_f);

	if (x11_active)
		ri.Con_Printf(PRINT_ALL,
		    "MesaFX input: X11 overlay active — keyboard and mouse captured.\n");
	else if (glw_terminal_available)
		ri.Con_Printf(PRINT_ALL,
		    "MesaFX input: terminal keyboard on stdin (no mouse support).\n");
	else
		ri.Con_Printf(PRINT_ALL,
		    "MesaFX input: no terminal — keyboard emulation unavailable.\n");
}

void RW_IN_Shutdown(void)
{
	ri.Cmd_RemoveCommand("+mlook");
	ri.Cmd_RemoveCommand("-mlook");
	ri.Cmd_RemoveCommand("force_centerview");
	glw_in_state = NULL;
}

void RW_IN_Commands(void)
{
}

void RW_IN_Move(usercmd_t *cmd)
{
	float sensitivity, m_yaw, m_pitch, m_forward, m_side;
	float mx, my;
	int   in_strafe;

	if (!x11_active || !glw_in_state)
		return;

	if (x11_mouse_dx == 0 && x11_mouse_dy == 0)
		return;

	sensitivity = ri.Cvar_Get("sensitivity", "3",     0)->value;
	m_yaw       = ri.Cvar_Get("m_yaw",       "0.022", 0)->value;
	m_pitch     = ri.Cvar_Get("m_pitch",     "0.022", 0)->value;
	m_forward   = ri.Cvar_Get("m_forward",   "1",     0)->value;
	m_side      = ri.Cvar_Get("m_side",      "0.8",   0)->value;
	in_strafe   = glw_in_state->in_strafe_state
	              ? *glw_in_state->in_strafe_state : 0;

	mx = (float)x11_mouse_dx * sensitivity;
	my = (float)x11_mouse_dy * sensitivity;
	x11_mouse_dx = 0;
	x11_mouse_dy = 0;

	if (in_strafe)
		cmd->sidemove += (short)(m_side * mx);
	else
		glw_in_state->viewangles[YAW] -= m_yaw * mx;

	if (ri.Cvar_Get("freelook", "1", 0)->value)
	{
		glw_in_state->viewangles[PITCH] += m_pitch * my;
		if (glw_in_state->viewangles[PITCH] >  89.0f)
			glw_in_state->viewangles[PITCH] =  89.0f;
		if (glw_in_state->viewangles[PITCH] < -89.0f)
			glw_in_state->viewangles[PITCH] = -89.0f;
	}
	else
	{
		cmd->forwardmove -= (short)(m_forward * my);
	}
}

void RW_IN_Frame(void)
{
}

void RW_IN_Activate(void)
{
}
