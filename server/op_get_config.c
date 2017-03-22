/**
 * @file op_get_config.c
 * @author Radek Krejci <rkrejci@cesnet.cz>
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief NETCONF <get> and <get-config> operations implementation
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <libyang/libyang.h>
#include <nc_server.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"
#include "netconf_monitoring.h"

/* add whole subtree */
static int
opget_build_subtree_from_sysrepo(sr_session_ctx_t *ds, struct lyd_node **root, const char *subtree_xpath)
{
    sr_val_t *value;
    sr_val_iter_t *sriter;
    struct lyd_node *node, *iter;
    char *full_subtree_xpath = NULL, buf[128];
    int rc;

    if (asprintf(&full_subtree_xpath, "%s//.", subtree_xpath) == -1) {
        EMEM;
        return -1;
    }

    rc = sr_get_items_iter(ds, full_subtree_xpath, &sriter);
    if ((rc == SR_ERR_UNKNOWN_MODEL) || (rc == SR_ERR_NOT_FOUND)) {
        /* it's ok, model without data */
        free(full_subtree_xpath);
        return 0;
    } else if (rc != SR_ERR_OK) {
        ERR("Getting items (%s) from sysrepo failed (%s).", full_subtree_xpath, sr_strerror(rc));
        free(full_subtree_xpath);
        return -1;
    }
    free(full_subtree_xpath);

    ly_errno = LY_SUCCESS;
    while (sr_get_item_next(ds, sriter, &value) == SR_ERR_OK) {
        ly_errno = LY_SUCCESS;
        node = lyd_new_path(*root, np2srv.ly_ctx, value->xpath,
                            op_get_srval(np2srv.ly_ctx, value, buf), 0, LYD_PATH_OPT_UPDATE);
        if (ly_errno) {
            sr_free_val(value);
            sr_free_val_iter(sriter);
            return -1;
        }

        if (!(*root)) {
            *root = node;
        }

        if (node) {
            /* propagate default flag */
            if (value->dflt) {
                /* go down */
                for (iter = node;
                     !(iter->schema->nodetype & (LYS_LEAF | LYS_LEAFLIST | LYS_ANYXML)) && iter->child;
                     iter = iter->child);
                /* go up, back to the node */
                for (; ; iter = iter->parent) {
                    if (iter->schema->nodetype == LYS_CONTAINER && ((struct lys_node_container *)iter->schema)->presence) {
                        /* presence container */
                        break;
                    } else if (iter->schema->nodetype == LYS_LIST && ((struct lys_node_list *)iter->schema)->keys_size) {
                        /* list with keys */
                        break;
                    }
                    iter->dflt = 1;
                    if (iter == node) {
                        /* done */
                        break;
                    }
                }
            } else { /* non default node, propagate it to the parents */
                for (iter = node->parent; iter && iter->dflt; iter = iter->parent) {
                    iter->dflt = 0;
                }
            }
        }
        sr_free_val(value);
    }
    sr_free_val_iter(sriter);

    return 0;
}

static int
opget_build_tree_from_data(struct lyd_node **root, struct lyd_node *data, const char *subtree_path)
{
    struct ly_set *nodeset;
    struct lyd_node *node, *node2, *key, *key2, *child, *tmp_root;
    struct lys_node_list *slist;
    uint16_t i, j;

    nodeset = lyd_find_xpath(data, subtree_path);
    for (i = 0; i < nodeset->number; ++i) {
        node = nodeset->set.d[i];
        tmp_root = lyd_dup(node, 1);
        if (!tmp_root) {
            EMEM;
            return -1;
        }
        for (node = node->parent; node; node = node->parent) {
            node2 = lyd_dup(node, 0);
            if (!node2) {
                EMEM;
                return -1;
            }
            if (lyd_insert(node2, tmp_root)) {
                EINT;
                lyd_free(node2);
                return -1;
            }
            tmp_root = node2;

            /* we want to include all list keys in the result */
            if (node2->schema->nodetype == LYS_LIST) {
                slist = (struct lys_node_list *)node2->schema;
                for (j = 0, key = node->child; j < slist->keys_size; ++j, key = key->next) {
                    assert((struct lys_node *)slist->keys[j] == key->schema);

                    /* was the key already duplicated? */
                    LY_TREE_FOR(node2->child, child) {
                        if (child->schema == (struct lys_node *)slist->keys[j]) {
                            break;
                        }
                    }

                    /* it wasn't */
                    if (!child) {
                        key2 = lyd_dup(key, 0);
                        if (!key2) {
                            EMEM;
                            return -1;
                        }
                        if (lyd_insert(node2, key2)) {
                            EINT;
                            lyd_free(key2);
                            return -1;
                        }
                    }
                }

                /* we added those keys at the end, if some existed before the order is wrong */
                if (lyd_schema_sort(node2->child, 0)) {
                    EINT;
                    return -1;
                }
            }
        }

        if (*root) {
            if (lyd_merge(*root, tmp_root, LYD_OPT_DESTRUCT)) {
                EINT;
                return -1;
            }
        } else {
            *root = tmp_root;
        }
    }
    ly_set_free(nodeset);

    return 0;
}

