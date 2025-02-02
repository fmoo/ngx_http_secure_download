#include <ngx_core.h>
#include <ngx_string.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mhash.h>
#include <openssl/md5.h>
#include <ctype.h>

#define FOLDER_MODE 0
#define FILE_MODE 1
#define COMPLEX_MODE 2

typedef struct {
  const char *timestamp;
  const char *md5;
  const char *path;
  int path_len;
  int path_to_hash_len;
  ngx_str_t parsed_expiry;
  ngx_str_t parsed_hash;
} ngx_http_secure_download_split_uri_t;

static ngx_int_t ngx_http_secure_download_split_uri (ngx_http_request_t*, ngx_http_secure_download_split_uri_t*);
static ngx_int_t ngx_http_secure_download_check_hash(ngx_http_request_t*, ngx_http_secure_download_split_uri_t*, ngx_str_t*);
static void * ngx_http_secure_download_create_loc_conf(ngx_conf_t*);
static char * ngx_http_secure_download_merge_loc_conf (ngx_conf_t*, void*, void*);
static ngx_int_t ngx_http_secure_download_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_secure_download_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
static char * ngx_conf_secure_download_set_path_mode(ngx_conf_t*, ngx_command_t*, void*);
static char * ngx_conf_secure_download_set_secrets(ngx_conf_t*, ngx_command_t*, void*);

typedef struct {
  ngx_flag_t enable;
  ngx_flag_t path_mode;
  ngx_flag_t path_override;
  ngx_array_t *secret_cvs;
  ngx_http_complex_value_t hash_cv;
  ngx_http_complex_value_t expires_cv;
  ngx_http_complex_value_t path_cv;
} ngx_http_secure_download_loc_conf_t;

static ngx_command_t ngx_http_secure_download_commands[] = {
  {
    ngx_string("secure_download"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, enable),
    NULL
  },

  {
    ngx_string("secure_download_path_mode"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE13|NGX_CONF_TAKE4,
    ngx_conf_secure_download_set_path_mode,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL
  },

  {
    ngx_string("secure_download_secret"),
    NGX_HTTP_LOC_CONF|NGX_CONF_1MORE,
    ngx_conf_secure_download_set_secrets,
    NGX_HTTP_LOC_CONF_OFFSET,
    0,
    NULL
  }
};

static ngx_http_module_t ngx_http_secure_download_module_ctx = {
  ngx_http_secure_download_add_variables,
  NULL,

  NULL,
  NULL,

  NULL,
  NULL,

  ngx_http_secure_download_create_loc_conf,
  ngx_http_secure_download_merge_loc_conf
};

ngx_module_t ngx_http_secure_download_module = {
  NGX_MODULE_V1,
  &ngx_http_secure_download_module_ctx,
  ngx_http_secure_download_commands,
  NGX_HTTP_MODULE,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NGX_MODULE_V1_PADDING
};

static ngx_str_t  ngx_http_secure_download = ngx_string("secure_download");

static char * ngx_conf_secure_download_set_path_mode(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_compile_complex_value_t   ccv;
  ngx_str_t *d = cf->args->elts;
  ngx_http_secure_download_loc_conf_t *sdlc = conf;

  if ((d[1].len == 6) && (strncmp((char*)d[1].data, "folder", 6) == 0))
  {
    if (cf->args->nelts != 2) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "incorrect number of arguments for folder.  Expected none, got %d",
                         cf->args->nelts - 2);
      return NGX_CONF_ERROR;
    }
    sdlc->path_mode = FOLDER_MODE;
  }
  else if((d[1].len == 4) && (strncmp((char*)d[1].data, "file", 4) == 0))
  {
    if (cf->args->nelts != 2) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "incorrect number of arguments for file.  Expected none, got %d",
                         cf->args->nelts - 2);
      return NGX_CONF_ERROR;
    }
    sdlc->path_mode = FILE_MODE;
  }
  else if((d[1].len == 7) && (strncmp((char*)d[1].data, "complex", 7) == 0))
  {
    if (cf->args->nelts != 4 && cf->args->nelts != 5) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "incorrect number of arguments for complex.  "
                         "Expected 2-3, got %d", cf->args->nelts - 2);
      return NGX_CONF_ERROR;
    }

    sdlc->path_mode = COMPLEX_MODE;

    // Extract hash_cv
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf = cf;
    ccv.value = &(d[2]);
    ccv.complex_value = &sdlc->hash_cv;
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
    if (ccv.complex_value->lengths == NULL) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "invalid hash; complex value is required: \"%V\"",
                         &d[2]);
      return NGX_CONF_ERROR;
    }

    // Extract expires_cv
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf = cf;
    ccv.value = &(d[3]);
    ccv.complex_value = &sdlc->expires_cv;
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
    if (ccv.complex_value->lengths == NULL) {
      ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                         "invalid expires; complex value is required: \"%V\"",
                         &d[3]);
      return NGX_CONF_ERROR;
    }

    // Extract path_cv
    if (cf->args->nelts == 5) {
      ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
      ccv.cf = cf;
      ccv.value = &(d[4]);
      ccv.complex_value = &sdlc->path_cv;
      if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
          return NGX_CONF_ERROR;
      }
      if (ccv.complex_value->lengths == NULL) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid path; complex value is required: \"%V\"",
                           &d[4]);
        return NGX_CONF_ERROR;
      }
      sdlc->path_override = 1;
    } else {
      sdlc->path_override = 0;
    }
  }
  else
  {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "secure_download_path_mode should be folder, file, or complex", 0);
    return NGX_CONF_ERROR;
  }
  return NGX_CONF_OK;
}

