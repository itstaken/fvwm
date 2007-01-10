/* -*-c-*- */
/* This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "config.h"

#include <errno.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

/* for F_CMD_ARGS */
#include "fvwm/fvwm.h"
#include "execcontext.h"
/* end of for CMD_ARGS */

/*for debug message*/
#include "fvwm.h"
#include "misc.h"
/* end of for debug message */

/* for fFvwmInStartup */
#include "externs.h"

/* for get_current_read_file */
#include "read.h"
/* for busy cursor */
#include "cursor.h"

/* for Scr global */
#include "screen.h"

/* for module syncronous */
#include "libs/ftime.h"
#include "fvwmsignal.h"
#include "events.h"
#include "bindings.h"

/* for positive write */

#include "module_list.h"
#include "module_interface.h"
/*
 * Use POSIX behaviour if we can, otherwise use SysV instead
 * should this be here?
 */
#ifndef O_NONBLOCK
#  define O_NONBLOCK  O_NDELAY
#endif

#define MOD_NOGRABMASK(m) ((m)->xNoGrabMask)
#define MOD_SYNCMASK(m) ((m)->xSyncMask)
#define MOD_NEXT(m) ((m)->xnext)


typedef struct
{
	unsigned long *data;
	int size;
	int done;
} mqueue_object_type;




/* the linked list pointers to the first and last modules */
static fmodule *module_list;
static int num_modules = 0;

/*
 * static functions
 */
static fmodule *module_alloc(void);
static void module_free(fmodule *module);
static inline void module_insert(fmodule *module);
static inline void module_remove(fmodule *module);
static void KillModuleByName(char *name, char *alias);
static char *get_pipe_name(fmodule *module);
static void DeleteMessageQueueBuff(fmodule *module);

static inline void msg_mask_set(
	msg_masks_t *msg_mask, unsigned long m1, unsigned long m2);
static void set_message_mask(msg_masks_t *mask, unsigned long msg);


void module_init_list(void)
{
	DBUG("initModules", "initializing the module list header");
	/* the list is empty */
	module_list = NULL;

	return;
}

void module_kill_all(void)
{
	fmodule *module;

/*
 * this improves speed, but having a single remotion routine should
 * help in mainainability.. replace by module_remove calls?
 */
	for (module = module_list; module != NULL; module = MOD_NEXT(module))
	{
		module_free(module);
	}
	module_list = NULL;

	return;
}


static fmodule *module_alloc(void)
{
	fmodule *module;

	num_modules++;
	module = (fmodule *)safemalloc(sizeof(fmodule));
	MOD_SET_CMDLINE(module, 0);
	MOD_READFD(module) = -1;
	MOD_WRITEFD(module) = -1;
	fqueue_init(&MOD_PIPEQUEUE(module));
	msg_mask_set(&MOD_PIPEMASK(module), DEFAULT_MASK, DEFAULT_MASK);
	msg_mask_set(&MOD_NOGRABMASK(module), 0, 0);
	msg_mask_set(&MOD_SYNCMASK(module), 0, 0);
	MOD_NAME(module) = NULL;
	MOD_ALIAS(module) = NULL;
	MOD_NEXT(module) = NULL;

	return module;
}

/* closes the pipes and frees every data associated with a module record */
static void module_free(fmodule *module)
{
	if (module == NULL)
	{
		return;
	}
	if (MOD_WRITEFD(module) >= 0)
	{
		close(MOD_WRITEFD(module));
	}
	if (MOD_READFD(module) >= 0)
	{
		close(MOD_READFD(module));
	}
	if (MOD_NAME(module) != NULL)
	{
		free(MOD_NAME(module));
	}
	if (MOD_ALIAS(module) != NULL)
	{
		free(MOD_ALIAS(module));
	}
	while (!FQUEUE_IS_EMPTY(&(MOD_PIPEQUEUE(module))))
	{
		DeleteMessageQueueBuff(module);
	}
	num_modules--;
	free(module);

	return;
}

static inline void module_insert(fmodule *module)
{
	MOD_NEXT(module) = module_list;
	module_list = module;

	return;
}

