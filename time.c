/*  Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2014, University of Amsterdam
			      VU University Amsterdam

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

    As a special exception, if you link this library with other files,
    compiled with a Free Software compiler, to produce an executable, this
    library does not by itself cause the resulting executable to be covered
    by the GNU General Public License. This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
*/

#define O_DEBUG 1			/* provides time:time_debug(+Level) */
//#define O_SAFE 1			/* extra safety checks */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <SWI-Stream.h>
#include <SWI-Prolog.h>
#include <error.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <math.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <string.h>
#include <errno.h>
#include <assert.h>

#ifdef SIGUSR2
#define SIG_TIME SIGUSR2
#else
#define SIG_TIME SIGALRM
#endif

#ifdef O_SAFE
#define __USE_GNU
#endif

#ifdef __WINDOWS__
#undef ETIMEDOUT
#endif
#include <pthread.h>

typedef enum
{ TIME_ABS,
  TIME_REL
} time_abs_rel;

#ifdef __WINDOWS__
#ifndef SIGALRM
#define SIGALRM 14
#endif
#endif

#ifdef _MSC_VER
#include <sys/timeb.h>
#include <malloc.h>

#ifndef WIN_PTHREADS
struct timeval
{ long tv_sec;
  long tv_usec;
};
#endif

struct timezone
{ int zone;
};

static int
gettimeofday(struct timeval *tv, struct timezone *tz)
{ struct timeb tb;

  ftime(&tb);
  tv->tv_sec  = (long)tb.time;
  tv->tv_usec = tb.millitm * 1000;

  return 0;
}


#else /*_MSC_VER*/

#include <time.h>
#include <sys/time.h>

#endif /*_MSC_VER*/

#ifdef O_DEBUG
static int debuglevel = 0;
#define DEBUG(n, g) if ( debuglevel >= n ) g