static char * ngx_conf_secure_download_set_secrets(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_compile_complex_value_t     ccv;
  ngx_http_complex_value_t            *cv;
  ngx_http_secure_download_loc_conf_t *sdlc = conf;
  ngx_uint_t                           i;

  // Allocate space for the complex value
  if (sdlc->secret_cvs == NULL) {
      sdlc->secret_cvs = ngx_array_create(cf->pool, cf->args->nelts - 1,
                                          sizeof(ngx_http_complex_value_t));
      if (sdlc->secret_cvs == NULL) {
          return NGX_CONF_ERROR;
      }
  }

  for (i = 1; i < cf->args->nelts; i++) {
    ngx_str_t *args = cf->args->elts;
    ngx_str_t *arg = &args[i];

    // Get a ptr to cv from the array/pool
    cv = (ngx_http_complex_value_t *)ngx_array_push(sdlc->secret_cvs);

    // Reinit the local compile complex compile value
    ngx_memzero(&ccv, sizeof(ngx_http_compile_complex_value_t));
    ccv.cf = cf;
    ccv.value = arg;
    ccv.complex_value = cv;

    // Compile the value for later
    if (ngx_http_compile_complex_value(&ccv) != NGX_OK) {
        return NGX_CONF_ERROR;
    }
  }

  return NGX_CONF_OK;
}

static void * ngx_http_secure_download_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_secure_download_loc_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_secure_download_loc_conf_t));
  if (conf == NULL) {
    return NGX_CONF_ERROR;
  }
  conf->enable = NGX_CONF_UNSET;
  conf->path_mode = NGX_CONF_UNSET;
  conf->path_override = NGX_CONF_UNSET;
  conf->secret_cvs = NULL;

  return conf;
}

static char * ngx_http_secure_download_merge_loc_conf (ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_secure_download_loc_conf_t *prev = parent;
  ngx_http_secure_download_loc_conf_t *conf = child;

  if (conf->path_mode == NGX_CONF_UNSET && prev->path_mode == COMPLEX_MODE) {
    conf->hash_cv = prev->hash_cv;
    conf->expires_cv = prev->expires_cv;
    conf->path_cv = prev->path_cv;
  }

  ngx_conf_merge_value(conf->path_override, prev->path_override, 0);
  ngx_conf_merge_value(conf->enable, prev->enable, 0);
  ngx_conf_merge_value(conf->path_mode, prev->path_mode, FOLDER_MODE);

  if (conf->secret_cvs == NULL) {
      conf->secret_cvs = prev->secret_cvs;
  }

  if (conf->enable == 1) {
      if (!conf->secret_cvs || conf->secret_cvs->nelts == 0) {
          ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
               "no 'secure_download_secret's specified");
          return NGX_CONF_ERROR;
      }
  }

  return NGX_CONF_OK;
}

