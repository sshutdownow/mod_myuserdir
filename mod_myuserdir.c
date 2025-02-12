/*
   Copyright (c) 2005-2007 Igor Popov <igorpopov@newmail.ru>

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

static const char cvsid[] = "$Id: mod_myuserdir.c 24 2007-08-05 20:04:18Z igor_popov $";


#define CORE_PRIVATE

#include "httpd.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"
#include "http_main.h"
#include "http_protocol.h"
#include "util_script.h"

#ifdef WITH_CACHE
#include "ap_alloc.h"
#include "ap_hash.h" /* backported from apr-1.2.2 */
#endif	/* WITH_CACHE */

#include <mysql.h>
#include "escape_sql.h"

module MODULE_VAR_EXPORT myuserdir_module;

typedef struct {

    int myuserdir_enabled;
    int mysql_connected;

    MYSQL *mysql;
    char *mysql_host;
    char *mysql_user;
    char *mysql_pass;
    char *mysql_dbname;
    char *mysql_user_query;
    char *mysql_unixsock;
    unsigned short int mysql_inetsock;
#ifdef WITH_CACHE
    int cache_enabled;
    ap_pool   *pool;
    ap_hash_t *cache;
    time_t ttl_positive;
    time_t ttl_negative;
#endif /* WITH_CACHE */
} myuserdir_cfg_t;

#ifdef WITH_PHP

#define PHP_INI_USER	(1<<0)
#define PHP_INI_PERDIR	(1<<1)
#define PHP_INI_SYSTEM	(1<<2)

#define PHP_INI_STAGE_STARTUP		(1<<0)
#define PHP_INI_STAGE_SHUTDOWN		(1<<1)
#define PHP_INI_STAGE_ACTIVATE		(1<<2)
#define PHP_INI_STAGE_DEACTIVATE	(1<<3)
#define PHP_INI_STAGE_RUNTIME		(1<<4)	

__BEGIN_DECLS
int zend_alter_ini_entry(const char *, size_t, const char *, size_t, int, int);
int zend_restore_ini_entry(const char *, size_t, int);
__END_DECLS

#pragma weak _zend_alter_ini_entry
#pragma weak _zend_restore_ini_entry

#pragma weak zend_alter_ini_entry = _zend_alter_ini_entry
#pragma weak zend_restore_ini_entry = _zend_restore_ini_entry

int _zend_alter_ini_entry(const char* __unused1, size_t __unused2, const char* __unused3, size_t  __unused4, int __unused5, int __unused6)
{
    return -1;
}

int _zend_restore_ini_entry(const char* __unused1, size_t __unused2, int __unused3)
{
    return -1;
}

#endif /* WITH_PHP */

#ifdef WITH_CACHE /* experimental internal cache support */

typedef struct {
    char *homedir;
#ifdef WITH_PHP
    char *php_ini_conf;
#endif
    int hits; /* negative means user not found or blocked */
    time_t access_time;
} cache_t, *p_cache_t;

static p_cache_t cache_userdir_find(myuserdir_cfg_t *cfg, const char *username)
{
	p_cache_t userdir;
	time_t cur;

	if (!cfg->cache_enabled) {
	    return NULL;
	}
	
	userdir = ap_hash_get(cfg->cache, username, AP_HASH_KEY_STRING);
	if (!userdir) {
	    return NULL;
	}

	cur = time(NULL);

	if (userdir->hits > 0 /*&& userdir->hits < 512*/ && userdir->access_time + cfg->ttl_positive >= cur) {
		userdir->hits++;
	} else if (userdir->hits < 0 /*&& userdir->hits > -256*/ && userdir->access_time + cfg->ttl_negative >= cur) {
		userdir->hits--;
	} else { /* expired */
		ap_hash_set(cfg->cache, username, AP_HASH_KEY_STRING, NULL);	/* delete hash entry */
		userdir = NULL;
	}
	return userdir;
}

