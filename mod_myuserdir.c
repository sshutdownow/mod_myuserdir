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


#include <sys/types.h>
#include <sys/stat.h>

#include "ap_config.h"
#include "httpd.h"
#include "http_request.h"
#include "http_config.h"
#include "http_core.h"
#include "http_log.h"

#include "apr.h"
#include "apr_pools.h"
#include "apr_strings.h"

#ifdef WITH_CACHE
#include "apr_hash.h"
#endif	/* WITH_CACHE */

#include <mysql.h>
#include "escape_sql.h"

#ifdef WITH_PHP
#include "mod_myuserdir_php.h"
#endif

#define apr_block_alarms()
#define apr_unblock_alarms()

#if !defined(__unused)

#if defined(__GNUC__) && (__GNUC__ > 2 || __GNUC__ == 2 && __GNUC_MINOR__ >= 7)
#define __unused __attribute__((__unused__))
#else
#define __unused
#endif

#endif /* __unused */

static const char __unused cvsid[] = "$Id: mod_myuserdir.c 30 2007-08-06 19:49:34Z igor_popov $";

module AP_MODULE_DECLARE_DATA myuserdir_module;

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
    apr_pool_t *pool;
    apr_hash_t *cache;
    time_t ttl_positive;
    time_t ttl_negative;
#endif				/* WITH_CACHE */
} myuserdir_cfg_t;

#ifdef WITH_CACHE		/* experimental internal cache support */

typedef struct {
    char *homedir;
#ifdef WITH_PHP
    char *php_ini_conf;
#endif
    int hits;			/* negative means user not found or blocked */
    time_t access_time;
} cache_t, *p_cache_t;

static p_cache_t cache_userdir_find(myuserdir_cfg_t *cfg, const char *username)
{
    p_cache_t userdir;
    time_t cur;

    if (!cfg->cache_enabled) {
        return NULL;
    }

    userdir = apr_hash_get(cfg->cache, username, APR_HASH_KEY_STRING);
    if (!userdir) {
        return NULL;
    }

    cur = time(NULL);

    if (userdir->hits > 0 /* && userdir->hits < 512 */ && userdir->access_time + cfg->ttl_positive >= cur) {
        userdir->hits++;
    }
    else if (userdir->hits < 0 /* && userdir->hits > -256 */ && userdir->access_time + cfg->ttl_negative >= cur) {
        userdir->hits--;
    }
    else {			/* expired */
        apr_hash_set(cfg->cache, username, APR_HASH_KEY_STRING, NULL);	/* delete hash entry */
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
    userdir = apr_pcalloc(cfg->pool, sizeof(cache_t));
    userdir->access_time = time(NULL);
    userdir->homedir = apr_pstrdup(cfg->pool, homedir);
#ifdef WITH_PHP
    userdir->php_ini_conf = apr_pstrdup(cfg->pool, php_ini_conf);
#endif
    userdir->hits = hits;
    apr_hash_set(cfg->cache, username, APR_HASH_KEY_STRING, userdir);
}

static void cache_userdir_del(myuserdir_cfg_t *cfg, apr_hash_t * cache, const char *host)
{
    if (!cfg->cache_enabled) {
        return;
    }
    apr_hash_set(cache, host, APR_HASH_KEY_STRING, NULL);	/* delete hash entry */
}

#endif				/* WITH_CACHE */


static int myuserdir_setup(server_rec *s)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(s->module_config, &myuserdir_module);

    cfg->mysql_connected = 0;

    if (!cfg->myuserdir_enabled) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, s,
                     "MyUserdir is disabled, but tried to connect to MySQL server");
        return -1;
    }
    /* This should never ever ever happen */
    if (!cfg->mysql) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, s, "MySQL handle is NULL");
        return -1;
    }

    if (!mysql_real_connect(cfg->mysql, cfg->mysql_host, cfg->mysql_user,
                            cfg->mysql_pass, cfg->mysql_dbname, cfg->mysql_inetsock, cfg->mysql_unixsock, 0)) {
        ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, s,
                     "failed to connect to database '%s': %s", cfg->mysql_dbname, mysql_error(cfg->mysql));
        cfg->mysql_connected = 0;
        return -1;
    }