static ngx_int_t ngx_http_secure_download_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
  unsigned timestamp;
  unsigned remaining_time = 0;
  ngx_http_secure_download_loc_conf_t *sdc;
  ngx_http_secure_download_split_uri_t sdsu;
  ngx_http_complex_value_t *cvs;
  ngx_str_t secret;
  ngx_uint_t i;
  ngx_flag_t hash_matched;
  int value = 0;

  // Instead of the hash being calculated on every request and routing
  // appropriately, this module invokes the hash calculation lazily when
  // accessing the variable.

  sdc = ngx_http_get_module_loc_conf(r, ngx_http_secure_download_module);
  if (sdc->enable != 1)
  {
      // Secure Download variable was accessed but was not explicitly enabled
      // for this location... oops
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
          "securedownload: module not enabled");
      value = -3;
      goto finish;
  }

  if (!sdc->secret_cvs || !sdc->secret_cvs->nelts) {
      // You didn't set up any secret lengths/values... oops
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
          "securedownload: module enabled, but secret key(s) not configured!");
      value = -3;
      goto finish;
  }

  if (ngx_http_secure_download_split_uri(r, &sdsu) == NGX_ERROR)
  {
    // This splits the hash/timestamp out of the URI *or* evaluates them
    // out using the provided variables.
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "received an error from ngx_http_secure_download_split_uri", 0);
    value = -3;
    goto finish;
  }

  // Attempt to convert the parsed out string timestamp to an integer
  if (sscanf(sdsu.timestamp, "%08X", &timestamp) != 1)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "error in timestamp hex-dec conversion", 0);
    value = -3;
    goto finish;
  }

  // Determine if the URL has expired by comparing the signed expiry time
  // to the current value of time(NULL)
  remaining_time = timestamp - (unsigned) time(NULL);
  if ((int)remaining_time <= 0)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "expired timestamp", 0);
    value = -1;
    goto finish;
  }

  // Let's evaluate some secrets!
  hash_matched = FALSE;
  cvs = sdc->secret_cvs->elts;
  for (i = 0; i < sdc->secret_cvs->nelts; i++) {
    if (ngx_http_complex_value(r, &cvs[i], &secret) != NGX_OK) {
      ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
          "securedownload: evaluation failed");
      value = -3;
      goto finish;
    }

    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
      "securedownload: evaluated value of secret: \"%V\"", &secret);
    if (ngx_http_secure_download_check_hash(r, &sdsu, &secret) == NGX_OK) {
      hash_matched = TRUE;
      value = 0;
      break;
    }
  }

  if (!hash_matched) {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "bad hash", 0);
    value = -2;
    goto finish;
  }

  finish:

  v->not_found = 0;
  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;
  if (value == 0)
  {
    v->data = ngx_pcalloc(r->pool, sizeof(char) * 12);
    if (v->data == NULL) {
        return NGX_ERROR;
    }
    v->len = (int) sprintf((char *)v->data, "%i", remaining_time);
    //printf("valid, %i\n", remaining_time);
  } else {
    v->data = ngx_pcalloc(r->pool, sizeof(char) * 3);
    if (v->data == NULL) {
        return NGX_ERROR;
    }
    v->len = (int) sprintf((char*)v->data, "%i", value);
    //printf("problem %i\n", value);
  }

  return NGX_OK;
}

////////////////////////