static inline void module_remove(fmodule *module)
{
	if (module == NULL)
	{
		return;
	}
	if (module == module_list)
	{
		DBUG("module_remove", "Removing from module list");
		module_list = MOD_NEXT(module);
	}
	else
	{
		fmodule *parent;
		fmodule *current;

		/* find it*/
		for (
			current = MOD_NEXT(module_list), parent = module_list;
			current != NULL;
			parent = current, current = MOD_NEXT(current))
		{
			if (current == module)
			{
				break;
			}
		}
		/* remove from the list */
		if (current != NULL)
		{
			DBUG("module_remove", "Removing from module list");
			MOD_NEXT(parent) = MOD_NEXT(module);
		}
		else
		{
			fvwm_msg(
				ERR, "module_remove",
				"Tried to remove a not listed module!");

			return;
		}
	}
}


/*static*/ fmodule *do_execute_module(
	F_CMD_ARGS, Bool desperate, Bool do_listen_only)
{
	int fvwm_to_app[2], app_to_fvwm[2];
	int i, val, nargs = 0;
	char *cptr = NULL;
	char **args = NULL;
	char *arg1 = NULL;
	char arg2[20];
	char arg3[20];
	char arg5[20];
	char arg6[20];
	char *token;
	extern char *ModulePath;
	Window win;
	FvwmWindow * const fw = exc->w.fw;
	fmodule *module;

	fvwm_to_app[0] = -1;
	fvwm_to_app[1] = -1;
	app_to_fvwm[1] = -1;
	app_to_fvwm[0] = -1;
	args = (char **)safemalloc(7 * sizeof(char *));
	/* Olivier: Why ? */
	/*   if (eventp->type != KeyPress) */
	/*     UngrabEm(); */
	if (action == NULL)
	{
		goto err_exit;
	}
	if (fw)
	{
		win = FW_W(fw);
	}
	else
	{
		win = None;
	}
	action = GetNextToken(action, &cptr);
	if (!cptr)
	{
		goto err_exit;
	}
	arg1 = searchPath(ModulePath, cptr, EXECUTABLE_EXTENSION, X_OK);
	if (arg1 == NULL)
	{
		/* If this function is called in 'desparate' mode this means
		 * fvwm is trying a module name as a last resort.  In this case
		 * the error message is inappropriate because it was most
		 * likely a typo in a command, not a module name. */
		if (!desperate)
		{
			fvwm_msg(
				ERR, "executeModule",
				"No such module '%s' in ModulePath '%s'",
				cptr, ModulePath);
		}
		goto err_exit;
	}
#ifdef REMOVE_EXECUTABLE_EXTENSION
	{
		char *p;

		p = arg1 + strlen(arg1) - strlen(EXECUTABLE_EXTENSION);
		if (strcmp(p, EXECUTABLE_EXTENSION) == 0)
		{
			*p = 0;
		}
	}
#endif

	/* I want one-ended pipes, so I open two two-ended pipes,
	 * and close one end of each. I need one ended pipes so that
	 * I can detect when the module crashes/malfunctions */
	if (do_listen_only == True)
	{
		fvwm_to_app[0] = -1;
		fvwm_to_app[1] = -1;
	}
	else if (pipe(fvwm_to_app) != 0)
	{
		fvwm_msg(ERR, "executeModule", "Failed to open pipe");
		goto err_exit;
	}
	if (pipe(app_to_fvwm) != 0)
	{
		fvwm_msg(ERR, "executeModule", "Failed to open pipe2");
		goto err_exit;
	}
	if (
		fvwm_to_app[0] >= fvwmlib_max_fd ||
		fvwm_to_app[1] >= fvwmlib_max_fd ||
		app_to_fvwm[0] >= fvwmlib_max_fd ||
		app_to_fvwm[1] >= fvwmlib_max_fd)
	{
		fvwm_msg(ERR, "executeModule", "too many open fds");
		goto err_exit;
	}

	/* all ok, create the space and insert into the list */
	module = module_alloc();
	module_insert(module);

	MOD_NAME(module) = stripcpy(cptr);
	free(cptr);
	sprintf(arg2, "%d", app_to_fvwm[1]);
	sprintf(arg3, "%d", fvwm_to_app[0]);
	sprintf(arg5, "%lx", (unsigned long)win);
	sprintf(arg6, "%lx", (unsigned long)exc->w.wcontext);
	args[0] = arg1;
	args[1] = arg2;
	args[2] = arg3;
	args[3] = (char *)get_current_read_file();
	if (!args[3])
	{
		args[3] = "none";
	}
	args[4] = arg5;
	args[5] = arg6;
	for (nargs = 6; action = GetNextToken(action, &token), token; nargs++)
	{
		args = (char **)saferealloc(
			(void *)args, (nargs + 2) * sizeof(char *));
		args[nargs] = token;
		if (MOD_ALIAS(module) == NULL)
		{
			const char *ptr = skipModuleAliasToken(args[nargs]);

			if (ptr && *ptr == '\0')
			{
				MOD_ALIAS(module) = stripcpy(args[nargs]);
			}
		}
	}
	args[nargs] = NULL;

	/* Try vfork instead of fork. The man page says that vfork is better!
	 */
	/* Also, had to change exit to _exit() */
	/* Not everyone has vfork! */
	val = fork();
	if (val > 0)
	{
		/* This fork remains running fvwm */
		/* close appropriate descriptors from each pipe so
		 * that fvwm will be able to tell when the app dies */
		close(app_to_fvwm[1]);
		/* dont't care that this may be -1 */
		close(fvwm_to_app[0]);

		/* add these pipes to fvwm's active pipe list */
		MOD_WRITEFD(module) = fvwm_to_app[1];
		MOD_READFD(module) = app_to_fvwm[0];
		msg_mask_set(
			&MOD_PIPEMASK(module), DEFAULT_MASK, DEFAULT_MASK);
		free(arg1);
		if (DoingCommandLine)
		{
			/* add to the list of command line modules */
			DBUG("executeModule", "starting commandline module\n");
			MOD_SET_CMDLINE(module, 1);
		}

		/* make the PositiveWrite pipe non-blocking. Don't want to jam
		 * up fvwm because of an uncooperative module */
		if (MOD_WRITEFD(module) >= 0)
		{
			fcntl(MOD_WRITEFD(module), F_SETFL, O_NONBLOCK);
		}
		/* Mark the pipes close-on exec so other programs
		 * won`t inherit them */
		if (fcntl(MOD_READFD(module), F_SETFD, 1) == -1)
		{
			fvwm_msg(
				ERR, "executeModule",
				"module close-on-exec failed");
		}
		if (
			MOD_WRITEFD(module) >= 0 &&
			fcntl(MOD_WRITEFD(module), F_SETFD, 1) == -1)
		{
			fvwm_msg(
				ERR, "executeModule",
				"module close-on-exec failed");
		}
		for (i = 6; i < nargs; i++)
		{
			if (args[i] != 0)
			{
				free(args[i]);
			}
		}
	}
	else if (val ==0)
	{
		/* this is the child */
		/* this fork execs the module */
#ifdef FORK_CREATES_CHILD
		/* dont't care that this may be -1 */
		close(fvwm_to_app[1]);
		close(app_to_fvwm[0]);
#endif
		if (!Pdefault)
		{
			char visualid[32];
			char colormap[32];

			sprintf(
				visualid, "FVWM_VISUALID=%lx",
				XVisualIDFromVisual(Pvisual));
			flib_putenv("FVWM_VISUALID", visualid);
			sprintf(colormap, "FVWM_COLORMAP=%lx", Pcmap);
			flib_putenv("FVWM_COLORMAP", colormap);
		}
		else
		{
			flib_unsetenv("FVWM_VISUALID");
			flib_unsetenv("FVWM_COLORMAP");
		}

		/* Why is this execvp??  We've already searched the module
		 * path! */
		execvp(arg1,args);
		fvwm_msg(
			ERR, "executeModule", "Execution of module failed: %s",
			arg1);
		perror("");
		close(app_to_fvwm[1]);
		/* dont't care that this may be -1 */
		close(fvwm_to_app[0]);
#ifdef FORK_CREATES_CHILD
		exit(1);
#endif
	}
	else
	{
		fvwm_msg(ERR, "executeModule", "Fork failed");
		free(arg1);
		for (i = 6; i < nargs; i++)
		{
			if (args[i] != 0)
			{
				free(args[i]);
			}
		}
		free(args);
		module_remove(module);
		module_free(module);

		return NULL;
	}
	free(args);

	return module;

  err_exit:
	if (arg1 != NULL)
	{
		free(arg1);
	}
	if (cptr != NULL)
	{
		free(cptr);
	}
	if (args != NULL)
	{
		free(args);
	}
	/* dont't care that these may be -1 */
	close(fvwm_to_app[0]);
	close(fvwm_to_app[1]);
	close(app_to_fvwm[0]);
	close(app_to_fvwm[1]);

	return NULL;
}

