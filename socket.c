/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2002, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
This module defines the bottom layer for dealing with TCP stream-sockets
from SWI-Prolog, both server and client side.

The basis of this module was implemented by Gordon Streeter. It has been
redesigned to make it a bit  easier   to  use  and handle problems using
Prolog exceptions instead of special return-values.

The   tcp_select()   call   has   been     replaced    by   SWI-Prolog's
wait_for_input/3.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef __CYGWIN__
#undef HAVE_H_ERRNO
#endif

#include <SWI-Stream.h>
#include "clib.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif
#include <sys/types.h>
#ifdef WIN32
#include <io.h>
#include <winsock.h>
#else
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_H_ERRNO
extern int h_errno;
#else
#define h_errno errno
#endif
#define closesocket(n) close((n))	/* same on Unix */
#endif
#include <assert.h>
#include <string.h>

#ifdef _REENTRANT
#include <pthread.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mutex)
#define UNLOCK() pthread_mutex_unlock(&mutex)
#else
#define LOCK()
#define UNLOCK()
#endif

static functor_t FUNCTOR_socket1;
static functor_t FUNCTOR_module2;
static functor_t FUNCTOR_ip4;


		 /*******************************
		 *	 ADMINISTRATION		*
		 *******************************/

#define SOCK_INSTREAM	0x01
#define SOCK_OUTSTREAM	0x02
#define SOCK_BIND	0x04		/* What have we done? */
#define SOCK_LISTEN	0x08
#define SOCK_CONNECT	0x10
#define SOCK_ACCEPT	0x20		/* set on accepted sockets */

typedef struct _plsocket
{ struct _plsocket *next;		/* next in list */
  int		    socket;		/* The OS socket */
  int		    flags;		/* Misc flags */
} plsocket;

static plsocket *sockets;
static int initialised = FALSE;		/* Windows only */

static plsocket *
lookupSocket(int socket)
{ plsocket *p;

  LOCK();
  for(p=sockets; p; p = p->next)
  { if ( p->socket == socket )
    { UNLOCK();
      return p;
    }
  }

  if ( !(p = malloc(sizeof(plsocket))) )
  { pl_error(NULL, 0, NULL, ERR_ERRNO);
    UNLOCK();
    return NULL;
  }

  p->socket = socket;
  p->flags  = 0;
  p->next   = sockets;
  sockets   = p;

  UNLOCK();
  return p;
}


static int
freeSocket(int socket)
{ plsocket **p;

  LOCK();
  p = &sockets;

  for( ; *p; p = &(*p)->next)
  { if ( (*p)->socket == socket )
    { plsocket *tmp = *p;
      
      *p = tmp->next;
      free(tmp);
      break;
    }
  }
  UNLOCK();

  return closesocket(socket);
}


		 /*******************************
		 *	     CONVERSION		*
		 *******************************/

static int
tcp_get_socket(term_t Socket, int *id)
{ IOSTREAM *s;
  int socket;

  if ( PL_is_functor(Socket, FUNCTOR_socket1) )
  { term_t a = PL_new_term_ref();

    PL_get_arg(1, Socket, a);
    if ( PL_get_integer(a, id) )
      return TRUE;
  }
  
  if ( PL_get_stream_handle(Socket, &s) &&
       (socket = Sfileno(s)) >= 0 )
  { *id = socket;
    return TRUE;
  }

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, -1, Socket, "socket");
}


static int
tcp_unify_socket(term_t Socket, int id)
{ return PL_unify_term(Socket,
		       PL_FUNCTOR, FUNCTOR_socket1,
		         IntArg(id));
}


		 /*******************************
		 *	      ERRORS		*
		 *******************************/

#ifdef WIN32
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The code in BILLY_GETS_BETTER is, according to various documents the
right code, but it doesn't work, so we do it by hand.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#ifdef BILLY_GETS_BETTER

