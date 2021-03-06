/**
 * @file operations.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Basic NETCONF operations
 *
 * Copyright (c) 2016 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include <sysrepo.h>

#include "common.h"
#include "operations.h"

char *
op_get_srval(struct ly_ctx *ctx, sr_val_t *value, char *buf)
{
    const struct lys_node *snode;

    if (!value) {
        return NULL;
    }

    switch (value->type) {
    case SR_STRING_T:
    case SR_BINARY_T:
    case SR_BITS_T:
    case SR_ENUM_T:
    case SR_IDENTITYREF_T:
    case SR_INSTANCEID_T:
        return (value->data.string_val);
    case SR_LEAF_EMPTY_T:
        return NULL;
    case SR_BOOL_T:
        return value->data.bool_val ? "true" : "false";
    case SR_DECIMAL64_T:
        /* get fraction-digits */
        snode = ly_ctx_get_node(ctx, NULL, value->xpath);
        if (!snode) {
            return NULL;
        }
        sprintf(buf, "%.*f", ((struct lys_node_leaf *)snode)->type.info.dec64.dig, value->data.decimal64_val);
        return buf;
    case SR_UINT8_T:
        sprintf(buf, "%u", value->data.uint8_val);
        return buf;
    case SR_UINT16_T:
        sprintf(buf, "%u", value->data.uint16_val);
        return buf;
    case SR_UINT32_T:
        sprintf(buf, "%u", value->data.uint32_val);
        return buf;
    case SR_UINT64_T:
        sprintf(buf, "%"PRIu64, value->data.uint64_val);
        return buf;
    case SR_INT8_T:
        sprintf(buf, "%d", value->data.int8_val);
        return buf;
    case SR_INT16_T:
        sprintf(buf, "%d", value->data.int16_val);
        return buf;
    case SR_INT32_T:
        sprintf(buf, "%d", value->data.int32_val);
        return buf;
    case SR_INT64_T:
        sprintf(buf, "%"PRId64, value->data.int64_val);
        return buf;
    default:
        return NULL;
    }

}

static int
copy_bits(const struct lyd_node_leaf_list *leaf, char **dest)
{
    int i;
    struct lys_node_leaf *sch = (struct lys_node_leaf *) leaf->schema;
    char *bits_str = NULL;
    int bits_count = sch->type.info.bits.count;
    struct lys_type_bit **bits = leaf->value.bit;

    size_t length = 1; /* terminating NULL byte*/
    for (i = 0; i < bits_count; i++) {
        if (NULL != bits[i] && NULL != bits[i]->name) {
            length += strlen(bits[i]->name);
            length++; /*space after bit*/
        }
    }
    bits_str = calloc(length, sizeof(*bits_str));
    if (NULL == bits_str) {
        EMEM;
        return -1;
    }
    size_t offset = 0;
    for (i = 0; i < bits_count; i++) {
        if (NULL != bits[i] && NULL != bits[i]->name) {
            strcpy(bits_str + offset, bits[i]->name);
            offset += strlen(bits[i]->name);
            bits_str[offset] = ' ';
            offset++;
        }
    }
    if (0 != offset) {
        bits_str[offset - 1] = '\0';
    }

    *dest = bits_str;
    return 0;
}