static ngx_int_t ngx_http_secure_download_check_hash(ngx_http_request_t *r, ngx_http_secure_download_split_uri_t *sdsu, ngx_str_t *secret)
{
  int i;
  unsigned char generated_hash[16];
  char hash[33];
  MHASH td;
  char *hash_data, *str;
  int data_len;

  static const char xtoc[] = "0123456789abcdef";

  /* rel_path_to_hash/secret/timestamp\0 */

  data_len = sdsu->path_to_hash_len + secret->len + 10;

  hash_data = malloc(data_len + 1);
  if (hash_data == NULL)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "error in allocating memory for string_to_hash.data", 0);
    return NGX_ERROR;
  }

  str = hash_data;
  memcpy(str, sdsu->path, sdsu->path_to_hash_len);
  str += sdsu->path_to_hash_len;
  *str++ = '/';
  memcpy(str, secret->data, secret->len);
  str += secret->len;
  *str++ = '/';
  memcpy(str, sdsu->timestamp, 8);
  str[8] = 0;

  td = mhash_init(MHASH_MD5);

  if (td == MHASH_FAILED)
  {
	free(hash_data);
    return NGX_ERROR;
  }
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hashing string \"%s\" with len %i", hash_data, data_len);
  mhash(td, hash_data, data_len);
  mhash_deinit(td, generated_hash);

  free(hash_data);

  for (i = 0; i < 16; ++i) {
	  hash[2 * i + 0] = xtoc[generated_hash[i] >> 4];
	  hash[2 * i + 1] = xtoc[generated_hash[i] & 0xf];
  }

  hash[32] = 0; //because %.32 doesn't work
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "computed hash: %32s", hash);
  // ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hash from uri: %.32s", sdsu->md5);

  if(memcmp(hash, sdsu->md5, 32) != 0) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hash mismatch", 0);
	  return NGX_ERROR;
  }

  return NGX_OK;
}

static ngx_int_t ngx_http_secure_download_split_uri(ngx_http_request_t *r, ngx_http_secure_download_split_uri_t *sdsu)
{
  int md5_len = 0;
  int tstamp_len = 0;
  int len;
  const char *uri;
  ngx_str_t parsed_path;

  ngx_http_secure_download_loc_conf_t *sdc = ngx_http_get_module_loc_conf(r, ngx_http_secure_download_module);

  // Let's get the URI / length first
  if (sdc->path_mode != COMPLEX_MODE ||
      sdc->path_override == 0) {
    uri = (char*)r->uri.data;
    len = r->uri.len;
  } else {
    if (ngx_http_complex_value(r, &sdc->path_cv, &parsed_path)
        != NGX_OK) {
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "Unable to evaluate path value");
      return NGX_ERROR;
    }
    uri = (char*)parsed_path.data;
    len = parsed_path.len;
  }

  // Parse expiration time
  if (sdc->path_mode != COMPLEX_MODE) {
    while(len && uri[--len] != '/')
      ++tstamp_len;
  } else {
    if (ngx_http_complex_value(r, &sdc->expires_cv, &sdsu->parsed_expiry)
        != NGX_OK) {
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "Unable to evaluate expiry value");
      return NGX_ERROR;
    }
    tstamp_len = sdsu->parsed_expiry.len;
  }
  if(tstamp_len != 8) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "timestamp size mismatch: %d", tstamp_len);
	  return NGX_ERROR;
  }


  if (sdc->path_mode != COMPLEX_MODE) {
    sdsu->timestamp = uri + len + 1;
  } else {
    sdsu->timestamp = (const char *)sdsu->parsed_expiry.data;
  }

  if (sdc->path_mode != COMPLEX_MODE) {
    while(len && uri[--len] != '/')
      ++md5_len;
  } else {
    if (ngx_http_complex_value(r, &sdc->hash_cv, &sdsu->parsed_hash) != NGX_OK) {
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                    "Unable to evaluate hash value");
      return NGX_ERROR;
    }
    md5_len = sdsu->parsed_hash.len;
  }

  if(md5_len != 32) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "md5 size mismatch: %d", md5_len);
	  return NGX_ERROR;
  }
  if (sdc->path_mode != COMPLEX_MODE) {
    sdsu->md5 = uri + len + 1;
  } else {
    sdsu->md5 = (const char *)sdsu->parsed_hash.data;
  }

  if(len == 0) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "bad path", 0);
	  return NGX_ERROR;
  }

  sdsu->path = uri;
  sdsu->path_len = len;

  if(sdc->path_mode == FOLDER_MODE) {
	  while(len && uri[--len] != '/');
  }
  sdsu->path_to_hash_len = len;

  return NGX_OK;
}

static ngx_int_t
ngx_http_secure_download_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_secure_download, NGX_HTTP_VAR_NOHASH);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_secure_download_variable;

    return NGX_OK;
}

