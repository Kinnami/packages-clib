/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        jan@swi.psy.uva.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2002, University of Amsterdam

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

    As a special exception, if you link this library with other files,
    compiled with a Free Software compiler, to produce an executable, this
    library does not by itself cause the resulting executable to be covered
    by the GNU General Public License. This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
*/

:- asserta(library_directory('.')).
:- asserta(user:file_search_path(foreign, '.')).
:- use_module(library(unix)).
:- use_module(library(socket)).

myls(Dir, Status) :-
	fork(Pid),
	(   Pid == child
	->  exec(ls(Dir))
	;   repeat,
	    wait(Pid, Status)
	).

signal(Status) :-
	fork(Pid),
	(   Pid = child
	->  sleep(100)
	;   kill(Pid, 9),
	    wait(Pid, Status)
	).

		 /*******************************
		 *	  SOCKET (CLIENT)	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
High-level interface:

Server:
=======
	tcp_server(+Address, +AcceptCallBack)
	AcceptCallBack(+Address, +Peer, +ReadFd, +WriteFd)
	...
	close(ReadFd),
	close(WriteFd).
	tcp_shutdown(Address).

	tcp_dispatch.

Client:
=======
	tcp_open(+Address, -ReadFd, -WriteFd),
	...
	close(ReadFd),
	close(WriteFd)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */








get_http_data(Host, URL) :-
	tcp_socket(Socket),
	tcp_connect(Socket, Host:80),
	tcp_open_socket(Socket, Read, Write),
	format(Write, 'GET ~w~n~n', URL),
	flush_output(Write),
	copy_stream(Read, user_output),
	close(Read),
	close(Write).

copy_stream(In, Out) :-
	get0(In, C0),
	copy_stream(C0, In, Out).

copy_stream(-1, _, _) :- !.
copy_stream(C, In, Out) :-
	put(Out, C),
	get0(In, C2),
	copy_stream(C2, In, Out).


		 /*******************************
		 *	   SOCKET (SERVER)	*
		 *******************************/

:- dynamic
	server/1,
	client/2.

mkserver :-
	tcp_socket(Socket),
	tcp_bind(Socket, 3000),
	tcp_listen(Socket, 5),
	tcp_open_socket(Socket, Read, _),
	asserta(server(Read)).

dispatch :-
	repeat,
	\+ dispatch1, !.

dispatch1 :-
	findall(C, client(C, _), Clients),
	server(Server),
	wait_for_input([Server|Clients], Ready, 0),
	dispatch(Ready).

dispatch([]).
dispatch([H|T]) :-
	format('Dispatching ~w ... ', [H]), flush,
	dispatch_fd(H),
	format('ok~n'),
	dispatch(T).

dispatch_fd(Server) :-
	server(Server), !,
	tcp_accept(Server, ClientSocket, Peer),
	format('Connected from ~w~n', [Peer]),
	tcp_fcntl(ClientSocket, setfl, nonblock),
	tcp_open_socket(ClientSocket, Read, Write),
	format(Write, 'Please to meet you!~n', []),
	flush_output(Write),
	assert(client(Read, Write)).
dispatch_fd(Client) :-
	client(Client, Write),
	(   at_end_of_stream(Client)
	->  format('Closing client ~w~n', [Client]),
	    close(Client),
	    close(Write),
	    retractall(client(Client, _))
	;   format(Write, 'You typed: ', []),
	    copy_stream(Client, Write),
	    flush_output(Write)
	).
	
	
		 /*******************************
		 *	       PIPE		*
		 *******************************/

pipe_demo(Result) :-
	pipe(Read, Write),
	fork(Pid),
	(   Pid == child
	->  close(Read),
	    format(Write, '~w.~n', [hello(world)]),
	    halt
	;   close(Write),
	    read(Read, Result),
	    close(Read)
	).

fork_server :-
	fork(Pid),
	(   Pid == child,
	    detach_IO,
	    sleep(60),
	    write(done)
	;   writeln(Pid)
	).
	