static char *
WinSockError(unsigned long eno)
{ char buf[1024];
  static HMODULE netmsg = 0;
  static int netmsg_loaded = FALSE;
  unsigned long flags = (FORMAT_MESSAGE_FROM_SYSTEM|
			 FORMAT_MESSAGE_IGNORE_INSERTS); 

  if ( !netmsg_loaded )
  { netmsg_loaded = TRUE;
    netmsg = LoadLibraryEx("netmsg.dll", 0, LOAD_LIBRARY_AS_DATAFILE);
    if ( !netmsg )
      Sdprintf("failed to load netmsg.dll\n");
    else
      Sdprintf("Loaded netmsg.dll as %p\n", netmsg); 
  }

  if ( netmsg )
    flags |= FORMAT_MESSAGE_FROM_HMODULE;

  if ( !FormatMessage(flags,
		      netmsg,
		      eno,
		      GetUserDefaultLangID(),
		      /*MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),*/
		      buf, sizeof(buf),
		      0))
  { sprintf(buf, "Unknown socket error (%u)", eno);
  }

  buf[sizeof(buf)-1]='\0';

  return strdup(buf);
}

#else /*BILLY_GETS_BETTER*/

static const char *
WinSockError(int error)
{ struct
  { int index;
    const char *string;
  } *ep, edefs[] =
  { { WSAEACCES, "Permission denied" },
    { WSAEADDRINUSE, "Address already in use" },
    { WSAEADDRNOTAVAIL, "Cannot assign requested address" },
    { WSAEAFNOSUPPORT, "Address family not supported by protocol family" },
    { WSAEALREADY, "Operation already in progress" },
    { WSAECONNABORTED, "Software caused connection abort" },
    { WSAECONNREFUSED, "Connection refused" },
    { WSAECONNRESET, "Connection reset by peer" },
    { WSAEDESTADDRREQ, "Destination address required" },
    { WSAEFAULT, "Bad address" },
    { WSAEHOSTDOWN, "Host is down" },
    { WSAEHOSTUNREACH, "No route to host" },
    { WSAEINPROGRESS, "Operation now in progress" },
    { WSAEINTR, "Interrupted function call" },
    { WSAEINVAL, "Invalid argument" },
    { WSAEISCONN, "Socket is already connected" },
    { WSAEMFILE, "Too many open files" },
    { WSAEMSGSIZE, "Message too long" },
    { WSAENETDOWN, "Network is down" },
    { WSAENETRESET, "Network dropped connection on reset" },
    { WSAENETUNREACH, "Network is unreachable" },
    { WSAENOBUFS, "No buffer space available" },
    { WSAENOPROTOOPT, "Bad protocol option" },
    { WSAENOTCONN, "Socket is not connected" },
    { WSAENOTSOCK, "Socket operation on non-socket" },
    { WSAEOPNOTSUPP, "Operation not supported" },
    { WSAEPFNOSUPPORT, "Protocol family not supported" },
    { WSAEPROCLIM, "Too many processes" },
    { WSAEPROTONOSUPPORT, "Protocol not supported" },
    { WSAEPROTOTYPE, "Protocol wrong type for socket" },
    { WSAESHUTDOWN, "Cannot send after socket shutdown" },
    { WSAESOCKTNOSUPPORT, "Socket type not supported" },
    { WSAETIMEDOUT, "Connection timed out" },
    { WSAEWOULDBLOCK, "Resource temporarily unavailable" },
    { WSAEDISCON, "Graceful shutdown in progress" },
    { 0, NULL }
  };

  for(ep=edefs; ep->string; ep++)
  { if ( ep->index == error )
      return ep->string;
  }

  return "Unknown error";
}

#endif /*BILLY_GETS_BETTER*/
#endif /*WIN32*/

#ifdef HAVE_H_ERRNO
typedef struct
{ int code;
  const char *string;
} error_codes;

static error_codes h_errno_codes[] = {
#ifdef HOST_NOT_FOUND
    { HOST_NOT_FOUND, "Host not found" },
#endif
#ifdef TRY_AGAIN
    { TRY_AGAIN, "Try Again" },
#endif
#ifdef NO_RECOVERY
    { NO_RECOVERY, "No Recovery" },
#endif
#ifdef NO_DATA
    { NO_DATA, "No Data" },
#endif
#ifdef NO_ADDRESS
    { NO_ADDRESS, "No Address" },
#endif
    {0, NULL}
};