fmodule *executeModuleDesperate(F_CMD_ARGS)
{
	return do_execute_module(F_PASS_ARGS, True, False);
}



void module_kill(fmodule *module)
{
	module_remove(module);
	module_free(module);
	if (fFvwmInStartup)
	{
		/* remove from list of command line modules */
		DBUG("module_kill", "ending command line module");
		MOD_IS_CMDLINE(module) = 0;
	}

	return;
}

/* void module_send(fmodule *module, unsigned long *ptr, int size) */
/* This used to be marked "fvwm_inline".  I removed this
   when I added the lockonsend logic.  The routine seems too big to
   want to inline.  dje 9/4/98 */
extern int myxgrabcount;                /* defined in libs/Grab.c */
extern char *ModuleUnlock;              /* defined in libs/Module.c */
void PositiveWrite(fmodule *module, unsigned long *ptr, int size)
{
	extern int moduleTimeout;
	msg_masks_t mask;

	if (ptr == NULL)
	{
		return;
	}
	if (MOD_WRITEFD(module) == -1)
	{
		return;
	}
	if (!IS_MESSAGE_IN_MASK(&(MOD_PIPEMASK(module)), ptr[1]))
	{
		return;
	}

	/* a dirty hack to prevent FvwmAnimate triggering during Recapture */
	/* would be better to send RecaptureStart and RecaptureEnd messages. */
	/* If module is lock on send for iconify message and it's an
	 * iconify event and server grabbed, then return */
	mask.m1 = (MOD_NOGRABMASK(module).m1 & MOD_SYNCMASK(module).m1);
	mask.m2 = (MOD_NOGRABMASK(module).m2 & MOD_SYNCMASK(module).m2);
	if (IS_MESSAGE_IN_MASK(&mask, ptr[1]) && myxgrabcount != 0)
	{
		return;
	}

	/* DV: This was once the AddToMessageQueue function.  Since it was only
	 * called once, put it in here for better performance. */
	{
		mqueue_object_type *c;

		c = (mqueue_object_type *)malloc(
			sizeof(mqueue_object_type) + size);
		if (c == NULL)
		{
			fvwm_msg(ERR, "PositiveWrite", "malloc failed\n");
			exit(1);
		}
		c->size = size;
		c->done = 0;
		c->data = (unsigned long *)(c + 1);
		memcpy((void*)c->data, (const void*)ptr, size);
		fqueue_add_at_end(&(MOD_PIPEQUEUE(module)), c);
	}

	/* dje, from afterstep, for FvwmAnimate, allows modules to sync with
	 * fvwm. this is disabled when the server is grabbed, otherwise
	 * deadlocks happen. M_LOCKONSEND has been replaced by a separated
	 * mask which defines on which messages the fvwm-to-module
	 * communication need to be lock on send. olicha Nov 13, 1999 */
	/* migo (19-Aug-2000): removed !myxgrabcount to sync M_DESTROY_WINDOW
	 */
	/* dv (06-Jul-2002): added the !myxgrabcount again.  Deadlocks *do*
	 * happen without it.  There must be another way to fix
	 * M_DESTROY_WINDOW handling in FvwmEvent. */
	/*if (IS_MESSAGE_IN_MASK(&(MOD_SYNCMASK(module)), ptr[1]))*/
	if (
		IS_MESSAGE_IN_MASK(
			&(MOD_SYNCMASK(module)), ptr[1]) && !myxgrabcount)
	{
		Window targetWindow;
		fd_set readSet;
		int channel = MOD_READFD(module);
		struct timeval timeout;

		FlushMessageQueue(module);
		if (MOD_READFD(module) < 0)
		{
			/* Module has died, break out */
			return;
		}

		do
		{
			int rc = 0;
			/*
			 * We give the read a long timeout; if the module
			 * fails to respond within this time then it deserves
			 * to be KILLED!
			 *
			 * NOTE: rather than impose an arbitrary timeout on the
			 * user, we will make this a configuration parameter.
			 */
			do
			{
				timeout.tv_sec = moduleTimeout;
				timeout.tv_usec = 0;
				FD_ZERO(&readSet);
				FD_SET(channel, &readSet);

				/* Wait for input to arrive on just one
				 * descriptor, with a timeout (fvwmSelect <= 0)
				 * or read() returning wrong size is bad news
				 */
				rc = fvwmSelect(
					channel + 1, &readSet, NULL, NULL,
					&timeout);
				/* retry if select() failed with EINTR */
			} while (rc < 0 && !isTerminated && (errno == EINTR));

			if ( isTerminated )
			{
				break;
			}

			if (rc <= 0 || read(channel, &targetWindow,
					    sizeof(targetWindow))
			    != sizeof(targetWindow))
			{
				char *name;

				name = get_pipe_name(module);
				/* Doh! Something has gone wrong - get rid of
				 * the offender! */
				fvwm_msg(ERR, "PositiveWrite",
					 "Failed to read descriptor from"
					 " '%s':\n"
					 "- data available=%c\n"
					 "- terminate signal=%c\n",
					 name,
					 (FD_ISSET(channel, &readSet) ?
					  'Y' : 'N'),
					 isTerminated ? 'Y' : 'N');
				module_kill(module);
				break;
			}

			/* Execute all messages from the module until UNLOCK is
			 * received N.B. This may cause recursion if a command
			 * results in a sync message to another module, which
			 * in turn may send a command that results in another
			 * sync message to this module.
			 * Hippo: I don't think this will cause deadlocks, but
			 * the third time we get here the first times UNLOCK
			 * will be read and then on returning up the third
			 * level UNLOCK will be read at the first level. This
			 * could be difficult to fix without turning queueing
			 * on.  Turning queueing on may be bad because it can
			 * be useful for modules to be able to inject commands
			 * from modules in a synchronous manner. e.g.
			 * FvwmIconMan can tell FvwmAnimate to do an animation
			 * when a window is de-iconified from the IconMan,
			 * queueing make s this happen too late. */
		}
		while (
			!HandleModuleInput(
				targetWindow, module, ModuleUnlockResponse,
				False));
	}

	return;
}

