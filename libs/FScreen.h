#ifndef __XINERAMA_SUPPORT_H
#define __XINERAMA_SUPPORT_H

/* needs X11/Xlib.h and X11/Xutil.h */

enum
{
  FSCREEN_GLOBAL  = -1,
  FSCREEN_CURRENT = -2,
  FSCREEN_PRIMARY = -3,
  FSCREEN_XYPOS   = -4
};

enum
{
  FSCREEN_SPEC_GLOBAL = 'g',
  FSCREEN_SPEC_CURRENT = 'c',
  FSCREEN_SPEC_PRIMARY = 'p'
};

typedef union
{
  XEvent *mouse_ev;
  struct
  {
    int x;
    int y;
  } xypos;
} fscreen_scr_arg;

/* Control */
Bool FScreenIsEnabled(void);
void FScreenInit(Display *dpy);
void FScreenDisable(void);
void FScreenEnable(void);
/* Intended to be called by modules.  Simply pass in the parameter from the
 * config string sent by fvwm. */
void FScreenConfigureModule(char *args);
void FScreenDisableRandR(void);

int FScreenGetPrimaryScreen(void);
void FScreenSetPrimaryScreen(int scr);

/* Screen info */
Bool FScreenGetScrRect(
  fscreen_scr_arg *arg, int screen, int *x, int *y, int *w, int *h);
void FScreenGetResistanceRect(
  int wx, int wy, int ww, int wh, int *x0, int *y0, int *x1, int *y1);
Bool FScreenIsRectangleOnScreen(
  fscreen_scr_arg *arg, int screen, rectangle *rec);

/* Clipping/positioning */
int FScreenClipToScreen(
  fscreen_scr_arg *arg, int screen, int *x, int *y, int w, int h);
void FScreenCenterOnScreen(
  fscreen_scr_arg *arg, int screen, int *x, int *y, int w, int h);

/* Geometry management */
int FScreenGetScreenArgument(char *scr_spec, char default_screen);
int FScreenParseGeometryWithScreen(
  char *parsestring, int *x_return, int *y_return, unsigned int *width_return,
  unsigned int *height_return, int *screen_return);
int FScreenParseGeometry(
  char *parsestring, int *x_return, int *y_return, unsigned int *width_return,
  unsigned int *height_return);
int  FScreenGetGeometry(
  char *parsestring, int *x_return, int *y_return,
  int *width_return, int *height_return, XSizeHints *hints, int flags);

/* RandR support */
int  FScreenGetRandrEventType(void);
Bool FScreenHandleRandrEvent(
  XEvent *event, int *old_w, int *old_h, int *new_w, int *new_h);

#endif /* __XINERAMA_SUPPORT_H */