#else /*HAVE_H_ERRNO*/
#define h_errno_codes NULL
typedef void * error_codes;
#endif /*HAVE_H_ERRNO*/

static int
tcp_error(int code, error_codes *map)
{ const char *msg;
  term_t except = PL_new_term_ref();

#ifdef WIN32
  msg = WinSockError(WSAGetLastError());
  WSASetLastError(0);
#else

#ifdef HAVE_H_ERRNO
  static char msgbuf[100];

  if ( map )
  { while( map->code && map->code != code )
      map++;
    if ( map->code )
      msg = map->string;
    else
    { sprintf(msgbuf, "Unknown error %d", code);
      msg = msgbuf;
    }
  } else
#endif
    msg = strerror(code);
#endif /*WIN32*/

  PL_unify_term(except,
		CompoundArg("error", 2),
		  CompoundArg("socket_error", 1),
		    AtomArg(msg),
		  PL_VARIABLE);

#if defined(WIN32) && 0
  free(msg);
#endif

  return PL_raise_exception(except);
}

		 /*******************************
		 *	  INITIALISATION	*
		 *******************************/

static int
tcp_init()
{ LOCK();
  if ( initialised )
  { UNLOCK();
    return TRUE;
  }
  initialised = TRUE;

#ifdef WIN32
{ WSADATA WSAData;
  int optionValue = SO_SYNCHRONOUS_NONALERT;

  if ( WSAStartup(MAKEWORD(1,1), &WSAData) )
  { UNLOCK();
    return PL_warning("tcp_init() - WSAStartup failed.");
  }

#if 0
  {  int err;
     err = setsockopt(INVALID_SOCKET, 
		      SOL_SOCKET, 
		      SO_OPENTYPE, 
		      (char *)&optionValue, 
		      sizeof(optionValue));

     if ( err != NO_ERROR )
     { UNLOCK();
       return PL_warning("tcp_winsock_init - setsockopt failed.");
     
     }
  }
#endif
}
#endif /*WIN32*/

  UNLOCK();
  return TRUE;
}
	

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
socket(-Socket)
    Create a stream inet socket.  The socket is represented by a term of
    the format $socket(Id).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static foreign_t