/* read input from a module and either execute it or queue it
 * returns True and does NOP if the module input matches the expect string */
Bool HandleModuleInput(Window w, fmodule *module, char *expect, Bool queue)
{
	char text[MAX_MODULE_INPUT_TEXT_LEN];
	unsigned long size;
	unsigned long cont;
	int n;

	/* Already read a (possibly NULL) window id from the pipe,
	 * Now read an fvwm bultin command line */
	n = read(MOD_READFD(module), &size, sizeof(size));
	if (n < sizeof(size))
	{
		fvwm_msg(
			ERR, "HandleModuleInput",
			"Fail to read (Module: %p, read: %i, size: %i)",
			module, n, (int)sizeof(size));
		module_kill(module);
		return False;
	}

	if (size > sizeof(text))
	{
		fvwm_msg(ERR, "HandleModuleInput",
			 "Module(%p) command is too big (%ld), limit is %d",
			 module, size, (int)sizeof(text));
		/* The rest of the output from this module is going to be
		 * scrambled so let's kill it rather than risk interpreting
		 * garbage */
		module_kill(module);
		return False;
	}

	n = read(MOD_READFD(module), text, size);
	if (n < size)
	{
		fvwm_msg(
			ERR, "HandleModuleInput",
			"Fail to read command (Module: %p, read: %i, size:"
			" %ld)", module, n, size);
		module_kill(module);
		return False;
	}
	text[n] = '\0';
	n = read(MOD_READFD(module), &cont, sizeof(cont));
	if (n < sizeof(cont))
	{
		fvwm_msg(ERR, "HandleModuleInput",
			 "Module %p, Size Problems (read: %d, size: %d)",
			 module, n, (int)sizeof(cont));
		module_kill(module);
		return False;
	}
	if (cont == 0)
	{
		/* this is documented as a valid way for a module to quit
		 * so let's not complain */
		module_kill(module);
	}
	if (strlen(text)>0)
	{
		if (expect && (strncasecmp(text, expect, strlen(expect)) == 0))
		{
			/* the module sent the expected string */
			return True;
		}

		if (queue)
		{
			AddToCommandQueue(w, module, text);
		}
		else
		{
			ExecuteModuleCommand(w, module, text);
		}
	}

	return False;
}

