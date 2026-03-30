/*
** GLW_IMP.C — IRIX/fxMesa backend for Quake2
**
** Replaces the X11/GLX version with a Voodoo/fxMesa implementation.
** No X11; keyboard input via stdin raw mode; no mouse.
**
** Required exports:
**   GLimp_Init, GLimp_Shutdown, GLimp_SetMode,
**   GLimp_BeginFrame, GLimp_EndFrame, GLimp_AppActivate
**   KBD_Init, KBD_Update, KBD_Close
**   RW_IN_Init, RW_IN_Shutdown, RW_IN_Commands,
**   RW_IN_Move, RW_IN_Frame, RW_IN_Activate
*/

#include <termios.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>

#include "../ref_gl/gl_local.h"
#include "../client/keys.h"
#include "../linux/rw_linux.h"

#include <GL/fxmesa.h>

/* ===================================================================
 * fxMesa context
 * =================================================================== */

static fxMesaContext fc = NULL;

/* ===================================================================
 * Terminal raw mode
 * =================================================================== */

static struct termios orig_termios;
static qboolean term_raw = false;

static void term_restore(void)
{
	if (term_raw) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
		term_raw = false;
	}
}

static void term_setraw(void)
{
	struct termios raw;

	if (tcgetattr(STDIN_FILENO, &orig_termios) < 0)
		return;
	raw = orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |=  (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN]  = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0)
		term_raw = true;
}

/* ===================================================================
 * Signal handling / atexit
 * =================================================================== */

static void signal_handler(int sig)
{
	fprintf(stderr, "Received signal %d, exiting...\n", sig);
	GLimp_Shutdown();
	_exit(0);
}

static void InitSig(void)
{
	signal(SIGHUP,  signal_handler);
	signal(SIGQUIT, signal_handler);
	signal(SIGILL,  signal_handler);
	signal(SIGTRAP, signal_handler);
	signal(SIGBUS,  signal_handler);
	signal(SIGFPE,  signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGINT,  signal_handler);
}

static void atexit_handler(void)
{
	term_restore();
	if (fc) {
		fxMesaDestroyContext(fc);
		fc = NULL;
	}
}

/* ===================================================================
 * GL implementation functions
 * =================================================================== */

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
	if (fc) {
		fxMesaDestroyContext(fc);
		fc = NULL;
	}
	term_restore();
}

int GLimp_Init(void *hinstance, void *wndproc)
{
	atexit(atexit_handler);
	InitSig();
	return true;
}

void GLimp_BeginFrame(float camera_separation)
{
}

void GLimp_EndFrame(void)
{
	glFlush();
	fxMesaSwapBuffers();
}

void GLimp_AppActivate(qboolean active)
{
}

/* ===================================================================
 * Palette extension (Voodoo hardware palette)
 * =================================================================== */

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

/* ===================================================================
 * Keyboard: stdin raw-mode input
 * =================================================================== */

static Key_Event_fp_t kbd_event_fp;

#define KEYQ_SIZE 64
static struct { int key; qboolean down; } keyq[KEYQ_SIZE];
static int keyq_head = 0, keyq_tail = 0;

static void keyq_push(int key, qboolean down)
{
	keyq[keyq_head].key  = key;
	keyq[keyq_head].down = down;
	keyq_head = (keyq_head + 1) & (KEYQ_SIZE - 1);
}

/*
 * Read available bytes from stdin and translate to Quake key events.
 * Handles VT100/ANSI escape sequences for arrow keys, F-keys, etc.
 */