static void cache_userdir_add(myuserdir_cfg_t *cfg,
				const char *username,
				const char *homedir,
#ifdef WITH_PHP
			         const char *php_ini_conf,
#endif
			         const int hits)
{
	p_cache_t userdir;

	if (!cfg->cache_enabled) {
	    return;
	}
	userdir = ap_pcalloc(cfg->pool, sizeof(cache_t));
	userdir->access_time = time(NULL);
	userdir->homedir = ap_pstrdup(cfg->pool, homedir);
#ifdef WITH_PHP
        userdir->php_ini_conf = ap_pstrdup(cfg->pool, php_ini_conf);
#endif
        userdir->hits = hits;
        ap_hash_set(cfg->cache, username, AP_HASH_KEY_STRING, userdir);
}

static void cache_userdir_del(myuserdir_cfg_t *cfg, ap_hash_t *cache, const char *host)
{
	if (!cfg->cache_enabled) {
	    return;
	}
	ap_hash_set(cache, host, AP_HASH_KEY_STRING, NULL);	/* delete hash entry */
}

#endif /* WITH_CACHE */


static int myuserdir_setup(server_rec *s)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(s->module_config, &myuserdir_module);
    cfg->mysql_connected = 0;

    if (!cfg->myuserdir_enabled) {
	ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, s,
	     "MyUserdir is disabled, but tried to connect to MySQL server");
	return -1;
    }
    /* This should never ever ever happen */
    if (!cfg->mysql) {
	ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, s, "MySQL handle is NULL");
	return -1;
    }

    if (!mysql_real_connect(cfg->mysql, cfg->mysql_host, cfg->mysql_user,
			    cfg->mysql_pass, cfg->mysql_dbname, cfg->mysql_inetsock, cfg->mysql_unixsock, 0)) {
	ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, s,
		     "failed to connect to database '%s': %s", cfg->mysql_dbname, mysql_error(cfg->mysql));
	cfg->mysql_connected = 0;
	return -1;
    }
#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, s, "MyUserdir connected to MySQL");
#endif
    cfg->mysql_connected = 1;

    return 0;
}

static void *myuserdir_create_server_config(pool *p, server_rec *s)
{
    myuserdir_cfg_t *cfg = (myuserdir_cfg_t *) ap_pcalloc(p, sizeof(myuserdir_cfg_t));
#ifdef WITH_CACHE
    cfg->ttl_positive = 300;
    cfg->ttl_negative = 180;
#endif /* WITH_CACHE */

    return (void *)cfg;
}

static void *myuserdir_merge_server_config(pool *p, void *base, void *override)
{
    myuserdir_cfg_t *new_conf = (myuserdir_cfg_t *) ap_pcalloc(p, sizeof(myuserdir_cfg_t));
    myuserdir_cfg_t *base_conf = (myuserdir_cfg_t *) base;
    myuserdir_cfg_t *override_conf = (myuserdir_cfg_t *) override;

    new_conf->mysql = ap_pcalloc(p, sizeof(MYSQL));
    mysql_init(new_conf->mysql);

    new_conf->myuserdir_enabled = (override_conf->myuserdir_enabled == 1) ? 1 : 0;
    new_conf->mysql_connected = 0;

    new_conf->mysql_host = (override_conf->mysql_host == NULL) ?
	base_conf->mysql_host : override_conf->mysql_host;

    new_conf->mysql_user = (override_conf->mysql_user == NULL) ?
	base_conf->mysql_user: override_conf->mysql_user;

    new_conf->mysql_pass = (override_conf->mysql_pass == NULL) ?
	base_conf->mysql_pass : override_conf->mysql_pass;

    new_conf->mysql_dbname = (override_conf->mysql_dbname == NULL) ?
	base_conf->mysql_dbname : override_conf->mysql_dbname;

    new_conf->mysql_inetsock = (override_conf->mysql_inetsock == 0) ?
	base_conf->mysql_inetsock : override_conf->mysql_inetsock;

    new_conf->mysql_unixsock = (override_conf->mysql_unixsock == NULL) ?
	base_conf->mysql_unixsock : override_conf->mysql_unixsock;

    new_conf->mysql_user_query = (override_conf->mysql_user_query == NULL) ?
	base_conf->mysql_user_query : override_conf->mysql_user_query;

#ifdef WITH_CACHE
    new_conf->cache_enabled = (override_conf->cache_enabled == 1) ? 1 : 0;
    new_conf->pool = ap_make_sub_pool(p);
    new_conf->cache = ap_hash_make(new_conf->pool);
    
    new_conf->ttl_positive = (override_conf->ttl_positive == 0) ?
	base_conf->ttl_positive : override_conf->ttl_positive;
    new_conf->ttl_negative = (override_conf->ttl_negative == 0) ?
	base_conf->ttl_negative : override_conf->ttl_negative;
#endif
    return new_conf;
}