fmodule *module_get_next(fmodule *prev)
{
	if (prev == NULL)
	{
		return module_list;
	}
	else
	{
		return MOD_NEXT(prev);
	}
}

int module_count(void)
{
	return num_modules;
}

static void KillModuleByName(char *name, char *alias)
{
	fmodule *module;

	if (name == NULL)
	{
		return;
	}
	module = module_get_next(NULL);
	for (; module != NULL; module = module_get_next(module))
	{
		if (
			MOD_NAME(module) != NULL &&
			matchWildcards(name, MOD_NAME(module)) &&
			(!alias || (
				 MOD_ALIAS(module) &&
				 matchWildcards(alias, MOD_ALIAS(module)))))
		{
			module_kill(module);
		}
	}

	return;
}

static char *get_pipe_name(fmodule *module)
{
	char *name="";

	if (MOD_NAME(module) != NULL)
	{
		if (MOD_ALIAS(module) != NULL)
		{
			name = CatString3(
				MOD_NAME(module), " ", MOD_ALIAS(module));
		}
		else
		{
			name = MOD_NAME(module);
		}
	}
	else
	{
		name = CatString3("(null)", "", "");
	}

	return name;
}

/*
 * returns a pointer inside a string (just after the alias) if ok or NULL
 */
char *skipModuleAliasToken(const char *string)
{
#define is_valid_first_alias_char(ch) (isalpha(ch) || (ch) == '/')
#define is_valid_alias_char(ch) (is_valid_first_alias_char(ch)  \
				|| isalnum(ch) || (ch) == '-' || \
				(ch) == '.' || (ch) == '/')

	if (is_valid_first_alias_char(*string))
	{
		int len = 1;
		string++;
		while (*string && is_valid_alias_char(*string))
		{
			if (++len > MAX_MODULE_ALIAS_LEN)
			{
				return NULL;
			}
			string++;
		}
		return (char *)string;
	}

	return NULL;
