#
# Copyright (c) 2005-2007 Igor Popov <igorpopov@newmail.ru>
#
# $Id: Makefile 31 2008-06-11 09:08:22Z igor_popov $
#

NAME = myuserdir
APACHE_MODULE = mod_myuserdir.so
APXS = apxs

SRCS = mod_myuserdir.c mod_myuserdir_php.c escape_sql.c
OBJS = mod_myuserdir.o mod_myuserdir_php.o escape_sql.o

RM = rm -f
LN = ln -sf
CP = cp -f

MYSQCPPFLAGS = `mysql_config --include`
MYSQLDFLAGS  = `mysql_config --libs`

CFLAGS =  -Wc,-W -Wc,-Wall -DWITH_PHP -DWITH_CACHE $(MYSQCPPFLAGS)
CFLAGS += -DNDEBUG
#CFLAGS += -DDEBUG -Wc,-g3

LDFLAGS = $(MYSQLDFLAGS)

default: all

all: $(APACHE_MODULE)

$(APACHE_MODULE): $(SRCS)
	$(APXS) -c $(CFLAGS) $(LDFLAGS) $(SRCS)

install: all
	$(APXS) -i -a -n $(NAME) $(APACHE_MODULE)

clean:
	$(RM) $(OBJS) $(APACHE_MODULE) *.lo *.slo mod_myuserdir.la