static void myuserdir_child_init(server_rec *s, pool *p)
{
    myuserdir_cfg_t *cfg =
        ap_get_module_config(s->module_config, &myuserdir_module);

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, s, "child init");
#endif
#ifdef WITH_CACHE
    cfg->pool = ap_make_sub_pool(NULL);
    cfg->cache = ap_hash_make(cfg->pool);
#endif
    cfg->mysql = ap_pcalloc(p, sizeof(MYSQL));
    mysql_init(cfg->mysql);
#if 1
    cfg->mysql_connected = 0;
#else    
    myuserdir_setup(s);
#endif
}
				

static void myuserdir_child_exit(server_rec *s, pool *p)
{
    myuserdir_cfg_t *cfg =
        (myuserdir_cfg_t*)ap_get_module_config(s->module_config, &myuserdir_module);

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, s, "child exit");
#endif

    if (cfg->mysql_connected && cfg->mysql) {
        mysql_close(cfg->mysql);
    }

#ifdef WITH_CACHE
    ap_destroy_pool(cfg->pool);
#endif
}

static void cleanup_mysql_result(void *result)
{
    if (result) {
	mysql_free_result((MYSQL_RES *) result);
	result = 0;
    }
}

static void reg_cleanup_mysql_result(pool *p, MYSQL_RES * result)
{
    ap_register_cleanup(p, (void *)result, cleanup_mysql_result, &ap_null_cleanup);
}

static void run_cleanup_mysql_result(pool *p, MYSQL_RES * result)
{
    ap_run_cleanup(p, (void *)result, &cleanup_mysql_result);
}

/* main */
static int myuserdir_translate(request_rec *r)
{
    myuserdir_cfg_t *cfg =
	ap_get_module_config(r->server->module_config, &myuserdir_module);
    char *query = 0;
    char *name = 0;
    char *safe_name = 0;
    char *dname = 0;
    char *homedir = 0;
    MYSQL_RES *res_set = 0;
    MYSQL_ROW row;
    int num_fields_fetched = 0;
    int name_len;
#ifdef WITH_CACHE
    p_cache_t userdir = 0;
#endif
#ifdef WITH_PHP
    char *php_ini_conf = 0;
#endif

//    if (zend_restore_ini_entry("open_basedir", sizeof(), ) < 0) {
//    ;
//    }

    if (!cfg->myuserdir_enabled) {
	return DECLINED;
    }

    if ((!cfg->mysql_connected || mysql_ping(cfg->mysql)) && myuserdir_setup(r->server) < 0) {
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, r, "declined: lost connection to mysql");
	return DECLINED;
    }
    
    if (!cfg->mysql_user_query) { /* it is seemed to be redundant, but it should be there */
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, r, "declined: !mysql_user_query");
	return DECLINED;
    }

    if (r->uri == 0 || r->uri[0] != '/' || r->uri[1] != '~') {
#ifdef DEBUG
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
		      "declined: uri '%s' doesn't begin with '/~'", r->uri);
#endif
	return DECLINED;
    }

    dname = ap_pstrdup(r->pool, r->uri + 2); /* skip '/~' */

    if (!dname || !*dname) {
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r,
		      "declined: uri '%s' has no username", r->uri);
	return DECLINED;
    }

    if (ap_ind(dname, '/') == -1) {	/* redirect /~user => /~user/ */
	char *redirect = ap_pstrcat(r->pool, r->uri, "/", NULL);

	ap_table_setn(r->headers_out, "Location", redirect);
#ifdef DEBUG
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
		      "redirect: '%s' to '%s'", r->uri, redirect);