tcp_socket(term_t Socket)
{ int sock;
	
  if ( !tcp_init() )
    return FALSE;

  if ( (sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    return tcp_error(errno, NULL);

  if ( !lookupSocket(sock) )		/* register it */
  { closesocket(sock);
    return FALSE;
  }

  return tcp_unify_socket(Socket, sock);
}


static foreign_t
tcp_close_socket(term_t Socket)
{ int socket;

  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;

  closesocket(socket);

  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Translate a host + port-number into a sockaddr structure.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
tcp_get_port(term_t Port, int *port)
{ char *name;

  if ( PL_get_atom_chars(Port, &name) )
  { struct servent *service;
    
    if ( !(service = getservbyname(name, "tcp")) )
      return tcp_error(errno, NULL);

    *port = ntohs(service->s_port);
    return TRUE;
  }

  if ( PL_get_integer(Port, port) )
    return TRUE;

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, -1, Port, "port");
}


static int
tcp_get_sockaddr(term_t Address, struct sockaddr_in *addr)
{ struct hostent *host;
  char           *hostName = NULL;
  int		  port;
	
  addr->sin_family = AF_INET;

  if ( PL_is_functor(Address, FUNCTOR_module2) )
  { term_t arg = PL_new_term_ref();

    PL_get_arg(1, Address, arg);
    if ( !PL_get_atom_chars(arg, &hostName) )
      return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 1, arg, "atom");

    PL_get_arg(2, Address, arg);
    if ( !tcp_get_port(arg, &port) )
      return FALSE;
  } else if ( !tcp_get_port(Address, &port) )
    return FALSE;

  if ( hostName )
  { if( !(host = gethostbyname(hostName)) )
      return tcp_error(h_errno, h_errno_codes);
    if ( sizeof(addr->sin_addr) < host->h_length )
      return PL_warning("Oops, host address too long!");
    memcpy(&addr->sin_addr, host->h_addr, host->h_length);
  } else
    addr->sin_addr.s_addr = INADDR_ANY;
	
  addr->sin_port = htons((short)port);

  return TRUE;
}


static int
tcp_get_ip(term_t Ip, struct in_addr *ip)
{ unsigned long hip = 0;

  if ( PL_is_functor(Ip, FUNCTOR_ip4) )
  { int i, ia;
    term_t a = PL_new_term_ref();

    for(i=1; i<=4; i++)
    { PL_get_arg(i, Ip, a);
      if ( PL_get_integer(a, &ia) )
	hip |= ia << ((4-i)*8);
      else
	return FALSE;
    }
    hip = htonl(hip);
    memcpy(ip, &hip, sizeof(hip));

    return TRUE;
  }

  return FALSE;
}


static int
tcp_unify_ip(term_t Ip, struct in_addr *ip, int netorder)
{ unsigned long hip;

  if ( netorder )
    hip = ntohl(ip->s_addr);
  else
    hip = ip->s_addr;

  return PL_unify_term(Ip,
		       PL_FUNCTOR, FUNCTOR_ip4,
		         IntArg((hip >> 24) & 0xff),
		         IntArg((hip >> 16) & 0xff),
		         IntArg((hip >>  8) & 0xff),
		         IntArg((hip >>  0) & 0xff));
}


static foreign_t
tcp_host_to_address(term_t Host, term_t Ip)
{ struct in_addr ip;
  struct hostent *host;
  char *host_name;

  if ( PL_get_atom_chars(Host, &host_name) )
  { if ( (host = gethostbyname(host_name)) )
    { if ( sizeof(ip) == host->h_length )
      { memcpy(&ip, host->h_addr, host->h_length);
	return tcp_unify_ip(Ip, &ip, TRUE);
      } else
	return PL_warning("tcp_host_to_address/2: length mismatch in address");
    } else
      return tcp_error(h_errno, h_errno_codes);
  } else if ( tcp_get_ip(Ip, &ip) )
  { if ( (host = gethostbyaddr((char *)&ip, sizeof(ip), AF_INET)) )
      return PL_unify_atom_chars(Host, host->h_name);
    else
      return tcp_error(h_errno, h_errno_codes);
  }

  return FALSE;
}


foreign_t
tcp_setopt(term_t Socket, term_t opt)
{ int socket;
  char *s;
       
  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;

  if ( PL_get_atom_chars(opt, &s) )
  { if ( strcmp(s, "reuseaddr") == 0 )
    { int val = 1;

      if( setsockopt(socket, SOL_SOCKET, SO_REUSEADDR,
		     (const char *)&val, sizeof(val)) == -1)
      { return tcp_error(h_errno, h_errno_codes);
      }

      return TRUE;
    }
  }
       
  return pl_error(NULL, 0, NULL, ERR_DOMAIN, opt, "socket_option");
}


foreign_t
tcp_bind(term_t Socket, term_t Address)
{ struct sockaddr_in sockaddr;
  int socket;
       
  if ( !tcp_get_socket(Socket, &socket) ||
       !tcp_get_sockaddr(Address, &sockaddr) )
    return FALSE;
	
  if ( bind(socket,
	    (struct sockaddr*)&sockaddr, sizeof(sockaddr)))
    return tcp_error(errno, NULL);

  lookupSocket(socket)->flags |= SOCK_BIND;

  return TRUE;
}


foreign_t
tcp_connect(term_t Socket, term_t Address)
{ struct sockaddr_in sockaddr;
  int socket;
       
  if ( !tcp_get_socket(Socket, &socket) ||
       !tcp_get_sockaddr(Address, &sockaddr) )
    return FALSE;
	
  if ( connect(socket,
	       (struct sockaddr*)&sockaddr, sizeof(sockaddr)))
    return tcp_error(errno, NULL);

  lookupSocket(socket)->flags |= SOCK_CONNECT;

  return TRUE;
}


static foreign_t
tcp_accept(term_t Master, term_t Slave, term_t Peer)
{ int master, slave;
  struct sockaddr_in addr;
  int addrlen = sizeof(addr);
	
  if ( !tcp_get_socket(Master, &master) )
    return FALSE;

  if ( (slave = accept(master, (struct sockaddr*)&addr, &addrlen)) == -1 )
    return tcp_error(errno, NULL);

  lookupSocket(slave)->flags |= SOCK_ACCEPT;
  
  if ( tcp_unify_ip(Peer, &addr.sin_addr, TRUE) &&
       tcp_unify_socket(Slave, slave) )
    return TRUE;

  return FALSE;
}



foreign_t
tcp_listen(term_t Sock, term_t BackLog)
{ int socket;
  int backlog;

  if ( !tcp_get_socket(Sock, &socket) )
    return FALSE;

  if ( !PL_get_integer(BackLog, &backlog) ) 
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, -1, BackLog, "integer");

  if( listen(socket, backlog) == -1 )
    return tcp_error(errno, NULL);

  lookupSocket(socket)->flags |= SOCK_LISTEN;

  return TRUE;
}


		 /*******************************
		 *	  IO-STREAM STUFF	*
		 *******************************/