static foreign_t
pl_time_debug(term_t n)
{ return PL_get_integer(n, &debuglevel);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
glibc defines backtrace() and friends to  print the calling context. For
debugging this is just great,  as   the  problem  generally appear after
generating an exception.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef HAVE_EXECINFO_H
#define BACKTRACE 1

#if BACKTRACE
#include <execinfo.h>
#include <string.h>

static void
print_trace (void)
{ void *array[100];
  size_t size;
  char **strings;
  size_t i;

  size = backtrace(array, sizeof(array)/sizeof(void *));
  strings = backtrace_symbols(array, size);

#ifdef _REENTRANT
  Sdprintf("on_alarm() Prolog-context [thread %d]:\n", PL_thread_self());
#else
  Sdprintf("on_alarm() Prolog-context:\n");
#endif
  PL_action(PL_ACTION_BACKTRACE, 3);

  Sdprintf("on_alarm() C-context:\n");

  for(i = 0; i < size; i++)
  { if ( !strstr(strings[i], "checkData") )
      Sdprintf("\t[%d] %s\n", i, strings[i]);
  }

  free(strings);
}
#endif /*BACKTRACE*/
#endif /*HAVE_EXECINFO_H*/
#else /*O_DEBUG*/
#define DEBUG(n, g) ((void)0)
#endif /*O_DEBUG*/

static void	on_alarm(int sig);

static module_t	   MODULE_user;
static atom_t	   ATOM_remove;
static atom_t	   ATOM_install;
static atom_t	   ATOM_done;
static atom_t	   ATOM_next;
static atom_t	   ATOM_scheduled;
static functor_t   FUNCTOR_module2;
static functor_t   FUNCTOR_alarm1;
static functor_t   FUNCTOR_alarm4;
static predicate_t PREDICATE_call1;

#define EV_MAGIC	1920299187	/* Random magic number */

#define EV_DONE		0x0001		/* Handled this one */
#define EV_REMOVE	0x0002		/* Automatically remove */
#define EV_FIRED	0x0004		/* Windows: got this one */
#define EV_NOINSTALL	0x0008		/* Only allocate; do not install */

typedef struct event
{ record_t	 goal;			/* Thing to call */
  module_t	 module;		/* Module to call in */
  struct event  *next;			/* linked list for current */
  struct event  *previous;		/* idem */
  unsigned long  flags;			/* misc flags */
  long		 magic;			/* validate magic */
  struct timeval at;			/* Time to deliver */
  pthread_t	 thread_id;		/* Thread to call in */
  int		 pl_thread_id;		/* Prolog thread ID */
} event, *Event;

typedef void (*handler_t)(int);

typedef struct
{ Event first;				/* first in list */
  Event scheduled;			/* The one we scheduled for */
  int   stop;				/* stop alarm-loop */
} schedule;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond   = PTHREAD_COND_INITIALIZER;
static int scheduler_running = FALSE;	/* is scheduler running? */
static pthread_t scheduler;		/* thread id of scheduler */

#define LOCK()   pthread_mutex_lock(&mutex)
#define UNLOCK() pthread_mutex_unlock(&mutex)

static schedule the_schedule = {0};	/* the schedule */
#define TheSchedule() (&the_schedule)	/* current schedule */

int signal_function_set = FALSE;	/* signal function is set */
static handler_t signal_function;	/* Current signal function */

static int removeEvent(Event ev);

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Allocate the event, maintaining a time-sorted list of scheduled events.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static Event
allocEvent()
{ Event ev = malloc(sizeof(*ev));

  if ( !ev )
  { pl_error(NULL, 0, NULL, ERR_ERRNO, errno, "allocate", "memory", 0);
    return NULL;
  }

  memset(ev, 0, sizeof(*ev));
  ev->magic = EV_MAGIC;

  return ev;
}


static void
setTimeEventAbs(Event ev, double t)
{ struct timeval tv;

  gettimeofday(&tv, NULL);
  tv.tv_usec = (long)((t-floor(t))*1000000);
  tv.tv_sec  = (long)t;

  ev->at = tv;
}


static void
setTimeEvent(Event ev, double t)
{ struct timeval tv;

  gettimeofday(&tv, NULL);
  tv.tv_usec += (long)((t-floor(t))*1000000);
  tv.tv_sec  += (long)t;
  if ( tv.tv_usec >= 1000000 )
  { tv.tv_usec -= 1000000;
    tv.tv_sec++;
  }

  ev->at = tv;
}



static int
insertEvent(Event ev)
{ schedule *sched = TheSchedule();
  Event e;

  DEBUG(1, Sdprintf("insertEvent(%d.%06d)\n", ev->at.tv_sec, ev->at.tv_usec));

  for(e = sched->first; e; e = e->next)
  { struct timeval d;

    if ( e == ev )
      return ERR_PERMISSION;		/* already scheduled */

    d.tv_sec  = ev->at.tv_sec  - e->at.tv_sec;
    d.tv_usec = ev->at.tv_usec - e->at.tv_usec;
    if ( d.tv_usec < 0 )
    { d.tv_sec--;
      d.tv_usec += 1000000;
    }

    if ( d.tv_sec < 0 )			/* new must be before e */
    { ev->next = e;
      ev->previous = e->previous;
      if ( e->previous )
      { e->previous->next = ev;
      } else
      { assert(sched->first == e);
	sched->first = ev;
      }
      e->previous = ev;

      return TRUE;
    } else
    { if ( e->next )
	continue;

      ev->previous = e;			/* end of the list */
      e->next = ev;

      return TRUE;
    }
  }

  sched->first = ev;			/* the very first one */
  return TRUE;
}


static void
unlinkEvent(Event ev)
{ schedule *sched = TheSchedule();

  if ( sched->scheduled == ev )
    sched->scheduled = NULL;

  if ( ev->previous )
    ev->previous->next = ev->next;
  else
    sched->first = ev->next;

  if ( ev->next )
    ev->next->previous = ev->previous;

  ev->next = ev->previous = NULL;	/* in case it's reused */
}


static void
freeEvent(Event ev)
{ unlinkEvent(ev);

  if ( ev->goal )
    PL_erase(ev->goal);

  ev->magic = 0;

  free(ev);
}


static void
cleanupHandler()
{ if ( signal_function_set )
  { signal_function_set = FALSE;
    PL_signal(SIG_TIME, signal_function);
  }
}


static void
installHandler()
{ if ( !signal_function_set )
  { signal_function = PL_signal(SIG_TIME|PL_SIGSYNC, on_alarm);
    signal_function_set = TRUE;
  }
}


static int
cleanup(int rc, void *arg)
{ Event ev;
  schedule *sched = TheSchedule();

  while( (ev=sched->first) )
  { removeEvent(ev);
  }

  cleanupHandler();

  if ( scheduler_running )
  { sched->stop = TRUE;
    pthread_cond_signal(&cond);

    pthread_join(scheduler, NULL);
    scheduler_running = FALSE;
  }

  return 0;
}


static void
cleanup_thread(void *data)
{ schedule *sched = TheSchedule();

  (void)data;

  if ( sched->first )
  { Event ev, next;
    pthread_t self = pthread_self();

    LOCK();
    for( ev=sched->first; ev; ev=next )
    { next = ev->next;

      if ( pthread_equal(self, ev->thread_id) )
      { DEBUG(1, Sdprintf("[%d] removing alarm %ld at exit\n",
			  PL_thread_self(), (long)(intptr_t)ev));
	if ( sched->scheduled == ev )
	  ev->flags |= EV_DONE;
	freeEvent(ev);
      }
    }
    UNLOCK();

    pthread_cond_signal(&cond);
  }
}


static Event
nextEvent(schedule *sched)
{ Event ev;

  for(ev=sched->first; ev; ev = ev->next)
  { if ( ev->flags & (EV_DONE|EV_FIRED) )
      continue;

    return ev;
  }

  return NULL;
}


typedef struct
{ int		*bits;
  size_t	size;
  size_t	high;
} bitvector;

#define BITSPERINT (8*sizeof(int))

static int
set_bit(bitvector *v, size_t bit)
{ size_t offset = bit/BITSPERINT;
  int bi = bit%BITSPERINT;

  while ( offset >= v->size )
  { size_t osize = v->size * sizeof(int);
    int *newbits = realloc(v->bits, osize*2);

    if ( !newbits )
      return FALSE;
    memset((char*)newbits+osize, 0, osize);
    v->bits  = newbits;
    v->size *= 2;
  }

  while ( bit > v->high )		/* TBD: zero entire ints */
  { size_t ho = v->high/BITSPERINT;
    int    b  = v->high%BITSPERINT;

    v->bits[ho] &= ~(1<<(b-1));
    v->high++;
  }

  v->bits[offset] |= 1<<(bi-1);
  return TRUE;
}


static int
is_set(bitvector *v, size_t bit)
{ if ( bit <= v->high )
  { size_t offset = bit/BITSPERINT;
    int bi = bit%BITSPERINT;

    return (v->bits[offset] & (1<<(bi-1))) != 0;
  }

  return FALSE;
}


static void *
alarm_loop(void * closure)
{ schedule *sched = TheSchedule();
  bitvector signalled;

  signalled.size = 4;
  signalled.bits = malloc(signalled.size*sizeof(int));
  signalled.high = 0;

  pthread_mutex_lock(&mutex);		/* for condition variable */
  DEBUG(1, Sdprintf("Iterating alarm_loop()\n"));

  while( !sched->stop )
  { Event ev = nextEvent(sched);
    struct timeval now;

    signalled.high = 0;
    gettimeofday(&now, NULL);

    for(; ev; ev = ev->next)
    { struct timeval left;

      left.tv_sec  = ev->at.tv_sec  - now.tv_sec;
      left.tv_usec = ev->at.tv_usec - now.tv_usec;
      if ( left.tv_usec < 0 )
      { left.tv_sec--;
	left.tv_usec += 1000000;
      }

      if ( left.tv_sec < 0 ||
	   (left.tv_sec == 0 && left.tv_usec == 0) )
      { if ( !is_set(&signalled, ev->pl_thread_id) )
	{ DEBUG(1, Sdprintf("Signalling (left = %ld) %d ...\n",
			    (long)left.tv_sec,
			    ev->pl_thread_id));
	  set_bit(&signalled, ev->pl_thread_id);
#ifdef __WINDOWS__
	  PL_thread_raise(ev->pl_thread_id, SIG_TIME);
#else
	  pthread_kill(ev->thread_id, SIG_TIME);
#endif
	}
      } else
	break;
    }

    if ( ev )
    { int rc;
      struct timespec timeout;
      timeout.tv_sec  = ev->at.tv_sec;
      timeout.tv_nsec = ev->at.tv_usec*1000;

    retry_timed_wait:
      DEBUG(1, Sdprintf("Waiting ...\n"));
      rc = pthread_cond_timedwait(&cond, &mutex, &timeout);

      switch( rc )
      { case ETIMEDOUT:
	case 0:
	  continue;
	case EINTR:
	  goto retry_timed_wait;
	default:
	  Sdprintf("alarm/4: pthread_cond_timedwait(): %d (%s)\n",
		   rc, strerror(rc));
	  assert(0);
      }
    } else
    { int rc;

    retry_wait:
      DEBUG(1, Sdprintf("No waiting events\n"));
      rc = pthread_cond_wait(&cond, &mutex);
      switch(rc)
      { case EINTR:
	  goto retry_wait;
	case 0:
	  continue;
	default:
	  Sdprintf("alarm/4: pthread_cond_timedwait(): %d (%s)\n",
		   rc, strerror(rc));
	  assert(0);
      }
    }
  }

  return NULL;
}


static void
on_alarm(int sig)
{ Event ev;
  schedule *sched = TheSchedule();
  pthread_t self = pthread_self();

  DEBUG(1, Sdprintf("Signal received in %d\n",
		    PL_thread_self()));
#ifdef BACKTRACE
  DEBUG(10, print_trace());
#endif

  for(;;)
  { struct timeval now;
    term_t goal = 0;
    module_t module = NULL;

    gettimeofday(&now, NULL);

    LOCK();
    for(ev = sched->first; ev; ev=ev->next)
    { struct timeval left;

      assert(ev->magic == EV_MAGIC);

      if ( (ev->flags & (EV_DONE|EV_FIRED)) ||
	   !pthread_equal(self, ev->thread_id) )
	continue;

      left.tv_sec  = ev->at.tv_sec - now.tv_sec;
      left.tv_usec = ev->at.tv_usec - now.tv_usec;
      if ( left.tv_usec < 0 )
      { left.tv_sec--;
	left.tv_usec += 1000000;
      }

      if ( left.tv_sec < 0 ||
	   (left.tv_sec == 0 && left.tv_usec == 0) )
      { DEBUG(1, Sdprintf("Calling event\n"));
	ev->flags |= EV_DONE;
	module = ev->module;
	goal = PL_new_term_ref();
	PL_recorded(ev->goal, goal);

	if ( ev->flags & EV_REMOVE )
	  freeEvent(ev);
	break;
      }
    }
    UNLOCK();

    if ( goal )
    { PL_call_predicate(module,
			PL_Q_PASS_EXCEPTION,
			PREDICATE_call1,
			goal);
    } else
      break;
  }

  DEBUG(1, Sdprintf("Processed pending events; signalling scheduler\n"));
  pthread_cond_signal(&cond);
}


static int
installEvent(Event ev)
{ int rc;

  ev->thread_id = pthread_self();
  ev->pl_thread_id = PL_thread_self();

  LOCK();
  if ( !scheduler_running )
  { pthread_attr_t attr;

    TheSchedule()->stop = FALSE;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8192);
    rc = pthread_create(&scheduler, &attr, alarm_loop, NULL);
    pthread_attr_destroy(&attr);

    if ( rc != 0 )
    { UNLOCK();
      return pl_error("alarm", 4, "Failed to start schedule thread",
		      ERR_ERRNO, rc);
    }

    DEBUG(1, Sdprintf("Started scheduler thread\n"));
    scheduler_running = TRUE;
  }

  rc = insertEvent(ev);
  UNLOCK();

  if ( rc )
    pthread_cond_signal(&cond);

  return rc;
}