#endif
	return REDIRECT;
    }

    name = ap_getword(r->pool, (const char **)&dname, '/');

    /* If there is .. it is not for us */
    if (name[0] == '\0' || (name[1] == '.' && (name[2] == '\0' || (name[2] = '.' && name[3] == '\0')))) {
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r,
		      "declined: bad uri '%s'", r->uri);
	return DECLINED;
    }

    if (dname[0] == '/') {
	dname--;
    }

#ifdef WITH_CACHE
    if ((userdir = cache_userdir_find(cfg, name)) != 0) {
	if (userdir->hits > 0) {
    	    homedir = userdir->homedir;
#ifdef WITH_PHP
    	    php_ini_conf = ap_pstrdup(r->pool, userdir->php_ini_conf);
#endif
#ifdef DEBUG
    	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
			    "cache: user '%s' found in positive cache", name);
#endif
	    goto USER_FOUND; /* but I don't like goto */
	} else if (userdir->hits < 0) {
#ifdef DEBUG
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
			  "cache: user '%s' found in negative cache", name);
#endif
	    return HTTP_NOT_FOUND;
	}
    }	
#endif /* WITH_CACHE */

    name_len = strlen(name);
    safe_name = ap_pcalloc(r->pool, name_len * 2 + 1); /* escape all chars */
#if 1
    escape_sql(name, name_len, safe_name, name_len * 2 + 1);
#else
#ifdef HAVE_MYSQL_REAL_ESCAPE_STRING
    mysql_real_escape_string(cfg->mysql, safe_name, name, name_len);
#else    
    mysql_escape_string(safe_name, name, name_len);
#endif
#endif    
    query = ap_psprintf(r->pool, cfg->mysql_user_query, safe_name, NULL);

    ap_block_alarms();		/* to prevent memleaks from mysql library */

    if (mysql_real_query(cfg->mysql, query, strlen(query))) { /* query failed */
	ap_unblock_alarms();
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r,
		      "declined: error in sql query '%s' %s", query, mysql_error(cfg->mysql));
	ap_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "QUERY_ERROR");
	return DECLINED;
    }

    /* we have data */
    res_set = mysql_store_result(cfg->mysql);
    reg_cleanup_mysql_result(r->pool, res_set);

    ap_unblock_alarms();

    row = mysql_fetch_row(res_set);
    if (!row) {
	ap_block_alarms();
	{
	    run_cleanup_mysql_result(r->pool, res_set);
#ifdef WITH_CACHE
#ifdef WITH_PHP
		cache_userdir_add(cfg, name, 0, 0, -1);
#else
		cache_userdir_add(cfg, name, 0, -1);
#endif
#endif
	}
	ap_unblock_alarms();
#if defined(WITH_CACHE) && defined(DEBUG)
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
			"cache: user '%s' added to negative cache", name);
#endif /* WITH_CACHE && DEBUG */
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r,
			"http_not_found: user '%s' not found", name);
	ap_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "USER_NOT_FOUND");
	return HTTP_NOT_FOUND;	/* DECLINED */
    }

    if ((num_fields_fetched = mysql_num_fields(res_set)) > 0) {
	struct stat finfo;

	switch (num_fields_fetched) {
	default:
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r,
			  "there are too many fields (%d) in mysql response", num_fields_fetched);
#ifdef WITH_PHP
	case 2:
	    if (row[1]) {
		php_ini_conf = ap_pstrdup(r->pool, row[1]);
	    }
#endif /* WITH_PHP */
	case 1:
	    if (row[0]) {
		homedir = ap_pstrdup(r->pool, row[0]);
	    }
	}

	ap_block_alarms();	/* to avoid memleaks */
	{
	    run_cleanup_mysql_result(r->pool, res_set);
	}
	ap_unblock_alarms();

USER_FOUND:
	if (!homedir) {
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, r,
			  "declined: no homedir for user '%s'", name);
	    return DECLINED;
	}

	if (!ap_is_directory(homedir)) {
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, r,
		       "declined: homedir '%s' is not dir at all", homedir);
	    ap_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "WRONG_HOMEDIR");
	    return DECLINED;
	}