static int
strws(const char *str)
{
    while (*str) {
        if (!isspace(*str)) {
            return 0;
        }
        ++str;
    }

    return 1;
}

static int
opget_xpath_add_filter(char *new_filter, char ***filters, int *filter_count)
{
    char **filters_new;

    filters_new = realloc(*filters, (*filter_count + 1) * sizeof **filters);
    if (!filters_new) {
        EMEM;
        return -1;
    }
    ++(*filter_count);
    *filters = filters_new;
    (*filters)[*filter_count - 1] = new_filter;

    return 0;
}

static int
opget_xpath_buf_add_attrs(struct ly_ctx *ctx, struct lyxml_attr *attr, char **buf, int size)
{
    const struct lys_module *module;
    struct lyxml_attr *next;
    int new_size;
    char *buf_new;

    LY_TREE_FOR(attr, next) {
        if (next->type == LYXML_ATTR_STD) {
            module = NULL;
            if (next->ns) {
                module = ly_ctx_get_module_by_ns(ctx, next->ns->value, NULL);
            }
            if (!module) {
                /* attribute without namespace or with unknown one will not match anything anyway */
                continue;
            }

            new_size = size + 2 + strlen(module->name) + 1 + strlen(next->name) + 2 + strlen(next->value) + 2;
            buf_new = realloc(*buf, new_size * sizeof(char));
            if (!buf_new) {
                EMEM;
                return -1;
            }
            *buf = buf_new;
            sprintf((*buf) + (size - 1), "[@%s:%s='%s']", module->name, next->name, next->value);
            size = new_size;
        }
    }

    return size;
}

/* top-level content node with optional namespace and attributes */
static int
opget_xpath_buf_add_top_content(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name,
                                char ***filters, int *filter_count)
{
    int size, len;
    const char *start;
    char *buf;

    /* skip leading and trailing whitespaces */
    for (start = elem->content; isspace(*start); ++start);
    for (len = strlen(start); isspace(start[len - 1]); --len);

    size = 1 + strlen(elem_module_name) + 1 + strlen(elem->name) + 9 + len + 3;
    buf = malloc(size * sizeof(char));
    if (!buf) {
        EMEM;
        return -1;
    }
    sprintf(buf, "/%s:%s[text()='%.*s']", elem_module_name, elem->name, len, start);

    size = opget_xpath_buf_add_attrs(ctx, elem->attr, &buf, size);
    if (!size) {
        free(buf);
        return 0;
    } else if (size < 1) {
        free(buf);
        return -1;
    }

    if (opget_xpath_add_filter(buf, filters, filter_count)) {
        free(buf);
        return -1;
    }

    return 0;
}

