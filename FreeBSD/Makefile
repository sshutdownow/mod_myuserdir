# New ports collection makefile for:    mod_myuserdir
# Date created:				October 25 2005
# Whom:					Igor Popov <igorpopov@newmail.ru>
#
# $$

PORTNAME=	mod_myuserdir
PORTVERSION=	0.99.3
CATEGORIES=	www
MASTER_SITES=	http://igorpopov.newmail.ru/
EXTRACT_SUFX=	.tar.gz

MAINTAINER=	igorpopov@newmail.ru
COMMENT=	An Apache module for mass ~user hosting - homedirs and php settings are stored in MySQL databases

USE_MYSQL=	yes
DOCS=		INSTALL README TODO httpd.conf.add tilde_users.sql

WANT_APACHE=13
AP_GENPLIST=yes

WRKSRC =${WRKDIR}/${PORTNAME}


.if !defined(NOPORTDOCS)
post-install:
	@${INSTALL} -d -m 0755 ${DOCSDIR}
.for file in ${DOCS}
	@${INSTALL_DATA} ${WRKSRC}/${file} ${DOCSDIR}/
.endfor
.endif

.include <bsd.port.mk>