static int
uninstallEvent(Event ev)
{ LOCK();
  if ( TheSchedule()->scheduled == ev )
    ev->flags |= EV_DONE;
  unlinkEvent(ev);
  ev->flags &= ~(EV_FIRED|EV_DONE);
  UNLOCK();

  pthread_cond_signal(&cond);

  return TRUE;
}


static int
removeEvent(Event ev)
{ LOCK();
  if ( TheSchedule()->scheduled == ev )
    ev->flags |= EV_DONE;
  freeEvent(ev);
  UNLOCK();

  pthread_cond_signal(&cond);

  return TRUE;
}


		 /*******************************
		 *	PROLOG CONNECTION	*
		 *******************************/

int
alarm_error(term_t alarm, int err)
{ switch(err)
  { case ERR_RESOURCE:
      return pl_error(NULL, 0, NULL, ERR_RESOURCE, "timers");
    case ERR_PERMISSION:
      return pl_error(NULL, 0, "already installed", ERR_PERMISSION,
		      alarm, "install", "alarm");
    default:
      assert(0);
      return FALSE;
  }
}


static int
unify_timer(term_t t, Event ev)
{ if ( !PL_is_variable(t) )
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 0, t, "unbound");

  return PL_unify_term(t,
		       PL_FUNCTOR, FUNCTOR_alarm1,
		         PL_POINTER, ev);
}