/* content node with optional namespace and attributes */
static int
opget_xpath_buf_add_content(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name,
                            const char **last_ns, char **buf, int size)
{
    const struct lys_module *module;
    int new_size, len;
    const char *start;
    char *buf_new;

    if (!elem_module_name && elem->ns && (elem->ns->value != *last_ns)
            && strcmp(elem->ns->value, "urn:ietf:params:xml:ns:netconf:base:1.0")) {
        module = ly_ctx_get_module_by_ns(ctx, elem->ns->value, NULL);
        if (!module) {
            /* not really an error */
            return 0;
        }

        *last_ns = elem->ns->value;
        elem_module_name = module->name;
    }

    new_size = size + 1 + (elem_module_name ? strlen(elem_module_name) + 1 : 0) + strlen(elem->name);
    buf_new = realloc(*buf, new_size * sizeof(char));
    if (!buf_new) {
        EMEM;
        return -1;
    }
    *buf = buf_new;
    sprintf((*buf) + (size - 1), "[%s%s%s", (elem_module_name ? elem_module_name : ""), (elem_module_name ? ":" : ""),
            elem->name);
    size = new_size;

    size = opget_xpath_buf_add_attrs(ctx, elem->attr, buf, size);
    if (!size) {
        return 0;
    } else if (size < 1) {
        return -1;
    }

    /* skip leading and trailing whitespaces */
    for (start = elem->content; isspace(*start); ++start);
    for (len = strlen(start); isspace(start[len - 1]); --len);

    new_size = size + 2 + len + 2;
    buf_new = realloc(*buf, new_size * sizeof(char));
    if (!buf_new) {
        EMEM;
        return -1;
    }
    *buf = buf_new;
    sprintf((*buf) + (size - 1), "='%.*s']", len, start);

    return new_size;
}

/* containment/selection node with optional namespace and attributes */
static int
opget_xpath_buf_add_node(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name,
                         const char **last_ns, char **buf, int size)
{
    const struct lys_module *module;
    int new_size;
    char *buf_new;

    if (!elem_module_name && elem->ns && (elem->ns->value != *last_ns)
            && strcmp(elem->ns->value, "urn:ietf:params:xml:ns:netconf:base:1.0")) {
        module = ly_ctx_get_module_by_ns(ctx, elem->ns->value, NULL);
        if (!module) {
            /* not really an error */
            return 0;
        }

        *last_ns = elem->ns->value;
        elem_module_name = module->name;
    }

    new_size = size + 1 + (elem_module_name ? strlen(elem_module_name) + 1 : 0) + strlen(elem->name);
    buf_new = realloc(*buf, new_size * sizeof(char));
    if (!buf_new) {
        EMEM;
        return -1;
    }
    *buf = buf_new;
    sprintf((*buf) + (size - 1), "/%s%s%s", (elem_module_name ? elem_module_name : ""), (elem_module_name ? ":" : ""),
            elem->name);
    size = new_size;

    size = opget_xpath_buf_add_attrs(ctx, elem->attr, buf, size);

    return size;
}

/* buf is spent in the function, removes content match nodes from elem->child list! */
static int
opget_xpath_buf_add(struct ly_ctx *ctx, struct lyxml_elem *elem, const char *elem_module_name, const char *last_ns,
                    char **buf, int size, char ***filters, int *filter_count)
{
    struct lyxml_elem *temp, *child;
    int new_size;
    char *buf_new;

    /* containment node, selection node */
    size = opget_xpath_buf_add_node(ctx, elem, elem_module_name, &last_ns, buf, size);
    if (!size) {
        free(*buf);
        *buf = NULL;
        return 0;
    } else if (size < 1) {
        goto error;
    }

    /* content match node */
    LY_TREE_FOR_SAFE(elem->child, temp, child) {
        if (!child->child && child->content && !strws(child->content)) {
            size = opget_xpath_buf_add_content(ctx, child, elem_module_name, &last_ns, buf, size);
            if (!size) {
                free(*buf);
                *buf = NULL;
                return 0;
            } else if (size < 1) {
                goto error;
            }

            /* this content match node must be present in the final output, so add it as a selection node as well */
            /* TODO optimization: needed only if child is not key and we have a sibling containment/selection node */
            buf_new = malloc(size * sizeof(char));
            if (!buf_new) {
                EMEM;
                goto error;
            }
            memcpy(buf_new, *buf, size * sizeof(char));
            new_size = size;

            new_size = opget_xpath_buf_add_node(ctx, child, elem_module_name, &last_ns, &buf_new, new_size);
            if (!new_size) {
                free(*buf);
                *buf = NULL;
                free(buf_new);
                return 0;
            } else if (new_size < 1) {
                goto error;
            }
            if (opget_xpath_add_filter(buf_new, filters, filter_count)) {
                goto error;
            }

            lyxml_free(ctx, child);
        }
    }

    /* that is it, it seems */
    if (!elem->child) {
        if (opget_xpath_add_filter(*buf, filters, filter_count)) {
            goto error;
        }
        *buf = NULL;
        return 0;
    }

    /* that is it for this filter depth, now we branch with every new node except last */
    LY_TREE_FOR(elem->child, child) {
        if (!child->next) {
            buf_new = *buf;
            *buf = NULL;
        } else {
            buf_new = malloc(size * sizeof(char));
            if (!buf_new) {
                EMEM;
                goto error;
            }
            memcpy(buf_new, *buf, size * sizeof(char));
        }
        new_size = size;

        /* child containment node */
        if (child->child) {
            opget_xpath_buf_add(ctx, child, NULL, last_ns, &buf_new, new_size, filters, filter_count);

        /* child selection node */
        } else {
            new_size = opget_xpath_buf_add_node(ctx, child, NULL, &last_ns, &buf_new, new_size);
            if (!new_size) {
                free(buf_new);
                continue;
            } else if (new_size < 1) {
                free(buf_new);
                goto error;
            }

            if (opget_xpath_add_filter(buf_new, filters, filter_count)) {
                goto error;
            }
        }
    }

    return 0;

error:
    free(*buf);
    return -1;
}

