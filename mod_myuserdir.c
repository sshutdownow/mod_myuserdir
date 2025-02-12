/*
   Copyright (c) 2005-2025 Igor Popov <ipopovi@gmail.com>

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
#include "apr_hash.h"
#include "apr_dbd.h"
#include "mod_dbd.h"

#ifdef WITH_PHP
#include "mod_myuserdir_php.h"
#endif


#if !defined(__unused)

#if defined(__GNUC__) && (__GNUC__ > 2 || __GNUC__ == 2 && __GNUC_MINOR__ >= 7)
#define __unused __attribute__((__unused__))
#else
#define __unused
#endif

#endif /* __unused */

static const char __unused cvsid[] = "$Id: mod_myuserdir.c 38 2008-07-15 07:27:33Z igor_popov $";

module AP_MODULE_DECLARE_DATA myuserdir_module;


typedef struct {
    int is_module_enabled;
    int is_bdb_connected;
    char *bdb_query; /* SQL query itself */
    char *bdb_label; /* DBD prepared statement label for SQL query */
} myuserdir_cfg_t;


/* import optional functions from mod_dbd */
static APR_OPTIONAL_FN_TYPE(ap_dbd_prepare) *dbd_prepare_fn = NULL;
static APR_OPTIONAL_FN_TYPE(ap_dbd_acquire) *dbd_acquire_fn = NULL;


static void *create_server_config(apr_pool_t * p, server_rec *s __unused)
{
    myuserdir_cfg_t *cfg = (myuserdir_cfg_t *)apr_pcalloc(p, sizeof(myuserdir_cfg_t));

    if (!dbd_prepare_fn) {
        ap_assert(dbd_prepare_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_prepare));
        ap_assert(dbd_acquire_fn = APR_RETRIEVE_OPTIONAL_FN(ap_dbd_acquire));
    }

    return (void *)cfg;
}

static void *merge_server_config(apr_pool_t * p, void *base, void *override)
{
    myuserdir_cfg_t *new_conf = (myuserdir_cfg_t *)apr_pcalloc(p, sizeof(myuserdir_cfg_t));
    myuserdir_cfg_t *base_conf = (myuserdir_cfg_t *)base;
    myuserdir_cfg_t *override_conf = (myuserdir_cfg_t *)override;

    new_conf->is_module_enabled = (override_conf->is_module_enabled == 1) ? 1 : 0;
    
    new_conf->bdb_label = (override_conf->bdb_label == NULL) ?
			    base_conf->bdb_label : override_conf->bdb_label;
    
    new_conf->bdb_query = (override_conf->bdb_query == NULL) ?
			    base_conf->bdb_query : override_conf->bdb_query;
    return new_conf;
}


/* main */
static int translate(request_rec *r)
{
    myuserdir_cfg_t *cfg =
        ap_get_module_config(r->server->module_config, &myuserdir_module);

    char *name = 0, *homedir_hash_key = 0;
    const char *dname = 0, *homedir = 0;
    ap_dbd_t *dbd;
    apr_finfo_t finfo;
#ifdef WITH_PHP
    const char *php_ini_conf = 0, *phpconf_hash_key = 0;
#endif


    if (!cfg->is_module_enabled) {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "declined: module disabled");
#endif
        return DECLINED;
    }

    if (!cfg->bdb_label) {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "declined: no bdb_label");
#endif
        return DECLINED;
    }

    if (r->proxyreq) {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "forbidden: proxy request");
#endif
        return HTTP_FORBIDDEN;
    }

    if (!cfg->bdb_query) {	/* it is seemed to be redundant, but it should be there */
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r, "declined: !bdb_query");
        return DECLINED;
    }

    if (r->uri == 0 || r->uri[0] != '/') {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r,
                      "bad request: '%s'", r->the_request);
        return HTTP_BAD_REQUEST;
    }

    if (r->uri[1] != '~') {
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

    dbd = dbd_acquire_fn(r);
    if (!dbd) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_CRIT, 0, r,
                      "acquiring connection to database failed");
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    /* try get cached result */
#ifdef WITH_PHP
    phpconf_hash_key = apr_pstrcat(r->pool,
		"DBD:tildeUserPHPConfKey=",
		apr_dbd_escape(dbd->driver, r->pool, name, dbd->handle),
		"\t",
		apr_dbd_escape(dbd->driver, r->pool, (r->hostname? r->hostname : "default"), dbd->handle), NULL);
    php_ini_conf = apr_table_get(r->connection->notes, phpconf_hash_key);