#ifdef WITH_CACHE
	if (!userdir) { /* not found in cache */
		ap_block_alarms();	/* to not break cache */
		{
#ifdef WITH_PHP
			cache_userdir_add(cfg, name, homedir, php_ini_conf, 1);
#else
			cache_userdir_add(cfg, name, homedir, 1);
#endif
		}
		ap_unblock_alarms();
#ifdef DEBUG
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
			    "cache: user '%s' added to positive cache", name);
#endif /* DEBUG */
	}
#endif

	if (dname) {
	    r->filename = ap_pstrcat(r->pool, homedir, "/", dname, NULL);
	} else {
	    r->filename = homedir;
	}

	ap_no2slash(r->filename);

	if (r->filename && stat(r->filename, &finfo) != -1) {
	    r->finfo = finfo;
	} else {
#ifdef DEBUG
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r,
			  "http_not_found: file '%s' doesn't exist", r->filename);
#endif /* DEBUG */
	    r->filename = 0;
	    return HTTP_NOT_FOUND;
	}

#ifdef WITH_PHP
	ap_table_setn(r->subprocess_env, "PHP_DOCUMENT_ROOT", homedir);
	if (zend_alter_ini_entry("open_basedir", sizeof("open_basedir"), homedir, strlen(homedir), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, r, "setting 'open_basedir' to '%s' failed", homedir);
	}

	if (zend_alter_ini_entry("safe_mode", sizeof("safe_mode"), "1", 1, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
		ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, r, "turning on 'safe_mode' failed");
	}

	if (php_ini_conf) {		/* there is an extra php config */
	    char *linend, *value;
#ifdef DEBUG
	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r, "php extra config is '%s'", php_ini_conf);
#endif
	    while ((linend = strchr(php_ini_conf, ';')) != NULL) {

		*linend++ = '\0';
		value = strchr(php_ini_conf, '=');
		if ((value = strchr(php_ini_conf, '=')) != NULL) {
		    *value++ = '\0';
#ifdef DEBUG
		    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r, "setting '%s' to '%s'", php_ini_conf, value);
#endif
		    if (zend_alter_ini_entry(php_ini_conf, strlen(php_ini_conf) + 1, value, strlen(value), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
			ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r, "setting '%s' to '%s' failed", php_ini_conf, value);
		    }
		}
		if (linend) {
		    php_ini_conf = linend;
		}
	    }

	    if (php_ini_conf && *php_ini_conf && (value = strchr(php_ini_conf, '=')) != NULL) {
		*value++ = '\0';
#ifdef DEBUG
		ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r, "setting php param '%s' to value '%s'", php_ini_conf, value);
#endif
		if (zend_alter_ini_entry(php_ini_conf, strlen(php_ini_conf) + 1, value, strlen(value), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
			ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, r, "setting '%s' to '%s' failed", php_ini_conf, value);
		}
	    }
	}
#endif /* WITH_PHP */

#ifdef DEBUG
	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r, "OK: translate '%s' to '%s'", r->uri, r->filename);
#endif
	return OK;
    }

    /* not for us */
#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, r, "declined");
#endif
    return DECLINED;
}