#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, s, "MyUserdir connected to MySQL");
#endif
    cfg->mysql_connected = 1;

    return 0;
}

static int myuserdir_init(apr_pool_t *p __unused, apr_pool_t *plog __unused, apr_pool_t *ptemp __unused, server_rec *s)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(s->module_config, &myuserdir_module);
    return OK;
}

static void *myuserdir_create_server_config(apr_pool_t * p, server_rec *s __unused)
{
    myuserdir_cfg_t *cfg = (myuserdir_cfg_t *)apr_pcalloc(p, sizeof(myuserdir_cfg_t));

#ifdef WITH_CACHE
    cfg->ttl_positive = 300;
    cfg->ttl_negative = 180;
#endif				/* WITH_CACHE */

    return (void *)cfg;
}

static void *myuserdir_merge_server_config(apr_pool_t * p, void *base, void *override)
{
    myuserdir_cfg_t *new_conf = (myuserdir_cfg_t *)apr_pcalloc(p, sizeof(myuserdir_cfg_t));
    myuserdir_cfg_t *base_conf = (myuserdir_cfg_t *)base;
    myuserdir_cfg_t *override_conf = (myuserdir_cfg_t *)override;

    new_conf->mysql = apr_pcalloc(p, sizeof(MYSQL));
    mysql_init(new_conf->mysql);

    new_conf->myuserdir_enabled = (override_conf->myuserdir_enabled == 1) ? 1 : 0;
    new_conf->mysql_connected = 0;

    new_conf->mysql_host = (override_conf->mysql_host == NULL) ?
                           base_conf->mysql_host : override_conf->mysql_host;

    new_conf->mysql_user = (override_conf->mysql_user == NULL) ?
                           base_conf->mysql_user : override_conf->mysql_user;

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
    apr_pool_create(&new_conf->pool, p);
    new_conf->cache = apr_hash_make(new_conf->pool);

    new_conf->ttl_positive = (override_conf->ttl_positive == 0) ?
                             base_conf->ttl_positive : override_conf->ttl_positive;
    new_conf->ttl_negative = (override_conf->ttl_negative == 0) ?
                             base_conf->ttl_negative : override_conf->ttl_negative;
#endif
    return new_conf;
}

static apr_status_t myuserdir_child_exit(void *s)
{
    myuserdir_cfg_t *cfg =
        (myuserdir_cfg_t *)ap_get_module_config( ((server_rec *)s)->module_config, &myuserdir_module);

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, s, "child exit");
#endif

    if (cfg->mysql_connected && cfg->mysql) {
        mysql_close(cfg->mysql);
    }

#ifdef WITH_CACHE
    apr_pool_destroy(cfg->pool);
#endif
    return APR_SUCCESS;
}

static void myuserdir_child_init(apr_pool_t *p, server_rec *s)
{
    myuserdir_cfg_t *cfg =
        ap_get_module_config(s->module_config, &myuserdir_module);

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, s, "child init");
#endif
#ifdef WITH_CACHE
    apr_pool_create(&cfg->pool, NULL);
    cfg->cache = apr_hash_make(cfg->pool);
#endif
    cfg->mysql = apr_pcalloc(p, sizeof(MYSQL));
    mysql_init(cfg->mysql);
#if 1
    cfg->mysql_connected = 0;
#else
    myuserdir_setup(s);
#endif
    apr_pool_cleanup_register(p, s, myuserdir_child_exit, myuserdir_child_exit);
}