#endif
    homedir_hash_key = apr_pstrcat(r->pool,
		"DBD:tildeUserHomedirKey=",
		apr_dbd_escape(dbd->driver, r->pool, name, dbd->handle),
		"\t",
		apr_dbd_escape(dbd->driver, r->pool, (r->hostname? r->hostname : "default"), dbd->handle), NULL);
    
    homedir = apr_table_get(r->connection->notes, homedir_hash_key);
    if (!homedir) {
        int rv, rows = 0, cols = 0, nparams = 1;
	const char **params = (char[1][1]){ {'\0'} };
        apr_dbd_prepared_t *stmt = NULL;
        apr_dbd_row_t *row = NULL;
        apr_dbd_results_t *res = NULL;


#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "there is no cached data for name '%s'", name);
#endif

        stmt = apr_hash_get(dbd->prepared, cfg->bdb_label, APR_HASH_KEY_STRING);
        if (!stmt) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_CRIT, 0, r,
                          "unable to retrieve prepared statement '%s'", cfg->bdb_label);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

	params[0] = apr_dbd_escape(dbd->driver, r->pool, name, dbd->handle);
        if ((rv = apr_dbd_pselect(dbd->driver, r->pool, dbd->handle, &res, stmt,
                             0, nparams, params))) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_CRIT, 0, r,
                          "unable to execute SQL statement: '%s'",
                          apr_dbd_error(dbd->driver, dbd->handle, rv));
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        rows = apr_dbd_num_tuples(dbd->driver, res);
        if (!rows) {
#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "prepared sql statement '%s' returned zero rows",
                          cfg->bdb_label);
#endif
            return DECLINED;
        } else if (rows > 1) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
                          "prepared sql statement '%s' returned too many rows '%d'",
                          cfg->bdb_label, rows);
            /* avoid mem leaks */
            while (!apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1)) {
                continue;
            }
            return HTTP_INTERNAL_SERVER_ERROR;
        } else if (rows < 0) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "prepared sql statement '%s' returned negative number of rows '%d'",
                          cfg->bdb_label, rows);
        }

        cols = apr_dbd_num_cols(dbd->driver, res);
        if (!cols) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_CRIT, 0, r,
                          "server_error: prepared sql statement '%s' returned no columns",
                          cfg->bdb_label);
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        if (apr_dbd_get_row(dbd->driver, r->pool, res, &row, -1) < 0) {
            if (rows < 0) {
        	ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r,
                          "declined: prepared sql statement '%s' returned negative number of rows '%d'",
                          cfg->bdb_label, rows);
                return DECLINED;
            } else {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_CRIT, 0, r,
                              "server_error: unable to fetch 1st row of '%d' rows (stmt '%s')",
                              rows, cfg->bdb_label);
                return HTTP_INTERNAL_SERVER_ERROR;
            }
        }

        homedir = apr_dbd_get_entry(dbd->driver, row, 0);
        if (!homedir || !homedir[0]) {
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ERR, 0, r,
                          "forbidden: homedir is empty");
            return HTTP_FORBIDDEN;
        }

	if (cols >= 2 && (php_ini_conf = apr_dbd_get_entry(dbd->driver, row, 1)) == 0) {
    	    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                          "no php settings returned for user '%s'", name);
	}    
    }
    


    if (!ap_is_directory(r->pool, homedir)) {
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_ALERT, 0, r,
                      "declined: homedir '%s' is not dir at all", homedir);
        apr_table_setn(r->subprocess_env, "MYUSERDIR_ERR", "WRONG_HOMEDIR");
        return DECLINED;
    }

    if (dname) {
        r->filename = apr_pstrcat(r->pool, homedir, "/", dname, NULL);
    } else {
        r->filename = (char*)homedir;
    }

    ap_no2slash(r->filename);

    if (r->filename &&
            apr_stat(&finfo, r->filename, APR_FINFO_MIN, r->pool) == APR_SUCCESS)
    {
        r->finfo = finfo;
	apr_table_set(r->connection->notes, homedir_hash_key, homedir);
#ifdef WITH_PHP
	apr_table_set(r->connection->notes, phpconf_hash_key, php_ini_conf);
#endif
	
    } else {
#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r,
                      "not_found: file '%s' doesn't exist", r->filename);