#undef is_valid_first_alias_char
#undef is_valid_alias_char
}

/* message mask handling - does this belong here? */

static inline void msg_mask_set(
	msg_masks_t *msg_mask, unsigned long m1, unsigned long m2)
{
	msg_mask->m1 = m1;
	msg_mask->m2 = m2;

	return;
}

/*
 * Sets the mask to the specific value.  If M_EXTENDED_MSG is set in mask, the
 * function operates only on the extended messages, otherwise it operates only
 * on normal messages.
 */
static void set_message_mask(msg_masks_t *mask, unsigned long msg)
{
	if (msg & M_EXTENDED_MSG)
	{
		mask->m2 = (msg & ~M_EXTENDED_MSG);
	}
	else
	{
		mask->m1 = msg;
	}

	return;
}


/* message queues */

static void DeleteMessageQueueBuff(fmodule *module)
{
	mqueue_object_type *obj;

	if (fqueue_get_first(&(MOD_PIPEQUEUE(module)), (void **)&obj) == 1)
	{
		/* remove from queue */
		fqueue_remove_or_operate_from_front(
			&(MOD_PIPEQUEUE(module)), NULL, NULL, NULL, NULL);
		/* we don't need to free the obj->data here because it's in the
		 * same malloced block as the obj itself. */
		free(obj);
	}

	return;
}