/* modifies elem XML tree! */
static int
opget_build_xpath_from_subtree_filter(struct ly_ctx *ctx, struct lyxml_elem *elem, char ***filters, int *filter_count)
{
    const struct lys_module *module, **modules, **modules_new;
    const struct lys_node *node;
    struct lyxml_elem *next;
    char *buf;
    uint32_t i, module_count;

    LY_TREE_FOR(elem, next) {
        /* first filter node, it must always have a namespace */
        modules = NULL;
        module_count = 0;
        if (next->ns && strcmp(next->ns->value, "urn:ietf:params:xml:ns:netconf:base:1.0")) {
            modules = malloc(sizeof *modules);
            if (!modules) {
                EMEM;
                goto error;
            }
            module_count = 1;
            modules[0] = ly_ctx_get_module_by_ns(ctx, next->ns->value, NULL);
            if (!modules[0]) {
                /* not really an error */
                free(modules);
                continue;
            }
        } else {
            i = 0;
            while ((module = ly_ctx_get_module_iter(ctx, &i))) {
                node = NULL;
                while ((node = lys_getnext(node, NULL, module, 0))) {
                    if (!strcmp(node->name, next->name)) {
                        modules_new = realloc(modules, (module_count + 1) * sizeof *modules);
                        if (!modules_new) {
                            EMEM;
                            goto error;
                        }
                        ++module_count;
                        modules = modules_new;
                        modules[module_count - 1] = module;
                        break;
                    }
                }
            }
        }

        buf = NULL;
        for (i = 0; i < module_count; ++i) {
            if (!next->child && next->content && !strws(next->content)) {
                /* special case of top-level content match node */
                if (opget_xpath_buf_add_top_content(ctx, next, modules[i]->name, filters, filter_count)) {
                    goto error;
                }
            } else {
                /* containment or selection node */
                if (opget_xpath_buf_add(ctx, next, modules[i]->name, modules[i]->ns, &buf, 1, filters, filter_count)) {
                    goto error;
                }
            }
        }
        free(modules);
    }

    return 0;

error:
    free(modules);
    for (i = 0; (signed)i < *filter_count; ++i) {
        free((*filters)[i]);
    }
    free(*filters);
    return -1;
}