static apr_status_t cleanup_mysql_result(void *result)
{
    if (result) {
        mysql_free_result((MYSQL_RES *)result);
        result = 0;
    }
    return APR_SUCCESS;
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

    /*if (zend_restore_ini_entry("open_basedir", sizeof(),) < 0) {
    ;
    }*/

    if (!cfg->myuserdir_enabled) {
        return DECLINED;
    }

    if ((!cfg->mysql_connected || mysql_ping(cfg->mysql)) && myuserdir_setup(r->server) < 0) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r, "declined: lost connection to mysql");
        return DECLINED;
    }

    if (!cfg->mysql_user_query) {	/* it is seemed to be redundant, but
					 * it should be there */
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r, "declined: !mysql_user_query");
        return DECLINED;
    }

    if (r->uri == 0 || r->uri[0] != '/' || r->uri[1] != '~') {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "declined: uri '%s' doesn't begin with '/~'", r->uri);
#endif
        return DECLINED;
    }

    dname = apr_pstrdup(r->pool, r->uri + 2);	/* skip '/~' */

    if (!dname || !*dname) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
                      "declined: uri '%s' has no username", r->uri);
        return DECLINED;
    }

    if (ap_ind(dname, '/') == -1) {	/* redirect /~user => /~user/ */
        char *redirect = apr_pstrcat(r->pool, r->uri, "/", NULL);

        apr_table_setn(r->headers_out, "Location", redirect);
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "redirect: '%s' to '%s'", r->uri, redirect);
#endif
        return HTTP_MOVED_TEMPORARILY;
    }

    name = ap_getword(r->pool, (const char **)&dname, '/');

    /* If there is .. it is not for us */
    if (name[0] == '\0' || (name[1] == '.' && (name[2] == '\0' || (name[2] = '.' && name[3] == '\0')))) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
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
            php_ini_conf = apr_pstrdup(r->pool, userdir->php_ini_conf);
#endif
#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "cache: user '%s' found in positive cache", name);
#endif
            goto USER_FOUND;	/* but I don't like goto */
        }
        else if (userdir->hits < 0) {
#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "cache: user '%s' found in negative cache", name);
#endif
            return HTTP_NOT_FOUND;
        }
    }
#endif				/* WITH_CACHE */

    name_len = strlen(name);
    safe_name = apr_pcalloc(r->pool, name_len * 2 + 1);	/* escape all chars */
#if 1
    escape_sql(name, name_len, safe_name, name_len * 2 + 1);
#else
#ifdef HAVE_MYSQL_REAL_ESCAPE_STRING
    mysql_real_escape_string(cfg->mysql, safe_name, name, name_len);
#else
    mysql_escape_string(safe_name, name, name_len);
#endif
#endif
    query = apr_psprintf(r->pool, cfg->mysql_user_query, safe_name, NULL);

    apr_block_alarms();		/* to prevent memleaks from mysql library */

    if (mysql_real_query(cfg->mysql, query, strlen(query))) {	/* query failed */
        apr_unblock_alarms();
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
                      "declined: error in sql query '%s' %s", query, mysql_error(cfg->mysql));
        apr_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "QUERY_ERROR");
        return DECLINED;
    }

    /* we have data */
    res_set = mysql_store_result(cfg->mysql);
    apr_pool_cleanup_register(r->pool, (void *)res_set, cleanup_mysql_result, &apr_pool_cleanup_null);

    apr_unblock_alarms();

    row = mysql_fetch_row(res_set);
    if (!row) {
        apr_block_alarms();
        {
	    apr_pool_cleanup_run(r->pool, (void *)res_set, &cleanup_mysql_result);
#ifdef WITH_CACHE
#ifdef WITH_PHP
            cache_userdir_add(cfg, name, 0, 0, -1);
#else
            cache_userdir_add(cfg, name, 0, -1);
#endif
#endif
        }
        apr_unblock_alarms();
#if defined(WITH_CACHE) && defined(DEBUG)
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "cache: user '%s' added to negative cache", name);
#endif				/* WITH_CACHE && DEBUG */
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
                      "http_not_found: user '%s' not found", name);
        apr_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "USER_NOT_FOUND");
        return HTTP_NOT_FOUND;	/* DECLINED */
    }

    if ((num_fields_fetched = mysql_num_fields(res_set)) > 0) {
	apr_finfo_t finfo;
        
	switch (num_fields_fetched) {
        default:
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
                          "there are too many fields (%d) in mysql response", num_fields_fetched);
#ifdef WITH_PHP
        case 2:
            if (row[1]) {
                php_ini_conf = apr_pstrdup(r->pool, row[1]);
            }
#endif				/* WITH_PHP */
        case 1:
            if (row[0]) {
                homedir = apr_pstrdup(r->pool, row[0]);
            }
        }

        apr_block_alarms();	/* to avoid memleaks */
        {
	    apr_pool_cleanup_run(r->pool, (void *)res_set, &cleanup_mysql_result);
        }
        apr_unblock_alarms();

