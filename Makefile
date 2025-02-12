#
# Copyright (c) 2005-2025 Igor Popov <ipopovi@gmail.com>
#
# $Id: Makefile 38 2008-07-15 07:27:33Z igor_popov $
#

NAME = myuserdir
APACHE_MODULE = mod_myuserdir.so
MODULE_LA = $(APACHE_MODULE:%.so=%.la)
APXS = apxs

SRCS = mod_myuserdir.c mod_myuserdir_php.c
OBJS = $(SRCS:%.c=%.o)

RM = rm -rf
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
	$(APXS) -i -a -n $(NAME) $(MODULE_LA)

clean:
	$(RM) $(OBJS) $(APACHE_MODULE) $(MODULE_LA) *.slo *.lo .libs
