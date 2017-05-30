
/* 
 * Copyright Alex(zchao1995@gmail.com)
 *
 * ngx_http_print_module
 *
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_config.h>


typedef struct {
    ngx_array_t *objects;
    ngx_str_t   sep;
    ngx_str_t   ends;
} ngx_http_print_loc_conf_t;


static ngx_int_t ngx_http_print_handler(ngx_http_request_t *r);
static void *ngx_http_print_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_print_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child);
static char *ngx_http_print(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);


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
};


/* module ctx */
ngx_http_module_t  ngx_http_print_module_ctx = {
    NULL,                                               /* preconfiguration */
    NULL,                                /* postconfiguration */

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



/* TODO support the variable */
static ngx_int_t
ngx_http_print_handler(ngx_http_request_t *r)
{
    ngx_int_t                  rc;
    ngx_uint_t                 i, size, nelts;
    ngx_buf_t                 *b;
    ngx_chain_t                out;
    ngx_array_t               *objects;
    ngx_str_t                 *item, *sep, *ends;
    ngx_http_print_loc_conf_t *plcf;

    size    = 0;
    plcf    = ngx_http_get_module_loc_conf(r, ngx_http_print_module);
    objects = plcf->objects;
    nelts   = objects->nelts;
    sep     = &plcf->sep;
    ends    = &plcf->ends;

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    /* calculate the total lengths */
    for (i = 0; i < nelts; i++) {
        item = (ngx_str_t *) objects->elts + i;
        size += item->len;
    }

    if (nelts > 0) {
        size += (nelts - 1) * sep->len;
    }

    size += ends->len;

    r->headers_out.content_length_n = size;
    r->headers_out.status = NGX_HTTP_OK;
    r->connection->log->action = "sending response to client";

    if (ngx_http_set_content_type(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b = ngx_create_temp_buf(r->pool, size);

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
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
    b->last_buf = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
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
     *     conf->sep      = { 0, NULL };
     *     conf->ends     = { 0, NULL };
     *     conf->objects  = NULL;
     */

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
        plcf->objects = ngx_array_create(cf->temp_pool, 1, sizeof(ngx_str_t));
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
        object->data = ngx_palloc(cf->temp_pool, object->len);
        ngx_memcpy(object->data, value[i].data, value[i].len); 
    }
        
    return NGX_CONF_OK;
}