static int
get_timer(term_t t, Event *ev)
{ if ( PL_is_functor(t, FUNCTOR_alarm1) )
  { term_t a = PL_new_term_ref();
    void *p;

    _PL_get_arg(1, t, a);
    if ( PL_get_pointer(a, &p) )
    { Event e = p;

      if ( e->magic == EV_MAGIC )
      { *ev = e;
        return TRUE;
      } else
      { return pl_error("get_timer", 1, NULL,
			ERR_DOMAIN, t, "alarm");
      }
    }
  }

  return pl_error("get_timer", 1, NULL,
		  ERR_ARGTYPE, 1, t, "alarm");
}


static int
pl_get_bool_ex(term_t arg, int *val)
{ if ( PL_get_bool(arg, val) )
    return TRUE;

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 0, arg, "bool");
}


static foreign_t
alarm4_gen(time_abs_rel abs_rel, term_t time, term_t callable,
	   term_t id, term_t options)
{ Event ev;
  double t;
  module_t m = NULL;
  unsigned long flags = 0L;

  if ( options )
  { term_t tail = PL_copy_term_ref(options);
    term_t head = PL_new_term_ref();

    while( PL_get_list(tail, head, tail) )
    { atom_t name;
      size_t arity;

      if ( PL_get_name_arity(head, &name, &arity) )
      { if ( arity == 1 )
	{ term_t arg = PL_new_term_ref();

	  _PL_get_arg(1, head, arg);

	  if ( name == ATOM_remove )
	  { int t = FALSE;

	    if ( !pl_get_bool_ex(arg, &t) )
	      return FALSE;
	    if ( t )
	      flags |= EV_REMOVE;
	  } else if ( name == ATOM_install )
	  { int t = TRUE;

	    if ( !pl_get_bool_ex(arg, &t) )
	      return FALSE;
	    if ( !t )
	      flags |= EV_NOINSTALL;
	  }
	}
      }
    }
    if ( !PL_get_nil(tail) )
      return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 4, options, "list");
  }

  if ( !PL_get_float(time, &t) )
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 1,
		    time, "number");
  if ( !PL_strip_module(callable, &m, callable) )
    return FALSE;

  if ( !(ev = allocEvent()) )
    return FALSE;

  if (abs_rel==TIME_REL)
	  setTimeEvent(ev, t);
  else
	  setTimeEventAbs(ev,t);

  if ( !unify_timer(id, ev) )
  { freeEvent(ev);			/* not linked: no need to lock */
    return FALSE;
  }

  ev->flags = flags;
  ev->module = m;
  ev->goal = PL_record(callable);

  if ( !(ev->flags & EV_NOINSTALL) )
  { int rc;

    if ( (rc=installEvent(ev)) != TRUE )
    { freeEvent(ev);			/* not linked: no need to lock */
      return alarm_error(id, rc);
    }
  }

  return TRUE;
}