int
op_set_srval(struct lyd_node *node, char *path, int dup, sr_val_t *val, char **val_buf)
{
    uint32_t i;
    struct lyd_node_leaf_list *leaf;
    const char *str;

    if (!dup) {
        assert(val_buf);
        (*val_buf) = NULL;
    }

    val->xpath = (dup && path) ? strdup(path) : path;
    val->dflt = 0;
    val->data.int64_val = 0;

    switch (node->schema->nodetype) {
    case LYS_CONTAINER:
        val->type = ((struct lys_node_container *)node->schema)->presence ? SR_CONTAINER_PRESENCE_T : SR_CONTAINER_T;
        break;
    case LYS_LIST:
        val->type = SR_LIST_T;
        break;
    case LYS_LEAF:
    case LYS_LEAFLIST:
        leaf = (struct lyd_node_leaf_list *)node;

        switch (((struct lys_node_leaf *)node->schema)->type.base) {
        case LY_TYPE_BINARY:
            val->type = SR_BINARY_T;
            str = leaf->value.binary;
            val->data.binary_val = (dup && str) ? strdup(str) : (char*)str;
            if (NULL == val->data.binary_val) {
                EMEM;
                return -1;
            }
            break;
        case LY_TYPE_BITS:
            val->type = SR_BITS_T;
            if (copy_bits(leaf, &(val->data.bits_val))) {
                ERR("Copy value failed for leaf '%s' of type 'bits'", leaf->schema->name);
                return -1;
            }
            break;
        case LY_TYPE_BOOL:
            val->type = SR_BOOL_T;
            val->data.bool_val = leaf->value.bln;
            break;
        case LY_TYPE_DEC64:
            val->type = SR_DECIMAL64_T;
            val->data.decimal64_val = (double)leaf->value.dec64;
            for (i = 0; i < ((struct lys_node_leaf *)node->schema)->type.info.dec64.dig; i++) {
                /* shift decimal point */
                val->data.decimal64_val *= 0.1;
            }
            break;
        case LY_TYPE_EMPTY:
            val->type = SR_LEAF_EMPTY_T;
            break;
        case LY_TYPE_ENUM:
            val->type = SR_ENUM_T;
            str = leaf->value.enm->name;
            val->data.enum_val = (dup && str) ? strdup(str) : (char*)str;
            if (NULL == val->data.enum_val) {
                EMEM;
                return -1;
            }
            break;
        case LY_TYPE_IDENT:
            val->type = SR_IDENTITYREF_T;
            if (leaf->value.ident->module == leaf->schema->module) {
                str = leaf->value.ident->name;
                val->data.identityref_val = (dup && str) ? strdup(str) : (char*)str;
                if (NULL == val->data.identityref_val) {
                    EMEM;
                    return -1;
                }
            } else {
                str = malloc(strlen(lys_main_module(leaf->value.ident->module)->name) + 1 + strlen(leaf->value.ident->name) + 1);
                if (NULL == str) {
                    EMEM;
                    return -1;
                }
                sprintf((char *)str, "%s:%s", lys_main_module(leaf->value.ident->module)->name, leaf->value.ident->name);
                val->data.identityref_val = (char *)str;
                if (!dup) {
                    (*val_buf) = (char *)str;
                }
            }
            break;
        case LY_TYPE_INST:
            val->type = SR_INSTANCEID_T;
            break;
        case LY_TYPE_STRING:
            val->type = SR_STRING_T;
            str = leaf->value.string;
            val->data.string_val = (dup && str) ? strdup(str) : (char*)str;
            if (NULL == val->data.string_val) {
                EMEM;
                return -1;
            }
            break;
        case LY_TYPE_INT8:
            val->type = SR_INT8_T;
            val->data.int8_val = leaf->value.int8;
            break;
        case LY_TYPE_UINT8:
            val->type = SR_UINT8_T;
            val->data.uint8_val = leaf->value.uint8;
            break;
        case LY_TYPE_INT16:
            val->type = SR_INT16_T;
            val->data.int16_val = leaf->value.int16;
            break;
        case LY_TYPE_UINT16:
            val->type = SR_UINT16_T;
            val->data.uint16_val = leaf->value.uint16;
            break;
        case LY_TYPE_INT32:
            val->type = SR_INT32_T;
            val->data.int32_val = leaf->value.int32;
            break;
        case LY_TYPE_UINT32:
            val->type = SR_UINT32_T;
            val->data.uint32_val = leaf->value.uint32;
            break;
        case LY_TYPE_INT64:
            val->type = SR_INT64_T;
            val->data.int64_val = leaf->value.int64;
            break;
        case LY_TYPE_UINT64:
            val->type = SR_UINT64_T;
            val->data.uint64_val = leaf->value.uint64;
            break;
        default:
            //LY_LEAFREF, LY_DERIVED, LY_UNION
            val->type = SR_UNKNOWN_T;
            break;
        }
        break;
    default:
        val->type = SR_UNKNOWN_T;
        break;
    }

    return 0;
}

/* return: -1 = discard, 0 = keep, 1 = keep and add the attribute */
int
op_dflt_data_inspect(struct ly_ctx *ctx, sr_val_t *value, NC_WD_MODE wd, int rpc_output)
{
    const struct lys_node_leaf *sleaf;
    struct lys_tpdf *tpdf;
    const char *dflt_val = NULL;
    char buf[256], *val;

    /* NC_WD_ALL HANDLED */
    if (wd == NC_WD_ALL) {
        /* we keep it all */
        return 0;
    }

    if ((wd == NC_WD_EXPLICIT) && !value->dflt) {
        return 0;
    }

    /*
     * we need the schema node now
     */

    sleaf = (const struct lys_node_leaf *)ly_ctx_get_node2(ctx, NULL, value->xpath, rpc_output);
    if (!sleaf) {
        EINT;
        return -1;
    }

    if (sleaf->nodetype != LYS_LEAF) {
        return 0;
    }

    /* NC_WD_EXPLICIT HANDLED */
    if (wd == NC_WD_EXPLICIT) {
        if ((sleaf->flags & LYS_CONFIG_W) && !rpc_output) {
            return -1;
        }
        return 0;
    }

    if (value->dflt) {
        switch (wd) {
        case NC_WD_TRIM:
            return -1;
        case NC_WD_ALL_TAG:
            return 1;
        default:
            EINT;
            return -1;
        }
    }

    /*
     * we need to actually examine the value now
     */

    /* leaf's default value */
    dflt_val = sleaf->dflt;

    /* typedef's default value */
    if (!dflt_val) {
        tpdf = sleaf->type.der;
        while (tpdf && !tpdf->dflt) {
            tpdf = tpdf->type.der;
        }
        if (tpdf) {
            dflt_val = tpdf->dflt;
        }
    }

    /* value itself */
    val = op_get_srval(ctx, value, buf);

    switch (wd) {
    case NC_WD_TRIM:
        if (dflt_val && !strcmp(dflt_val, val)) {
            return -1;
        }
        break;
    case NC_WD_ALL_TAG:
        if (dflt_val && !strcmp(dflt_val, val)) {
            return 1;
        }
        break;
    default:
        EINT;
        return -1;
    }

    return 0;
}