struct nc_server_reply *
op_get(struct lyd_node *rpc, struct nc_session *ncs)
{
    const struct lys_module *module;
    const struct lys_node *snode;
    struct lyd_node_leaf_list *leaf;
    struct lyd_node *root = NULL, *node, *yang_lib_data = NULL, *ncm_data = NULL, *ntf_data = NULL;
    struct lyd_attr *attr;
    char **filters = NULL, *path;
    int filter_count = 0;
    unsigned int config_only;
    uint32_t i;
    struct lyxml_elem *subtree_filter;
    struct np2_sessions *sessions;
    struct ly_set *nodeset;
    sr_datastore_t ds = 0;
    struct nc_server_error *e;
    struct nc_server_reply *ereply = NULL;
    NC_WD_MODE nc_wd;

    /* get sysrepo connections for this session */
    sessions = (struct np2_sessions *)nc_session_get_data(ncs);

    /* get default value for with-defaults */
    nc_server_get_capab_withdefaults(&nc_wd, NULL);

    /* get know which datastore is being affected */
    if (!strcmp(rpc->schema->name, "get")) {
        config_only = 0;
        ds = SR_DS_RUNNING;
    } else { /* get-config */
        config_only = SR_SESS_CONFIG_ONLY;
        nodeset = lyd_find_xpath(rpc, "/ietf-netconf:get-config/source/*");
        if (!strcmp(nodeset->set.d[0]->schema->name, "running")) {
            ds = SR_DS_RUNNING;
        } else if (!strcmp(nodeset->set.d[0]->schema->name, "startup")) {
            ds = SR_DS_STARTUP;
        } else if (!strcmp(nodeset->set.d[0]->schema->name, "candidate")) {
            ds = SR_DS_CANDIDATE;
        }
        /* TODO URL capability */

        ly_set_free(nodeset);
    }
    if (ds != sessions->ds || (sessions->opts & SR_SESS_CONFIG_ONLY) != config_only) {
        /* update sysrepo session datastore */
        sr_session_switch_ds(sessions->srs, ds);
        sessions->ds = ds;

        /* update sysrepo session config */
        sr_session_set_options(sessions->srs, config_only);
        sessions->opts = config_only;
    }

    /* create filters */
    nodeset = lyd_find_xpath(rpc, "/ietf-netconf:*/filter");
    if (nodeset->number) {
        node = nodeset->set.d[0];
        ly_set_free(nodeset);
        LY_TREE_FOR(node->attr, attr) {
            if (!strcmp(attr->name, "type")) {
                if (!strcmp(attr->value_str, "xpath")) {
                    LY_TREE_FOR(node->attr, attr) {
                        if (!strcmp(attr->name, "select")) {
                            break;
                        }
                    }
                    if (!attr) {
                        ERR("RPC with an XPath filter without the \"select\" attribute.");
                        goto error;
                    }
                    break;
                } else if (!strcmp(attr->value_str, "subtree")) {
                    attr = NULL;
                    break;
                }
            }
        }

        if (!attr) {
            /* subtree */
            if (!((struct lyd_node_anydata *)node)->value.str
                    || (((struct lyd_node_anydata *)node)->value_type <= LYD_ANYDATA_STRING &&
                        !((struct lyd_node_anydata *)node)->value.str[0])) {
                /* empty filter, fair enough */
                goto send_reply;
            }

            switch (((struct lyd_node_anydata *)node)->value_type) {
            case LYD_ANYDATA_CONSTSTRING:
            case LYD_ANYDATA_STRING:
                subtree_filter = lyxml_parse_mem(np2srv.ly_ctx, ((struct lyd_node_anydata *)node)->value.str, LYXML_PARSE_MULTIROOT);
                break;
            case LYD_ANYDATA_XML:
                subtree_filter = ((struct lyd_node_anydata *)node)->value.xml;
                break;
            default:
                /* filter cannot be parsed as lyd_node tree */
                goto error;
            }
            if (!subtree_filter) {
                goto error;
            }

            if (opget_build_xpath_from_subtree_filter(np2srv.ly_ctx, subtree_filter, &filters, &filter_count)) {
                goto error;
            }
        } else {
            /* xpath */
            if (!attr->value_str || !attr->value_str[0]) {
                /* empty select, okay, I guess... */
                goto send_reply;
            }
            path = strdup(attr->value_str);
            if (!path) {
                EMEM;
                goto error;
            }
            if (opget_xpath_add_filter(path, &filters, &filter_count)) {
                free(path);
                goto error;
            }
        }
    } else {
        ly_set_free(nodeset);

        i = 0;
        while ((module = ly_ctx_get_module_iter(np2srv.ly_ctx, &i))) {
            LY_TREE_FOR(module->data, snode) {
                if (!(snode->nodetype & (LYS_GROUPING | LYS_NOTIF | LYS_RPC))) {
                    /* module with some actual data definitions */
                    break;
                }
            }

            if (snode) {
                asprintf(&path, "/%s:*", module->name);
                if (opget_xpath_add_filter(path, &filters, &filter_count)) {
                    free(path);
                    goto error;
                }
            }
        }
    }

    /* get with-defaults mode */
    nodeset = lyd_find_xpath(rpc, "/ietf-netconf:*/ietf-netconf-with-defaults:with-defaults");
    if (nodeset->number) {
        leaf = (struct lyd_node_leaf_list *)nodeset->set.d[0];
        if (!strcmp(leaf->value_str, "report-all")) {
            nc_wd = NC_WD_ALL;
        } else if (!strcmp(leaf->value_str, "report-all-tagged")) {
            nc_wd = NC_WD_ALL_TAG;
        } else if (!strcmp(leaf->value_str, "trim")) {
            nc_wd = NC_WD_TRIM;
        } else if (!strcmp(leaf->value_str, "explicit")) {
            nc_wd = NC_WD_EXPLICIT;
        } else {
            /* we received it, so it was validated, this cannot be */
            EINT;
            goto error;
        }
    }
    ly_set_free(nodeset);


    if (sessions->ds != SR_DS_CANDIDATE) {
        /* refresh sysrepo data */
        if (sr_session_refresh(sessions->srs) != SR_ERR_OK) {
            goto srerror;
        }
    } else if (!(sessions->flags & NP2S_CAND_CHANGED)) {
        /* update candidate to be the same as running */
        if (sr_session_refresh(sessions->srs)) {
            goto srerror;
        }
    }

    /*
     * create the data tree for the data reply
     */
    for (i = 0; (signed)i < filter_count; i++) {
        /* special case, we have these data locally */
        if (!strncmp(filters[i], "/ietf-yang-library:", 19)) {
            if (config_only) {
                /* these are all state data */
                continue;
            }

            if (!yang_lib_data) {
                yang_lib_data = ly_ctx_info(np2srv.ly_ctx);
                if (!yang_lib_data) {
                    goto error;
                }
            }

            if (opget_build_tree_from_data(&root, yang_lib_data, filters[i])) {
                goto error;
            }
            continue;
        } else if (!strncmp(filters[i], "/ietf-netconf-monitoring:", 25)) {
            if (config_only) {
                /* these are all state data */
                continue;
            }

            if (!ncm_data) {
                ncm_data = ncm_get_data();
                if (!ncm_data) {
                    goto error;
                }
            }

            if (opget_build_tree_from_data(&root, ncm_data, filters[i])) {
                goto error;
            }
            continue;
        } else if (!strncmp(filters[i], "/nc-notifications:", 18)) {
            if (config_only) {
                /* these are all state data */
                continue;
            }

            if (!ntf_data) {
                ntf_data = ntf_get_data();
                if (!ntf_data) {
                    goto error;
                }
            }

            if (opget_build_tree_from_data(&root, ntf_data, filters[i])) {
                goto error;
            }
            continue;
        }

        /* create this subtree */
        if (opget_build_subtree_from_sysrepo(sessions->srs, &root, filters[i])) {
            goto error;
        }
    }
    lyd_free_withsiblings(yang_lib_data);
    yang_lib_data = NULL;
    lyd_free_withsiblings(ncm_data);
    ncm_data = NULL;
    lyd_free_withsiblings(ntf_data);
    ntf_data = NULL;

    for (i = 0; (signed)i < filter_count; ++i) {
        free(filters[i]);
    }
    filter_count = 0;
    free(filters);
    filters = NULL;

    /* debug
    lyd_print_file(stdout, root, LYD_XML_FORMAT, LYP_WITHSIBLINGS);
    debug */

send_reply:
    /* build RPC Reply */
    if (lyd_validate(&root, (config_only ? LYD_OPT_GETCONFIG : LYD_OPT_GET), np2srv.ly_ctx)) {
        EINT;
        goto error;
    }
    node = root;
    root = lyd_dup(rpc, 0);

    lyd_new_output_anydata(root, NULL, "data", node, LYD_ANYDATA_DATATREE);
    if (lyd_validate(&root, LYD_OPT_RPCREPLY, NULL)) {
        EINT;
        goto error;
    }

    return nc_server_reply_data(root, nc_wd, NC_PARAMTYPE_FREE);

srerror:
    ereply = op_build_err_sr(ereply, sessions->srs);

error:
    if (!ereply) {
        e = nc_err(NC_ERR_OP_FAILED, NC_ERR_TYPE_APP);
        nc_err_set_msg(e, np2log_lasterr(), "en");
        ereply = nc_server_reply_err(e);
    }

    for (i = 0; (signed)i < filter_count; ++i) {
        free(filters[i]);
    }
    free(filters);

    lyd_free_withsiblings(yang_lib_data);
    lyd_free_withsiblings(ncm_data);
    lyd_free_withsiblings(ntf_data);
    lyd_free_withsiblings(root);

    return ereply;
}