USER_FOUND:
        if (!homedir) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r,
                          "declined: no homedir for user '%s'", name);
            return DECLINED;
        }

        if (!ap_is_directory(r->pool, homedir)) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, 0, r,
                          "declined: homedir '%s' is not dir at all", homedir);
            apr_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "WRONG_HOMEDIR");
            return DECLINED;
        }

#ifdef WITH_CACHE
        if (!userdir) {		/* not found in cache */
            apr_block_alarms();	/* to not break cache */
            {
#ifdef WITH_PHP
                cache_userdir_add(cfg, name, homedir, php_ini_conf, 1);
#else
                cache_userdir_add(cfg, name, homedir, 1);
#endif
            }
            apr_unblock_alarms();
#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "cache: user '%s' added to positive cache", name);
#endif				/* DEBUG */
        }
#endif

        if (dname) {
            r->filename = apr_pstrcat(r->pool, homedir, "/", dname, NULL);
        } else {
            r->filename = homedir;
        }

        ap_no2slash(r->filename);

        if (r->filename &&
	    apr_stat(&finfo, r->filename, APR_FINFO_MIN, r->pool) == APR_SUCCESS)
	{
	    r->finfo = finfo;
        } else {
#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "http_not_found: file '%s' doesn't exist", r->filename);
#endif				/* DEBUG */
            r->filename = 0;
            return HTTP_NOT_FOUND;
        }

#ifdef WITH_PHP
        apr_table_setn(r->subprocess_env, "PHP_DOCUMENT_ROOT", homedir);
        if (zend_alter_ini_entry("open_basedir", sizeof("open_basedir"), homedir, strlen(homedir), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, 0, r, "setting 'open_basedir' to '%s' failed", homedir);
        }

        if (zend_alter_ini_entry("safe_mode", sizeof("safe_mode"), "1", 1, PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, 0, r, "turning on 'safe_mode' failed");
        }

        if (php_ini_conf) {	/* there is an extra php config */
            char *linend, *value;

#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "php extra config is '%s'", php_ini_conf);
#endif
            while ((linend = strchr(php_ini_conf, ';')) != NULL) {

                *linend++ = '\0';
                value = strchr(php_ini_conf, '=');
                if ((value = strchr(php_ini_conf, '=')) != NULL) {
                    *value++ = '\0';
#ifdef DEBUG
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "setting '%s' to '%s'", php_ini_conf, value);
#endif
                    if (zend_alter_ini_entry(php_ini_conf, strlen(php_ini_conf) + 1, value, strlen(value), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
                        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r, "setting '%s' to '%s' failed", php_ini_conf, value);
                    }
                }
                if (linend) {
                    php_ini_conf = linend;
                }
            }

            if (php_ini_conf && *php_ini_conf && (value = strchr(php_ini_conf, '=')) != NULL) {
                *value++ = '\0';
#ifdef DEBUG
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "setting php param '%s' to value '%s'", php_ini_conf, value);
#endif
                if (zend_alter_ini_entry(php_ini_conf, strlen(php_ini_conf) + 1, value, strlen(value), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r, "setting '%s' to '%s' failed", php_ini_conf, value);
                }
            }
        }