#endif
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

    if (php_ini_conf && strlen(php_ini_conf)) {	/* there is an extra php config */
        char *linend, *value;

#ifdef DEBUG
        ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "php extra config is '%s'", php_ini_conf);
#endif
        while ((linend = ap_strchr_c(php_ini_conf, ';')) != NULL) {

            *linend++ = '\0';
            value = ap_strchr_c(php_ini_conf, '=');
            if ((value = ap_strchr_c(php_ini_conf, '=')) != NULL) {
                *value++ = '\0';
#ifdef DEBUG
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "try to set '%s' to '%s'", php_ini_conf, value);
#endif
                if (zend_alter_ini_entry(php_ini_conf, strlen(php_ini_conf) + 1, value, strlen(value), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
                    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r, "setting '%s' to '%s' failed", php_ini_conf, value);
                }
            }
            if (linend) {
                php_ini_conf = linend;
            }
        }

        if (php_ini_conf && *php_ini_conf && (value = ap_strchr_c(php_ini_conf, '=')) != NULL) {
            *value++ = '\0';
#ifdef DEBUG
            ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "trying to set php param '%s' to value '%s'", php_ini_conf, value);
#endif
            if (zend_alter_ini_entry(php_ini_conf, strlen(php_ini_conf) + 1, value, strlen(value), PHP_INI_SYSTEM, PHP_INI_STAGE_RUNTIME) < 0) {
                ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_WARNING, 0, r, "setting '%s' to '%s' failed", php_ini_conf, value);
            }
        }
    }
#endif /* WITH_PHP */

#ifdef DEBUG
    ap_log_rerror(APLOG_MARK, APLOG_NOERRNO | APLOG_DEBUG, 0, r, "OK: translate '%s' to '%s'", r->uri, r->filename);
#endif
    return OK;
}


/*
* config stuff
*/
static const char *set_module_onoff(cmd_parms *cmd, void *p __unused, int flag)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config, &myuserdir_module);
    const char *err = ap_check_cmd_context(cmd, NOT_IN_LIMIT|NOT_IN_DIR_LOC_FILE);
    if (err) {
        return err;
    }
    cfg->is_module_enabled = (flag ? 1 : 0);
    return NULL;
}

static const char *set_bdb_query(cmd_parms *cmd, void *p __unused, const char *arg)
{
    myuserdir_cfg_t *cfg = ap_get_module_config(cmd->server->module_config, &myuserdir_module);
    const char *err = ap_check_cmd_context(cmd, NOT_IN_LIMIT|NOT_IN_DIR_LOC_FILE);
    if (err) {
        return err;
    }

    if (!dbd_prepare_fn || !dbd_acquire_fn) {
        return "mod_dbd must be enabled to use mod_myuserdir";
    }

    if (!arg || !strlen(arg)) {
        return "MyUserQuery must be set";
    }

    cfg->bdb_label = apr_pstrcat(cmd->pool, "mod_myuserdir_for_",
                                 (cmd->server->is_virtual && cmd->server->server_hostname) ? cmd->server->server_hostname : "default", NULL);
    dbd_prepare_fn(cmd->server, arg, cfg->bdb_label);
    cfg->bdb_query = apr_pstrdup(cmd->pool, arg);

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, cmd->server,
                 "query prepared from '%s' to '%s', bdb_label is '%s'",
                 arg, cfg->bdb_query, cfg->bdb_label);
#endif /* DEBUG */

    return NULL;
}


/* Dispatch list of content handlers */
static const command_rec cmds[] = {
    AP_INIT_FLAG("MyUserOn", set_module_onoff, NULL, RSRC_CONF, "Turn on /~user translation for this [virtual] server"),
    AP_INIT_TAKE1("MyUserQuery", set_bdb_query, NULL, RSRC_CONF, "The SQL query, it should return homedir and extra php config"),
    {NULL},
};

static void register_hooks(apr_pool_t *pool __unused)
{
    static const char * const aszPre[] = { "mod_alias.c", NULL };
    static const char * const aszSucc[] = { "mod_vhost_alias.c", "mod_dbd.c", "mod_php4.c", "mod_php5.c", NULL };
    
    ap_hook_translate_name(translate, aszPre, aszSucc, APR_HOOK_MIDDLE);
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
    create_server_config,	/* create per-server config structures */
    merge_server_config,	/* merge  per-server config structures */
    cmds,			/* table of config file commands       */
    register_hooks		/* register hooks                      */
};