static void parse_stdin(void)
{
	unsigned char buf[32];
	int n, i;
	fd_set fds;
	struct timeval tv;

	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	tv.tv_sec = 0;
	tv.tv_usec = 0;
	if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
		return;

	n = read(STDIN_FILENO, buf, sizeof(buf));
	if (n <= 0)
		return;

	i = 0;
	while (i < n) {
		unsigned char c = buf[i++];
		int key = 0;

		if (c == 0x1b) {
			/* Escape sequence or bare ESC */
			if (i < n && buf[i] == '[') {
				i++;
				if (i < n) {
					unsigned char code = buf[i++];
					if (code >= '1' && code <= '9') {
						if (i < n && buf[i] == '~') {
							/* ESC [ n ~ */
							i++;
							switch (code) {
							case '1': key = K_HOME;  break;
							case '2': key = K_INS;   break;
							case '3': key = K_DEL;   break;
							case '4': key = K_END;   break;
							case '5': key = K_PGUP;  break;
							case '6': key = K_PGDN;  break;
							}
						} else if (i < n && buf[i] >= '0' && buf[i] <= '9') {
							/* ESC [ nn ~ (two-digit, function keys) */
							unsigned char code2 = buf[i++];
							if (i < n && buf[i] == '~') {
								int num = (code - '0') * 10 + (code2 - '0');
								i++;
								switch (num) {
								case 11: key = K_F1;  break;
								case 12: key = K_F2;  break;
								case 13: key = K_F3;  break;
								case 14: key = K_F4;  break;
								case 15: key = K_F5;  break;
								case 17: key = K_F6;  break;
								case 18: key = K_F7;  break;
								case 19: key = K_F8;  break;
								case 20: key = K_F9;  break;
								case 21: key = K_F10; break;
								case 23: key = K_F11; break;
								case 24: key = K_F12; break;
								}
							}
						}
					} else {
						/* ESC [ letter */
						switch (code) {
						case 'A': key = K_UPARROW;    break;
						case 'B': key = K_DOWNARROW;  break;
						case 'C': key = K_RIGHTARROW; break;
						case 'D': key = K_LEFTARROW;  break;
						case 'H': key = K_HOME;       break;
						case 'F': key = K_END;        break;
						}
					}
				}
			} else if (i < n && buf[i] == 'O') {
				/* ESC O letter (SS3: F1-F4 and arrows on some terminals) */
				i++;
				if (i < n) {
					unsigned char code = buf[i++];
					switch (code) {
					case 'P': key = K_F1;         break;
					case 'Q': key = K_F2;         break;
					case 'R': key = K_F3;         break;
					case 'S': key = K_F4;         break;
					case 'A': key = K_UPARROW;    break;
					case 'B': key = K_DOWNARROW;  break;
					case 'C': key = K_RIGHTARROW; break;
					case 'D': key = K_LEFTARROW;  break;
					}
				}
			} else {
				/* bare ESC */
				key = K_ESCAPE;
			}
		} else if (c == 0x08 || c == 0x7f) {
			key = K_BACKSPACE;
		} else if (c == 0x09) {
			key = K_TAB;
		} else if (c == 0x0d || c == 0x0a) {
			key = K_ENTER;
		} else if (c == 0x00) {
			/* ignore NUL */
		} else if (c < 0x20) {
			/* Ctrl+letter: pass as K_CTRL */
			key = K_CTRL;
		} else if (c <= 0x7e) {
			/* printable ASCII — lowercase letters */
			key = (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : (int)c;
		}

		if (key) {
			keyq_push(key, true);
			keyq_push(key, false);
		}
	}
}

void KBD_Init(Key_Event_fp_t fp)
{
	kbd_event_fp = fp;
	term_setraw();
}

void KBD_Update(void)
{
	parse_stdin();
	while (keyq_tail != keyq_head) {
		kbd_event_fp(keyq[keyq_tail].key, keyq[keyq_tail].down);
		keyq_tail = (keyq_tail + 1) & (KEYQ_SIZE - 1);
	}
}

void KBD_Close(void)
{
	term_restore();
}

/* ===================================================================
 * Mouse: no-op stubs (no mouse support without X11)
 * =================================================================== */

void RW_IN_Init(in_state_t *in_state_p)  { }
void RW_IN_Shutdown(void)                { }
void RW_IN_Commands(void)                { }
void RW_IN_Move(usercmd_t *cmd)          { }
void RW_IN_Frame(void)                   { }
void RW_IN_Activate(void)                { }
