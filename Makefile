#
# Copyright (c) 2005-2025 Igor Popov <ipopovi@gmail.com>
#
# $Id: Makefile 38 2008-07-15 07:27:33Z igor_popov $
#

NAME = myuserdir
APACHE_MODULE = mod_myuserdir.so
MODULE_LA = mod_myuserdir.la
APXS = apxs

SRCS = mod_myuserdir.c mod_myuserdir_php.c
OBJS = $(SRCS:%.c=%.o)

ifeq (0,${MAKELEVEL})
ifeq (0,$(-shell [ apxs = 127 ]))
APXS := apxs2
else
APXS := apxs
endif
endif

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
	$(APXS) -i -a -n $(NAME) $(APACHE_MODULE) $(MODULE_LA)

clean:
	$(RM) $(OBJS) $(APACHE_MODULE) *.lo *.slo $(MODULE_LA)