#endif				/* WITH_PHP */

#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "OK: translate '%s' to '%s'", r->uri, r->filename);
#endif
        return OK;
    }

    /* not for us */
#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "declined");
#endif
    return DECLINED;
}

/*
* config stuff
*/
static const char *set_socket(cmd_parms *cmd, void *p1 __unused, const char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config,
                           &myuserdir_module);

    cfg->mysql_unixsock = 0;
    cfg->mysql_inetsock = 0;
    if (arg && strlen(arg) > 0) {
        if (arg[0] == '/') {
            cfg->mysql_unixsock = apr_pstrdup(cmd->pool, arg);
        } else {
            cfg->mysql_inetsock = strtol(arg, 0, 10);
        }
    }
    return NULL;
}


/* Dispatch list of content handlers */
static const command_rec myuserdir_cmds[] = {
    AP_INIT_FLAG("MyUserOn", ap_set_flag_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, myuserdir_enabled), RSRC_CONF, "Turn on Apache MySQL Vuser on this server"),
    AP_INIT_TAKE1("MyUserDbHost", ap_set_string_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, mysql_host), RSRC_CONF, "Set hostname for MySQL server"),
    AP_INIT_TAKE1("MyUserDbName", ap_set_string_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, mysql_dbname), RSRC_CONF, "Set database to connect"),
    AP_INIT_TAKE1("MyUserDbUser", ap_set_string_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, mysql_user), RSRC_CONF, "Set username for database"),
    AP_INIT_TAKE1("MyUserDbPass", ap_set_string_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, mysql_pass), RSRC_CONF, "Set password for database"),
    AP_INIT_TAKE1("MyUserDbSocket", set_socket, NULL, RSRC_CONF, "Set MySQL socket to use"),
    AP_INIT_TAKE1("MyUserQuery", ap_set_string_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, mysql_user_query), RSRC_CONF, "The SQL query, it should return homedir and extra php config"),
#ifdef WITH_CACHE
    AP_INIT_FLAG("MyUserCacheOn", ap_set_flag_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, cache_enabled), RSRC_CONF, "Turn on internal caching"),
    AP_INIT_TAKE1("MyUserTTLPositive", ap_set_int_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, ttl_positive), RSRC_CONF, "Set TTL for cache record"),
    AP_INIT_TAKE1("MyUserTTLNegative", ap_set_int_slot, (void*)APR_OFFSETOF(myuserdir_cfg_t, ttl_negative), RSRC_CONF, "Set TTL for cache record"),
#endif
    {NULL},
};

static void register_hooks(apr_pool_t * pool __unused)
{
    ap_hook_post_config(myuserdir_init, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(myuserdir_child_init, NULL, NULL, APR_HOOK_MIDDLE);
	
    ap_hook_translate_name(myuserdir_translate,
	(const char * const []){ "mod_alias.c", NULL },
	(const char * const []){ "mod_vhost_alias.c", NULL },
	APR_HOOK_MIDDLE);
/*
#ifdef HAVE_UNIX_SUEXEC
    ap_hook_get_suexec_identity(get_suexec_id_doer,NULL,NULL,APR_HOOK_FIRST);
#endif
*/
}


/* Dispatch list for API hooks */
module AP_MODULE_DECLARE_DATA myuserdir_module = {
    STANDARD20_MODULE_STUFF,
    NULL,			/* create per-dir    config structures */
    NULL,			/* merge  per-dir    config structures */
    myuserdir_create_server_config,	/* create per-server config structures */
    myuserdir_merge_server_config,	/* merge  per-server config structures */
    myuserdir_cmds,		/* table of config file commands       */
    register_hooks		/* register hooks                      */
};
