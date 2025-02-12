#
# Copyright (c) 2005-2007 Igor Popov <igorpopov@newmail.ru>
#
# $Id: Makefile 38 2008-07-15 07:27:33Z igor_popov $
#

NAME = myuserdir
APACHE_MODULE = mod_myuserdir.so
APXS = apxs

SRCS = mod_myuserdir.c mod_myuserdir_php.c
OBJS = mod_myuserdir.o mod_myuserdir_php.o

RM = rm -f
LN = ln -sf
CP = cp -f


CFLAGS =  -Wc,-W -Wc,-Wall -DWITH_PHP
#CFLAGS += -DNDEBUG
CFLAGS += -DDEBUG -Wc,-g -Wc,-ggdb3
LDFLAGS =


default: all

all: $(APACHE_MODULE)

$(APACHE_MODULE): $(SRCS)
	$(APXS) -c $(CFLAGS) $(LDFLAGS) $(SRCS)

install: all
	$(APXS) -i -a -n $(NAME) $(APACHE_MODULE)

clean:
	$(RM) $(OBJS) $(APACHE_MODULE) *.lo *.slo mod_myuserdir.la