void FlushMessageQueue(fmodule *module)
{
	extern int moduleTimeout;
	mqueue_object_type *obj;
	char *dptr;
	int a;

	if (module == NULL)
	{
		return;
	}

	while (fqueue_get_first(&(MOD_PIPEQUEUE(module)), (void **)&obj) == 1)
	{
		dptr = (char *)obj->data;
		while (obj->done < obj->size)
		{
			a = write(MOD_WRITEFD(module), &dptr[obj->done],
				  obj->size - obj->done);
			if (a >=0)
			{
				obj->done += a;
			}
			/* the write returns EWOULDBLOCK or EAGAIN if the pipe
			 * is full. (This is non-blocking I/O). SunOS returns
			 * EWOULDBLOCK, OSF/1 returns EAGAIN under these
			 * conditions. Hopefully other OSes return one of these
			 * values too. Solaris 2 doesn't seem to have a man
			 * page for write(2) (!) */
			else if (errno == EWOULDBLOCK || errno == EAGAIN)
			{
				fd_set writeSet;
				struct timeval timeout;
				int channel = MOD_WRITEFD(module);
				int rc = 0;

				do
				{
					/* Wait until the pipe accepts further
					 * input */
					timeout.tv_sec = moduleTimeout;
					timeout.tv_usec = 0;
					FD_ZERO(&writeSet);
					FD_SET(channel, &writeSet);
					rc = fvwmSelect(
						channel + 1, NULL, &writeSet,
						NULL, &timeout);
					/* retry if select() failed with EINTR
					 */
				} while ((rc < 0) && !isTerminated &&
					 (errno == EINTR));

				if ( isTerminated )
				{
					return;
				}
				if (!FD_ISSET(channel, &writeSet))
				{
					char *name;

					name = get_pipe_name(module);
					/* Doh! Something has gone wrong - get
					 * rid of the offender! */
					fvwm_msg(
						ERR, "FlushMessageQueue",
						"Failed to write descriptor to"
						" '%s':\n"
						"- select rc=%d\n"
						"- terminate signal=%c\n",
						name, rc, isTerminated ?
						'Y' : 'N');
					module_kill(module);
					return;
				}

				/* pipe accepts further input; continue */
				continue;
			}
			else if (errno != EINTR)
			{
				module_kill(module);
				return;
			}
		}
		DeleteMessageQueueBuff(module);
	}

	return;
}

void FlushAllMessageQueues(void)
{
	fmodule *module;

	module = module_get_next(NULL);
	for (; module != NULL; module = module_get_next(module))
	{
		FlushMessageQueue(module);
	}

	return;
}

/* empty, only here so that the signal handling initialization code is the
 * same for modules and fvwm  */
RETSIGTYPE DeadPipe(int sig)
{
	SIGNAL_RETURN;
}

void CMD_Module(F_CMD_ARGS)
{
	do_execute_module(F_PASS_ARGS, False, False);

 return;
}

void CMD_ModuleListenOnly(F_CMD_ARGS)
{
	do_execute_module(F_PASS_ARGS, False, True);

	return;
}

void CMD_KillModule(F_CMD_ARGS)
{
	char *name;
	char *alias = NULL;

	action = GetNextToken(action,&name);
	if (!name)
	{
		return;
	}

	GetNextToken(action, &alias);
	KillModuleByName(name, alias);
	free(name);
	if (alias)
	{
		free(alias);
	}
	return;
}