#define fdFromHandle(p) ((int)((long)(p)))

#ifdef WIN32
static void
waitMsg()
{ MSG msg;

  if ( GetMessage(&msg, NULL, 0, 0) )
  { TranslateMessage(&msg);
    DispatchMessage(&msg);
  } else
  { ExitProcess(0);			/* WM_QUIT received */
  }
}
#endif


static int
tcp_read(void *handle, char *buf, int bufSize)
{ int socket = fdFromHandle(handle);
  int n;

#ifdef WIN32
again:
#endif

  n = recv(socket, buf, bufSize, 0);

#ifdef WIN32
  if ( n < 0 && WSAGetLastError() == WSAEWOULDBLOCK )
  { waitMsg();

    goto again;
  }
#endif  

  return n;
}

static int
tcp_write(void *handle, char *buf, int bufSize)
{ int socket = fdFromHandle(handle);
  int len = bufSize;
  char *str = buf;

  while( len > 0 )
  { int n = send(socket, str, len, 0);

    if ( n < 0 )
    {
#ifdef WIN32
      if ( WSAGetLastError() == WSAEWOULDBLOCK )
      { waitMsg();			/* The process gets FD_WRITE */
	continue;
      }
#endif
      return -1;
    }

    len -= n;
    str += n;
  }

  return bufSize;
}


static long
tcp_seek_null(void *handle, long offset, int whence)
{ return -1;
}


static int
tcp_close_input(void *handle)
{ int socket = fdFromHandle(handle);

  plsocket *s = lookupSocket(socket);
  s->flags &= ~SOCK_INSTREAM;

  if ( !(s->flags & (SOCK_INSTREAM|SOCK_OUTSTREAM)) )
    return freeSocket(socket);

  return 0;
}


static int
tcp_close_output(void *handle)
{ int socket = fdFromHandle(handle);

  plsocket *s = lookupSocket(socket);
  s->flags &= ~SOCK_OUTSTREAM;

  if ( !(s->flags & (SOCK_INSTREAM|SOCK_OUTSTREAM)) )
    return freeSocket(socket);

  return 0;
}


static IOFUNCTIONS readFunctions =
{ tcp_read,
  tcp_write,
  tcp_seek_null,
  tcp_close_input,
};


static IOFUNCTIONS writeFunctions =
{ tcp_read,
  tcp_write,
  tcp_seek_null,
  tcp_close_output,
};


