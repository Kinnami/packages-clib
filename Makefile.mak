################################################################
# Build the SWI-Prolog tabling package for MS-Windows
#
# Author: Jan Wielemaker
# 
# Use:
#	nmake /f Makefile.mak
#	nmake /f Makefile.mak install
################################################################

PLHOME=..\..
!include $(PLHOME)\src\rules.mk
PKGDLL=socket

SOCKOBJ=	socket.obj error.obj
CGIOBJ=		error.obj form.obj cgi.obj

all:		socket.dll cgi.dll

socket.dll:	$(SOCKOBJOBJ)
		$(LD) /dll /out:$@ $(LDFLAGS) $(SOCKOBJ) $(PLLIB) $(LIBS)
cgi.dll:	$(CGIOBJ)
		$(LD) /dll /out:$@ $(LDFLAGS) $(CGIOBJ) $(PLLIB) $(LIBS)

!IF "$(CFG)" == "rt"
install:	idll
!ELSE
install:	idll ilib
!ENDIF

idll::
		copy socket.dll $(BINDIR)
		copy cgi.dll $(BINDIR)
ilib::
		copy socket.pl $(PLBASE)\library
		copy cgi.pl $(PLBASE)\library
		$(MAKEINDEX)

uninstall::
		del $(BINDIR)\socket.dll
		del $(BINDIR)\cgi.dll
		del $(PLBASE)\library\socket.pl
		del $(PLBASE)\library\cgi.pl
		$(MAKEINDEX)

html-install::
		copy clib.html $(PKGDOC)

clean::
		DEL *.obj *~

distclean:	clean
		DEL socket.dll socket.lib socket.exp
		DEL cgi.dll cgi.lib cgi.exp