void CMD_ModuleSynchronous(F_CMD_ARGS)
{
	int sec = 0;
	char *next;
	char *token;
	char *expect = ModuleFinishedStartupResponse;
	fmodule *module;
	fd_set in_fdset;
	fd_set out_fdset;
	Window targetWindow;
	time_t start_time;
	Bool done = False;
	Bool need_ungrab = False;
	char *escape = NULL;
	XEvent tmpevent;

	if (!action)
	{
		return;
	}

	token = PeekToken(action, &next);
	if (StrEquals(token, "expect"))
	{
		token = PeekToken(next, &next);
		if (token)
		{
			expect = alloca(strlen(token) + 1);
			strcpy(expect, token);
		}
		action = next;
		token = PeekToken(action, &next);
	}
	if (token && StrEquals(token, "timeout"))
	{
		if (GetIntegerArguments(next, &next, &sec, 1) > 0 && sec > 0)
		{
			/* we have a delay, skip the number */
			action = next;
		}
		else
		{
			fvwm_msg(ERR, "executeModuleSync", "illegal timeout");
			return;
		}
	}

	if (!action)
	{
		/* no module name */
		return;
	}

	module = do_execute_module(F_PASS_ARGS, False, False);
	if (module == NULL)
	{
		/* executing the module failed, just return */
		return;
	}

	/* Busy cursor stuff */
	if (Scr.BusyCursor & BUSY_MODULESYNCHRONOUS)
	{
		if (GrabEm(CRS_WAIT, GRAB_BUSY))
			need_ungrab = True;
	}

	/* wait for module input */
	start_time = time(NULL);

	while (!done)
	{
		struct timeval timeout;
		int num_fd;

		/* A signal here could interrupt the select call. We would
		 * then need to restart our select, unless the signal was
		 * a "terminate" signal. Note that we need to reinitialise
		 * all of select's parameters after it has returned. */
		do
		{
			FD_ZERO(&in_fdset);
			FD_ZERO(&out_fdset);
			if (MOD_READFD(module) >= 0)
			{
				FD_SET(MOD_READFD(module), &in_fdset);
			}
			if (!FQUEUE_IS_EMPTY(&MOD_PIPEQUEUE(module)))
			{
				FD_SET(MOD_WRITEFD(module), &out_fdset);
			}
			timeout.tv_sec = 0;
			timeout.tv_usec = 1;
			num_fd = fvwmSelect(
				fvwmlib_max_fd, &in_fdset, &out_fdset, 0,
				&timeout);
		} while (num_fd < 0 && !isTerminated);

		/* Exit if we have received a "terminate" signal */
		if (isTerminated)
		{
			break;
		}

		if (num_fd > 0)
		{
			if ((MOD_READFD(module) >= 0) &&
			    FD_ISSET(MOD_READFD(module), &in_fdset))
			{
				/* Check for module input. */
				if (read(MOD_READFD(module), &targetWindow,
					 sizeof(Window)) > 0)
				{
					if (HandleModuleInput(
						    targetWindow, module,
						    expect, False))
					{
						/* we got the message we were
						 * waiting for */
						done = True;
					}
				}
				else
				{
					module_kill(module);
					done = True;
				}
			}

			if ((MOD_WRITEFD(module) >= 0) &&
			    FD_ISSET(MOD_WRITEFD(module), &out_fdset))
			{
				FlushMessageQueue(module);
			}
		}

		usleep(1000);
		if (difftime(time(NULL), start_time) >= sec && sec)
		{
			/* timeout */
			done = True;
		}

		/* Check for "escape function" */
		if (FPending(dpy) &&
		    FCheckMaskEvent(dpy, KeyPressMask, &tmpevent))
		{
			int context;
			XClassHint *class;
			char *name;

			context = GetContext(
				NULL, exc->w.fw, &tmpevent, &targetWindow);
			if (exc->w.fw != NULL)
			{
				class = &(exc->w.fw->class);
				name = exc->w.fw->name.name;
			}
			else
			{
				class = NULL;
				name = NULL;
			}
			escape = CheckBinding(
				Scr.AllBindings, STROKE_ARG(0)
				tmpevent.xkey.keycode, tmpevent.xkey.state,
				GetUnusedModifiers(), context, BIND_KEYPRESS,
				class, name);
			if (escape != NULL)
			{
				if (!strcasecmp(escape,"escapefunc"))
				{
					done = True;
				}
			}
		}
	} /* while */

	if (need_ungrab)
	{
		UngrabEm(GRAB_BUSY);
	}

	return;
}

/* mask handling - does this belong here? */

void CMD_set_mask(F_CMD_ARGS)
{
	unsigned long val;

	if (exc->m.module == NULL)
	{
		return;
	}
	if (!action || sscanf(action, "%lu", &val) != 1)
	{
		val = 0;
	}
	set_message_mask(&(MOD_PIPEMASK(exc->m.module)), (unsigned long)val);

	return;
}

void CMD_set_sync_mask(F_CMD_ARGS)
{
	unsigned long val;

	if (exc->m.module == NULL)
	{
		return;
	}
	if (!action || sscanf(action,"%lu",&val) != 1)
	{
		val = 0;
	}
	set_message_mask(&(MOD_SYNCMASK(exc->m.module)), (unsigned long)val);

	return;
}

void CMD_set_nograb_mask(F_CMD_ARGS)
{
	unsigned long val;

	if (exc->m.module == NULL)
	{
		return;
	}
	if (!action || sscanf(action,"%lu",&val) != 1)
	{
		val = 0;
	}
	set_message_mask(&(MOD_NOGRABMASK(exc->m.module)), (unsigned long)val);

	return;
}