static foreign_t
alarm4_abs(term_t time, term_t callable, term_t id, term_t options)
{ return alarm4_gen(TIME_ABS,time,callable,id,options);
}

static foreign_t
alarm4_rel(term_t time, term_t callable, term_t id, term_t options)
{ return alarm4_gen(TIME_REL,time,callable,id,options);
}

static foreign_t
alarm3_abs(term_t time, term_t callable, term_t id)
{ return alarm4_gen(TIME_ABS,time, callable, id, 0);
}

static foreign_t
alarm3_rel(term_t time, term_t callable, term_t id)
{ return alarm4_gen(TIME_REL,time, callable, id, 0);
}

static foreign_t
install_alarm(term_t alarm)
{ Event ev = NULL;
  int rc;

  if ( !get_timer(alarm, &ev) )
    return FALSE;

  if ( (rc=installEvent(ev)) != TRUE )
    return alarm_error(alarm, rc);

  return TRUE;
}


static foreign_t
install_alarm2(term_t alarm, term_t time)
{ Event ev = NULL;
  double t;
  int rc;

  if ( !get_timer(alarm, &ev) )
    return FALSE;

  if ( !PL_get_float(time, &t) )
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 1,
		    time, "number");

  setTimeEvent(ev, t);
  if ( (rc=installEvent(ev)) != TRUE )
    return alarm_error(alarm, rc);

  return TRUE;
}


static foreign_t
uninstall_alarm(term_t alarm)
{ Event ev = NULL;

  if ( !get_timer(alarm, &ev) )
    return FALSE;

  return uninstallEvent(ev);
}


static foreign_t
remove_alarm(term_t alarm)
{ Event ev = NULL;

  if ( !get_timer(alarm, &ev) )
    return FALSE;

  return removeEvent(ev);
}


static int
unify_event_goal(term_t goal, Event ev)
{ term_t g = PL_new_term_ref();

  return ( PL_recorded(ev->goal, g) &&
	   PL_unify_term(goal,
			 PL_FUNCTOR, FUNCTOR_module2,
			   PL_ATOM, PL_module_name(ev->module),
			   PL_TERM, g) );
}