/*
* config stuff
*/
static const char *set_host(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
	if (!arg || !strlen(arg))
	return "mysql db host must be set";
    cfg->mysql_host = ap_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *set_user(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
    if (!arg || !strlen(arg))
	return "mysql db user must be set";
    cfg->mysql_user = ap_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *set_pass(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
    if (!arg || !strlen(arg))
	return "mysql db passwd must be set";
    cfg->mysql_pass = ap_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *set_dbname(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
    if (!arg || !strlen(arg))
	return "mysql db name must be set";
    cfg->mysql_dbname = ap_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *set_socket(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
    cfg->mysql_unixsock = 0;
    cfg->mysql_inetsock = 0;
    if (arg && strlen(arg) > 0) {
	if (arg[0] == '/') {
	    cfg->mysql_unixsock = ap_pstrdup(cmd->pool, arg);
	} else {
	    cfg->mysql_inetsock = ap_strtol(arg, 0, 10);
	}
    }
    return NULL;
}

static const char *set_myquery(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config, &myuserdir_module);
    if (!arg || !strlen(arg))
	return "mysql query must be set";
    cfg->mysql_user_query = ap_pstrdup(cmd->pool, arg);
    return NULL;
}

static const char *set_module_onoff(cmd_parms *cmd, void *__unused__, int flag)
{
    myuserdir_cfg_t *cfg = (myuserdir_cfg_t *) ap_get_module_config(cmd->server->module_config, &myuserdir_module);
    cfg->myuserdir_enabled = (flag ? 1 : 0);
    return NULL;
}

#ifdef WITH_CACHE
static const char *set_cache_onoff(cmd_parms *cmd, void *__unused__, int flag)
{
    myuserdir_cfg_t *cfg = (myuserdir_cfg_t *) ap_get_module_config(cmd->server->module_config, &myuserdir_module);
    cfg->cache_enabled = (flag ? 1 : 0);
    return NULL;
}

static const char *set_ttl_positive(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
    if (arg && strlen(arg) > 0) {
	cfg->ttl_positive = ap_strtol(arg, 0, 10);
    }
    return NULL;
}

static const char *set_ttl_negative(cmd_parms *cmd, void *__unused__, char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
						&myuserdir_module);
    if (arg && strlen(arg) > 0) {
	cfg->ttl_negative = ap_strtol(arg, 0, 10);
    }
    return NULL;
}
#endif

static const command_rec myuserdir_cmds[] = {
    {"MyUserOn", set_module_onoff, NULL, RSRC_CONF, FLAG, "Turn on Apache MySQL Vuser on this server"},
    {"MyUserDbHost", set_host, NULL, RSRC_CONF, TAKE1, "Set hostname for MySQL server"},
    {"MyUserDbName", set_dbname, NULL, RSRC_CONF, TAKE1, "Set database to connect"},
    {"MyUserDbUser", set_user, NULL, RSRC_CONF, TAKE1, "Set username for database"},
    {"MyUserDbPass", set_pass, NULL, RSRC_CONF, TAKE1, "Set password for database"},
    {"MyUserDbSocket", set_socket, NULL, RSRC_CONF, TAKE1, "Set MySQL socket to use"},
    {"MyUserQuery", set_myquery, NULL, RSRC_CONF, TAKE1, "The SQL query, it should return homedir and extra php config"},
#ifdef WITH_CACHE
    {"MyUserCacheOn", set_cache_onoff, NULL, RSRC_CONF, FLAG, "Turn on internal caching"},
    {"MyUserTTLPositive", set_ttl_positive, NULL, RSRC_CONF, TAKE1, "Set TTL for cache record"},
    {"MyUserTTLNegative", set_ttl_negative, NULL, RSRC_CONF, TAKE1, "Set TTL for cache record"},
#endif
    {NULL}
};

module MODULE_VAR_EXPORT myuserdir_module =
{
    STANDARD_MODULE_STUFF,
    NULL,			/* initializer */
    NULL,			/* myuserdir_create_dir_config,	/ * dir
				 * config creater */
    NULL,			/* myuserdir_merge_dir_config,	/ * dir
				 * merger --- default is to override */
    myuserdir_create_server_config,	/* server config */
    myuserdir_merge_server_config,	/* merge server configs */
    myuserdir_cmds,		/* command table */
    NULL,			/* handlers */
    myuserdir_translate,	/* filename translation */
    NULL,			/* check_user_id */
    NULL,			/* check auth */
    NULL,			/* check access */
    NULL,			/* type_checker */
    NULL,			/* fixups */
    NULL,			/* logger */
    NULL,			/* header parser */
    myuserdir_child_init,	/* child_init */
    myuserdir_child_exit,	/* child_exit */
    NULL,			/* post read-request */
#ifdef EAPI
    NULL,			/* EAPI: add_module */
    NULL,			/* EAPI: remove_module */
    NULL,			/* EAPI: rewrite_command */
    NULL,			/* EAPI: new_connection */
#endif
};