foreign_t
tcp_streams(term_t Socket, term_t Read, term_t Write)
{ IOSTREAM *in, *out;
  int socket;
  plsocket *pls;

  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;
  
  pls = lookupSocket(socket);
  in  = Snew((void *)(long)socket, SIO_FILE|SIO_INPUT,  &readFunctions);
  if ( !PL_open_stream(Read, in) )
    return FALSE;
  pls->flags |= SOCK_INSTREAM;

  if ( !(pls->flags & SOCK_LISTEN) )
  { out = Snew((void *)(long)socket, SIO_FILE|SIO_OUTPUT, &writeFunctions);
    if ( !PL_open_stream(Write, out) )
      return FALSE;
    pls->flags |= SOCK_OUTSTREAM;
  }

  return TRUE;
}


		 /*******************************
		 *	   BLOCKING IO		*
		 *******************************/
#ifdef WIN32
#define F_SETFL 0
#define O_NONBLOCK 0

static int
fcntl(int fd, int op, int arg)
{ switch(op)
  { case F_SETFL:
      switch(arg)
      { case O_NONBLOCK:
	{ int rval;
	  int non_block;

	  non_block = 1;
	  rval = ioctlsocket(fd, FIONBIO, &non_block);
	  return rval ? -1 : 0;
	}
	default:
	  return -1;
      }
    break;
    default:
      return -1;
  }
}
#endif

static foreign_t
tcp_fcntl(term_t Socket, term_t Cmd, term_t Arg)
{ int socket;
  char *cmd;

  if ( !tcp_get_socket(Socket, &socket) )
    return FALSE;
  if ( !PL_get_atom_chars(Cmd, &cmd) )
    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 2, Cmd, "atom");

  if ( strcmp(cmd, "setfl") == 0 )
  { char *arg;

    if ( !PL_get_atom_chars(Arg, &arg) )
      return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 3, Arg, "flag");
    if ( strcmp(arg, "nonblock") == 0 )
    { fcntl(socket, F_SETFL, O_NONBLOCK);
      return TRUE;
    }

    return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 3, Arg, "flag");
  }

  return pl_error(NULL, 0, NULL, ERR_ARGTYPE, 3, Arg, "command");
}


static foreign_t
pl_gethostname(term_t name)
{ char buf[256];

  if ( !tcp_init() )
    return FALSE;

  if ( gethostname(buf, sizeof(buf)) == 0 )
  { struct hostent *he;

    if ( (he = gethostbyname(buf)) )
      return PL_unify_atom_chars(name, he->h_name);
    else
      return PL_unify_atom_chars(name, buf);
  }

  return tcp_error(h_errno, h_errno_codes);
}


install_t
install_socket()
{ FUNCTOR_socket1 = PL_new_functor(PL_new_atom("$socket"), 1);
  FUNCTOR_module2 = PL_new_functor(PL_new_atom(":"), 2);
  FUNCTOR_ip4     = PL_new_functor(PL_new_atom("ip"), 4);
  
  PL_register_foreign("tcp_accept",           3, tcp_accept,          0);
  PL_register_foreign("tcp_bind",             2, tcp_bind,            0);
  PL_register_foreign("tcp_connect",          2, tcp_connect,         0);
  PL_register_foreign("tcp_listen",           2, tcp_listen,          0);
  PL_register_foreign("tcp_open_socket",      3, tcp_streams,         0);
  PL_register_foreign("tcp_socket",           1, tcp_socket,          0);
  PL_register_foreign("tcp_close_socket",     1, tcp_close_socket,    0);
  PL_register_foreign("tcp_fcntl",            3, tcp_fcntl,           0);
  PL_register_foreign("tcp_setopt",           2, tcp_setopt,          0);
  PL_register_foreign("tcp_host_to_address",  2, tcp_host_to_address, 0);
  PL_register_foreign("gettcp_host_to_address",  2, tcp_host_to_address, 0);
  PL_register_foreign("gethostname",          1, pl_gethostname,      0);
}


install_t
uninstall_socket()
{ if ( initialised )
  {
#ifdef WIN32
    WSACleanup();
#endif
  }
}