static foreign_t
current_alarms(term_t time, term_t goal, term_t id, term_t status,
	       term_t matching)
{ Event ev;
  term_t next = PL_new_term_ref();
  term_t tail = PL_copy_term_ref(matching);
  term_t head = PL_new_term_ref();
  term_t av   = PL_new_term_refs(4);
  pthread_t self = pthread_self();
  int iterate;

  LOCK();
  if ( !PL_is_variable(id) )
  { if ( !get_timer(id, &ev) )
    { UNLOCK();
      return FALSE;
    }
    if ( !pthread_equal(self, ev->thread_id) )
      ev = NULL;
    iterate = FALSE;
  } else
  { ev = TheSchedule()->first;
    iterate = TRUE;
  }

  for(; ev; ev = ev->next)
  { atom_t s;
    double at;
    fid_t fid;
    int ok;

    if ( !pthread_equal(self, ev->thread_id) )
      continue;

    fid = PL_open_foreign_frame();

    if ( ev->flags & EV_DONE )
      s = ATOM_done;
    else if ( ev == TheSchedule()->scheduled )
      s = ATOM_next;
    else
      s = ATOM_scheduled;

    ok = PL_unify_atom(status, s);

    if ( ok )
      ok = unify_event_goal(goal, ev);

    if ( ok )
    { at = (double)ev->at.tv_sec + (double)ev->at.tv_usec / 1000000.0;
      ok = PL_unify_float(time, at);
    }

    PL_discard_foreign_frame(fid);

    if ( ok )
    { if ( !PL_put_float(av+0, at) ||		/* time */
	   !PL_put_variable(av+1)  ||		/* goal */
	   !unify_event_goal(av+1, ev) ||
	   !PL_put_variable(av+2) ||		/* id */
	   !unify_timer(av+2, ev) ||
	   !PL_put_atom(av+3, s) ||		/* status */
					        /* Create term */
	   !PL_cons_functor_v(next, FUNCTOR_alarm4, av) ||
						/* Add to list */
	   !PL_unify_list(tail, head, tail) ||
	   !PL_unify(head, next) )
      { UNLOCK();
	return FALSE;
      }
    } else
    { if ( PL_exception(0) )
      { UNLOCK();
	return FALSE;
      }
    }

    if ( !iterate )
      break;
  }
  UNLOCK();

  return PL_unify_nil(tail);
}


install_t
install_time()
{ MODULE_user	  = PL_new_module(PL_new_atom("user"));

  FUNCTOR_alarm1  = PL_new_functor(PL_new_atom("$alarm"), 1);
  FUNCTOR_alarm4  = PL_new_functor(PL_new_atom("alarm"), 4);
  FUNCTOR_module2 = PL_new_functor(PL_new_atom(":"), 2);

  ATOM_remove	  = PL_new_atom("remove");
  ATOM_install	  = PL_new_atom("install");
  ATOM_done	  = PL_new_atom("done");
  ATOM_next	  = PL_new_atom("next");
  ATOM_scheduled  = PL_new_atom("scheduled");

  PREDICATE_call1 = PL_predicate("call", 1, "user");

  PL_register_foreign("alarm_at",       4, alarm4_abs,     PL_FA_TRANSPARENT);
  PL_register_foreign("alarm",          4, alarm4_rel,     PL_FA_TRANSPARENT);
  PL_register_foreign("alarm_at",       3, alarm3_abs,     PL_FA_TRANSPARENT);
  PL_register_foreign("alarm",          3, alarm3_rel,     PL_FA_TRANSPARENT);
  PL_register_foreign("remove_alarm",   1, remove_alarm,   0);
  PL_register_foreign("uninstall_alarm",1, uninstall_alarm,0);
  PL_register_foreign("install_alarm",  1, install_alarm,  0);
  PL_register_foreign("install_alarm",  2, install_alarm2, 0);
  PL_register_foreign("remove_alarm_notrace",1, remove_alarm,   PL_FA_NOTRACE);
  PL_register_foreign("current_alarms", 5, current_alarms, 0);
#ifdef O_DEBUG
  PL_register_foreign("time_debug",	1, pl_time_debug,  0);
#endif

  installHandler();
  PL_on_halt(cleanup, NULL);
  PL_thread_at_exit(cleanup_thread, NULL, TRUE);
}


install_t
uninstall_time()
{ cleanup(0, NULL);
}
