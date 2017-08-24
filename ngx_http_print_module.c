
/* 
 * Copyright (C) Alex Zhang
 *
 * ngx_http_print_module
 *
 * Simple Nginx module for producing HTTP response
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_config.h>


typedef struct {
    ngx_array_t *objects;
    ngx_int_t    interval;
    ngx_int_t    count;
} ngx_http_print_duplicate_t;


typedef struct {
    ngx_array_t *objects;
    ngx_array_t *dup_objects; /* ngx_http_print_duplicate_t */
    ngx_str_t    sep;
    ngx_str_t    ends;
    ngx_int_t    flush;
} ngx_http_print_loc_conf_t;


typedef struct {
    int        rest;
    ngx_uint_t index; /* used when print duplicate objs */
    ngx_buf_t *dup_objects; /* used when print duplicate objs */
} ngx_http_print_ctx_t;


static ngx_int_t ngx_http_print_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_print_duplicate_handler(ngx_http_request_t *r);
static ngx_int_t ngx_http_print_gen_print_buf(ngx_http_request_t *r, ngx_array_t *objects, ngx_buf_t **out);
static ngx_int_t ngx_http_print_process_duplicate(ngx_http_request_t *r);
static void *ngx_http_print_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_print_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static char *ngx_http_print(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static char *ngx_http_print_duplicate(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void ngx_http_print_process_duplicate_event_handler(ngx_event_t *wev);
static void ngx_http_print_process_duplicate_cleanup(void *data);


static ngx_command_t ngx_http_print_commands[] = {
    { ngx_string("print"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_1MORE,
      ngx_http_print,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_print_loc_conf_t, objects),
      NULL },

    { ngx_string("print_sep"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_print_loc_conf_t, sep),
      NULL },

    { ngx_string("print_ends"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_print_loc_conf_t, ends),
      NULL },

    { ngx_string("print_flush"),
      NGX_HTTP_SRV_CONF|NGX_HTTP_SIF_CONF|NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF
                       |NGX_CONF_TAKE1,
      ngx_conf_set_flag_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_print_loc_conf_t, flush),
      NULL },

    { ngx_string("print_duplicate"),
      NGX_HTTP_LOC_CONF|NGX_HTTP_LIF_CONF|NGX_CONF_2MORE,
      ngx_http_print_duplicate,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },
};


/* module ctx */
ngx_http_module_t  ngx_http_print_module_ctx = {
    NULL,                                               /* preconfiguration */
    NULL,                                               /* postconfiguration */

    NULL,                                               /* create main configuration */
    NULL,                                               /* init main configuration */

    NULL,                                               /* create server configuration */
    NULL,                                               /* merge server configuration */

    ngx_http_print_create_loc_conf,                     /* create location configuration */
    ngx_http_print_merge_loc_conf,                      /* merge location configuration */
};


/* ngx_http_print_module */
ngx_module_t  ngx_http_print_module = {
    NGX_MODULE_V1,
    &ngx_http_print_module_ctx,            /* module context */
    ngx_http_print_commands,               /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};



static void
ngx_http_print_process_duplicate_cleanup(void *data)
{
    ngx_http_request_t *r = data;
    ngx_connection_t *c;
    ngx_event_t *wev;

    c = r->connection;
    wev = c->write;

    if (wev->timer_set) {
        ngx_del_timer(wev);
        wev->timer_set = 0;
    }
}


static void
ngx_http_print_process_duplicate_event_handler(ngx_event_t *wev)
{
    ngx_connection_t   *c;
    ngx_http_request_t *r;

    c = wev->data;
    r = c->data;

    if (!wev->timedout) {
        ngx_log_error(NGX_LOG_ERR, c->log, 0, "timer expired prematurely");
        ngx_http_finalize_request(r, NGX_ERROR);
    }

    if (c->destroyed) {
        return;
    }

    if (c->error) {
        ngx_http_finalize_request(r, NGX_ERROR);
    }

    wev->timedout = 0;

    (void) ngx_http_print_process_duplicate(r);
}


static ngx_int_t
ngx_http_print_process_duplicate(ngx_http_request_t *r)
{
    ngx_int_t                   rc;
    ngx_int_t                   last;
    ngx_int_t                   first;
    ngx_chain_t                 out;
    ngx_connection_t           *c;
    ngx_buf_t                  *buf;
    ngx_http_print_loc_conf_t  *plcf;
    ngx_http_print_ctx_t       *pctx;
    ngx_http_print_duplicate_t *pd;
    ngx_http_print_duplicate_t *next_pd;
    ngx_event_t                *wev;

    first = 0;
    last = 0;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_print_module);

    pctx = ngx_http_get_module_ctx(r, ngx_http_print_module);

    pd = (ngx_http_print_duplicate_t *) plcf->dup_objects->elts + pctx->index;

    c = r->connection;

    ngx_log_error(NGX_LOG_DEBUG_HTTP, c->log, 0, "index = %d, rest = %d"
                  "total = %d", pctx->index, pctx->rest, pd->count);
    if (pctx->index == 0 && pctx->rest == pd->count) {
        first = 1;
    }

    pctx->rest--;
    /* next object */
    if (pctx->rest == 0) {
        pctx->index++;

        next_pd = pd;
        next_pd++;
        pctx->rest = next_pd->count;
    }

    if (pctx->index == plcf->dup_objects->nelts) {
        last = 1;
    }

    /* TODO support the complex value, so we recalculate the content */
    rc = ngx_http_print_gen_print_buf(r, pd->objects, &buf);
    if (rc == NGX_ERROR) {
        return rc;
    }

    if (last) {
        /* last one */
        ngx_log_error(NGX_LOG_DEBUG_HTTP, c->log, 0, "last one");
        if (r == r->main) {
            buf->last_buf = 1;

        } else {
            buf->last_in_chain = 1;
        }
    }

    out.buf = buf;
    out.next = NULL;

    rc = ngx_http_output_filter(r, &out);

    if (rc == NGX_ERROR || rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    /* add to timer */
    if (pctx->index != plcf->dup_objects->nelts) {
        wev = r->connection->write;
        wev->handler = ngx_http_print_process_duplicate_event_handler;
        ngx_add_timer(wev, pd->interval);

    } else {
        /* done */
        ngx_http_finalize_request(r, NGX_HTTP_OK);
        return rc;
    }

    if (first) {
        /* keep the count intact */
        r->main->count++;
        return NGX_DONE;
    }

    return rc;
}


static ngx_int_t
ngx_http_print_handler(ngx_http_request_t *r)
{
    ngx_int_t                   rc;
    ngx_chain_t                 out;
    ngx_buf_t                  *normal_buf;
    ngx_http_print_ctx_t       *pctx;
    ngx_http_print_loc_conf_t  *plcf;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_print_module);

    pctx = ngx_http_get_module_ctx(r, ngx_http_print_module);
    if (pctx == NULL) {
        pctx = ngx_pcalloc(r->pool, sizeof(ngx_http_print_ctx_t));
        if (pctx == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_http_set_ctx(r, pctx, ngx_http_print_module)
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_print_gen_print_buf(r, plcf->objects, &normal_buf);
    if (rc == NGX_ERROR) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* We need to calculate the proper Content-Length header */
    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = rc;
    r->allow_ranges = 1;

    if (ngx_http_set_content_type(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    if (r == r->main)  {
        normal_buf->last_buf = 1; /* last buf */

    } else {
        normal_buf->last_in_chain = 1;
    }

    out.buf = normal_buf;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}


static ngx_int_t
ngx_http_print_duplicate_handler(ngx_http_request_t *r)
{
    ngx_int_t                  rc;
    ngx_array_t               *dup_objects;
    ngx_http_print_loc_conf_t *plcf;
    ngx_http_print_ctx_t      *pctx;
    ngx_http_cleanup_t        *cln;

    plcf = ngx_http_get_module_loc_conf(r, ngx_http_print_module);
    pctx = ngx_http_get_module_ctx(r, ngx_http_print_module);
    if (pctx == NULL) {
        pctx = ngx_pcalloc(r->pool, sizeof(ngx_http_print_ctx_t));
        if (pctx == NULL) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_http_set_ctx(r, pctx, ngx_http_print_module)
    }

    dup_objects = plcf->dup_objects;
    pctx->index = 0;

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    if (ngx_http_set_content_type(r) == NGX_ERROR) {
        return NGX_ERROR;
    }

    /* We use the chunked data */
    ngx_http_clear_content_length(r);

    r->headers_out.status = NGX_HTTP_OK;
    r->allow_ranges = 1;

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    pctx->index = 0;
    pctx->rest = ((ngx_http_print_duplicate_t *) dup_objects->elts)->count;

    cln = ngx_http_cleanup_add(r, 0);
    if (cln == NULL) {
        return NGX_ERROR;
    }

    cln->handler = ngx_http_print_process_duplicate_cleanup;
    cln->data = r;

    return ngx_http_print_process_duplicate(r);
}


static ngx_int_t
ngx_http_print_gen_print_buf(ngx_http_request_t *r, ngx_array_t *objects,
    ngx_buf_t **out)
{
    ngx_uint_t                 i, size, nelts;
    ngx_buf_t                 *b;
    ngx_str_t                 *item, *sep, *ends;
    ngx_http_print_loc_conf_t *plcf;

    if (objects == NULL) {
        if (out) {
            *out = NULL;
        }

        return 0;
    }

    size  = 0;
    plcf  = ngx_http_get_module_loc_conf(r, ngx_http_print_module);
    nelts = objects->nelts;
    sep   = &plcf->sep;
    ends  = &plcf->ends;

    /* calculate the total lengths */
    for (i = 0; i < nelts; i++) {
        item = (ngx_str_t *) objects->elts + i;
        size += item->len;
    }

    if (nelts > 0) {
        size += (nelts - 1) * sep->len;
    }

    size += ends->len;

    /* only compute the size */
    if (out == NULL) {
        return size;
    }

    if (size == 0) {
        /* empty normal buf */
        *out = NULL;

        return size;
    }

    b = ngx_create_temp_buf(r->pool, size);
    if (b == NULL) {
        return NGX_ERROR;
    }

    for (i = 0; i < nelts; i++) {
        item = (ngx_str_t *) objects->elts + i;
        b->last = ngx_cpymem(b->last, item->data, item->len);

        if (i == nelts - 1) {
            continue;
        }

        b->last = ngx_cpymem(b->last, sep->data, sep->len);
    }

    b->last = ngx_cpymem(b->last, ends->data, ends->len);
    *out = b;

    if (plcf->flush) {
        b->flush = 1;
    }

    return size;
}


static void *
ngx_http_print_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_print_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_print_loc_conf_t));
    if (conf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->sep         = { 0, NULL };
     *     conf->ends        = { 0, NULL };
     *     conf->objects     = NULL;
     *     conf->dup_objects = NULL;
     */

    conf->flush = NGX_CONF_UNSET;

    return conf;
}


static char *
ngx_http_print_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_print_loc_conf_t *prev = parent;
    ngx_http_print_loc_conf_t *conf = child;

    ngx_conf_merge_str_value(conf->sep, prev->sep, "");
    ngx_conf_merge_str_value(conf->ends, prev->ends, "\n");

    return NGX_CONF_OK;
}


static char *
ngx_http_print(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;
    ngx_http_print_loc_conf_t *plcf;
    ngx_str_t                 *object;
    ngx_str_t                 *value;
    ngx_uint_t                 i;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_print_handler;

    plcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_print_module);
    if (plcf->objects == NULL) {
        /* create the objects */
        plcf->objects = ngx_array_create(cf->pool, 1, sizeof(ngx_str_t));
        if (plcf->objects == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    value = cf->args->elts;

    for (i = 1; i < cf->args->nelts; i++) {
        object = ngx_array_push(plcf->objects);
        if (object == NULL) {
            return NGX_CONF_ERROR;
        }

        object->len = value[i].len;
        object->data = ngx_palloc(cf->pool, object->len);
        ngx_memcpy(object->data, value[i].data, value[i].len); 
    }
        
    return NGX_CONF_OK;
}


/* print_duplicate count interval *objects */
static char *
ngx_http_print_duplicate(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_uint_t                  i;
    ngx_int_t                   count;
    ngx_int_t                   interval;
    ngx_http_core_loc_conf_t   *clcf;
    ngx_http_print_duplicate_t *pd;
    ngx_str_t                  *value;
    ngx_str_t                  *object;
    ngx_http_print_loc_conf_t  *plcf = conf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_print_duplicate_handler;

    value = cf->args->elts;

    count = ngx_atoi(value[1].data, value[1].len);
    if (count == NGX_ERROR || count <= 0) {
        ngx_log_error(NGX_LOG_EMERG, cf, 0,
                      "invalid count value \"%V\"", value[1]);

        return NGX_CONF_ERROR;
    }

    interval = ngx_atoi(value[2].data, value[2].len);
    if (interval == NGX_ERROR || interval <= 0) {
        ngx_log_error(NGX_LOG_EMERG, cf, 0,
                      "invalid intervalvalue \"%V\"", value[2]);

        return NGX_CONF_ERROR;
    }

    if (plcf->dup_objects == NULL) {
        /* create the array */
        plcf->dup_objects = ngx_array_create(cf->pool, 1, sizeof(ngx_http_print_duplicate_t));
        if (plcf->dup_objects == NULL) {
            return NGX_CONF_ERROR;
        }
    }

    pd = ngx_array_push(plcf->dup_objects);
    if (pd == NULL) {
        return NGX_CONF_ERROR;
    }

    pd->count    = count;
    pd->interval = interval;
    pd->objects  = ngx_array_create(cf->pool, cf->args->nelts - 2, sizeof(ngx_str_t));

    for (i = 3; i < cf->args->nelts; i++) {
        object = ngx_array_push(pd->objects);
        if (object == NULL) {
            return NGX_CONF_ERROR;
        }

        object->len = value[i].len;
        object->data = ngx_palloc(cf->pool, object->len);
        ngx_memcpy(object->data, value[i].data, value[i].len);
    }

    return NGX_CONF_OK;
}
