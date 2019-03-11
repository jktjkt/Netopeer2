/**
 * @file test_copy_config.c
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Cmocka np2srv <copy-config> test.
 *
 * Copyright (c) 2016-2017 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */
#define _GNU_SOURCE

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <unistd.h>

#include "config.h"
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
#define ENABLE_TEST_FILE
#endif
#include "tests/config.h"
#undef ENABLE_TEST_FILE

#define main server_main
#undef NP2SRV_PIDFILE
#define NP2SRV_PIDFILE "/tmp/test_np2srv.pid"

#ifdef NP2SRV_ENABLED_URL_CAPABILITY
#define URL_TESTFILE "/tmp/nc2_copy_config.xml"
#endif

#include "../main.c"

#undef main

struct lyd_node *ietf_if_data;
struct lyd_node *simplified_melt_data;
int initialized;
sem_t wait_for_init;
int pipes[2][2], p_in, p_out;

/*
 * SYSREPO WRAPPER FUNCTIONS
 */
int
__wrap_sr_connect(const char *app_name, const sr_conn_options_t opts, sr_conn_ctx_t **conn_ctx)
{
    (void)app_name;
    (void)opts;
    (void)conn_ctx;
    return SR_ERR_OK;
}

int
__wrap_sr_session_start(sr_conn_ctx_t *conn_ctx, const sr_datastore_t datastore,
                        const sr_sess_options_t opts, sr_session_ctx_t **session)
{
    (void)conn_ctx;
    (void)datastore;
    (void)opts;
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_session_start_user(sr_conn_ctx_t *conn_ctx, const char *user_name, const sr_datastore_t datastore,
                             const sr_sess_options_t opts, sr_session_ctx_t **session)
{
    (void)conn_ctx;
    (void)user_name;
    (void)datastore;
    (void)opts;
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_session_stop(sr_session_ctx_t *session)
{
    (void)session;
    return SR_ERR_OK;
}

void
__wrap_sr_disconnect(sr_conn_ctx_t *conn_ctx)
{
    (void)conn_ctx;
}

int
__wrap_sr_session_refresh(sr_session_ctx_t *session)
{
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_module_install_subscribe(sr_session_ctx_t *session, sr_module_install_cb callback, void *private_ctx,
                                   sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    (void)session;
    (void)callback;
    (void)private_ctx;
    (void)opts;
    (void)subscription;
    return SR_ERR_OK;
}

int
__wrap_sr_feature_enable_subscribe(sr_session_ctx_t *session, sr_feature_enable_cb callback, void *private_ctx,
                                   sr_subscr_options_t opts, sr_subscription_ctx_t **subscription)
{
    (void)session;
    (void)callback;
    (void)private_ctx;
    (void)opts;
    (void)subscription;
    return SR_ERR_OK;
}

int
__wrap_sr_module_change_subscribe(sr_session_ctx_t *session, const char *module_name, sr_module_change_cb callback,
                                  void *private_ctx, uint32_t priority, sr_subscr_options_t opts,
                                  sr_subscription_ctx_t **subscription)
{
    (void)session;
    (void)module_name;
    (void)callback;
    (void)private_ctx;
    (void)priority;
    (void)opts;
    (void)subscription;
    return SR_ERR_OK;
}

int
__wrap_sr_session_switch_ds(sr_session_ctx_t *session, sr_datastore_t ds)
{
    (void)session;
    (void)ds;

    return SR_ERR_OK;
}

void
__wrap_sr_free_val_iter(sr_val_iter_t *iter)
{
    if (iter) {
        free(iter);
    }
}

static int set_item_count = 0;

int
__wrap_sr_set_item(sr_session_ctx_t *session, const char *xpath, const sr_val_t *value, const sr_edit_options_t opts)
{
    (void)session;
    (void)value;
    (void)opts;

    switch (set_item_count) {
    case 0:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']");
        break;
    case 1:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/description");
        break;
    case 2:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/type");
        break;
    case 3:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/enabled");
        break;
    case 4:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/link-up-down-trap-enable");
        break;
    case 5:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv4");
        break;
    case 6:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv4/enabled");
        break;
    case 7:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv4/forwarding");
        break;
    case 8:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv4/mtu");
        break;
    case 9:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv4/neighbor[ip='10.0.0.2']");
        break;
    case 10:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv4/neighbor[ip='10.0.0.2']/link-layer-address");
        break;
    case 11:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv6");
        break;
    case 12:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv6/enabled");
        break;
    case 13:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name='iface1/1']/ietf-ip:ipv6/forwarding");
        break;
    case 14:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]");
        break;
    case 15:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/description");
        break;
    case 16:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/type");
        break;
    case 17:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/enabled");
        break;
    case 18:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/link-up-down-trap-enable");
        break;
    case 19:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv4");
        break;
    case 20:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv4/enabled");
        break;
    case 21:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv4/forwarding");
        break;
    case 22:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv4/mtu");
        break;
    case 23:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv4/neighbor[ip='10.0.0.2']");
        break;
    case 24:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv4/neighbor[ip='10.0.0.2']/link-layer-address");
        break;
    case 25:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv6");
        break;
    case 26:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv6/enabled");
        break;
    case 27:
        assert_string_equal(xpath, "/ietf-interfaces:interfaces/interface[name=\"'iface1/2'\"]/ietf-ip:ipv6/forwarding");
        break;
    default:
        assert_string_equal(xpath, "too many nodes");
        break;
    }
    ++set_item_count;

    return SR_ERR_OK;
}

int
__wrap_sr_commit(sr_session_ctx_t *session)
{
    (void)session;
    return SR_ERR_OK;
}

int
__wrap_sr_delete_item(sr_session_ctx_t *session, const char *xpath, const sr_edit_options_t opts)
{
    (void)session;
    (void)xpath;
    (void)opts;

    return SR_ERR_OK;
}

int
__wrap_sr_event_notif_send(sr_session_ctx_t *session, const char *xpath, const sr_val_t *values,
                           const size_t values_cnt, sr_ev_notif_flag_t opts)
{
    (void)session;
    (void)xpath;
    (void)values;
    (void)values_cnt;
    (void)opts;
    return SR_ERR_OK;
}

int
__wrap_sr_check_exec_permission(sr_session_ctx_t *session, const char *xpath, bool *permitted)
{
    (void)session;
    (void)xpath;
    *permitted = true;
    return SR_ERR_OK;
}

int
__wrap_sr_get_items_iter(sr_session_ctx_t *session, const char *xpath, sr_val_iter_t **iter)
{
    (void)session;
    *iter = (sr_val_iter_t *)strdup(xpath);
    return SR_ERR_OK;
}

int
__wrap_sr_get_item_next(sr_session_ctx_t *session, sr_val_iter_t *iter, sr_val_t **value)
{
    static struct ly_set *set = NULL;
    const char *xpath = (const char *)iter;
    char *path;
    const char *ietf_interfaces_xpath = "/ietf-interfaces:";
    size_t ietf_interfaces_xpath_len = strlen(ietf_interfaces_xpath);
    const char *simplified_melt_xpath = "/simplified-melt:";
    size_t simplified_melt_xpath_len = strlen(simplified_melt_xpath);
    (void)session;

    if (!strncmp(xpath, ietf_interfaces_xpath, ietf_interfaces_xpath_len)) {
        if (!set) {
            set = lyd_find_path(ietf_if_data, xpath);
        }
    } else if (!strncmp(xpath, simplified_melt_xpath, simplified_melt_xpath_len)) {
        if (!set) {
            set = lyd_find_path(simplified_melt_data, xpath);
        }
    } else {
        *value = NULL;
        set = NULL;
        return SR_ERR_NOT_FOUND;
    }

    if (!set->number) {
        ly_set_free(set);
        set = NULL;
        return SR_ERR_NOT_FOUND;
    }

    path = lyd_path(set->set.d[0]);

    /* Copied from sysrepo rp_dt_create_xpath_for_node() */

    /* remove leaf-list predicate */
    if (LYS_LEAFLIST & set->set.d[0]->schema->nodetype) {
        char *leaf_list_name = strstr(path, "[.='");
        if (NULL != leaf_list_name) {
            *leaf_list_name = 0;
        } else if (NULL != (leaf_list_name = strstr(path, "[.=\""))) {
            *leaf_list_name = 0;
        }
    }
    /* End copy */

    *value = calloc(1, sizeof **value);
    op_set_srval(set->set.d[0], path, 1, *value, NULL);
    (*value)->dflt = set->set.d[0]->dflt;
    free(path);

    --set->number;
    if (set->number) {
        memmove(set->set.d, set->set.d + 1, set->number * sizeof(void *));
    }

    return SR_ERR_OK;
}

int
__wrap_sr_session_set_options(sr_session_ctx_t *session, const sr_sess_options_t opts)
{
    (void)session;
    (void)opts;
    return SR_ERR_OK;
}

/*
 * LIBNETCONF2 WRAPPER FUNCTIONS
 */
NC_MSG_TYPE
__wrap_nc_accept(int timeout, struct nc_session **session)
{
    NC_MSG_TYPE ret;

    if (!initialized) {
        pipe(pipes[0]);
        pipe(pipes[1]);

        fcntl(pipes[0][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[0][1], F_SETFL, O_NONBLOCK);
        fcntl(pipes[1][0], F_SETFL, O_NONBLOCK);
        fcntl(pipes[1][1], F_SETFL, O_NONBLOCK);

        p_in = pipes[0][0];
        p_out = pipes[1][1];

        *session = calloc(1, sizeof **session);
        (*session)->status = NC_STATUS_RUNNING;
        (*session)->side = 1;
        (*session)->id = 1;
        (*session)->io_lock = malloc(sizeof *(*session)->io_lock);
        pthread_mutex_init((*session)->io_lock, NULL);
        (*session)->opts.server.rpc_lock = malloc(sizeof *(*session)->opts.server.rpc_lock);
        pthread_mutex_init((*session)->opts.server.rpc_lock, NULL);
        (*session)->opts.server.rpc_cond = malloc(sizeof *(*session)->opts.server.rpc_cond);
        pthread_cond_init((*session)->opts.server.rpc_cond, NULL);
        (*session)->opts.server.rpc_inuse = malloc(sizeof *(*session)->opts.server.rpc_inuse);
        *(*session)->opts.server.rpc_inuse = 0;
        (*session)->ti_type = NC_TI_FD;
        (*session)->ti.fd.in = pipes[1][0];
        (*session)->ti.fd.out = pipes[0][1];
        (*session)->ctx = np2srv.ly_ctx;
        (*session)->flags = 1; //shared ctx
        (*session)->username = "user1";
        (*session)->host = "localhost";
        (*session)->opts.server.session_start = (*session)->opts.server.last_rpc = time(NULL);
        printf("test: New session 1\n");
        initialized = 1;
        if (sem_post(&wait_for_init)) {
            perror("sem_post");
            exit(1);
        }
        ret = NC_MSG_HELLO;
    } else {
        usleep(timeout * 1000);
        ret = NC_MSG_WOULDBLOCK;
    }

    return ret;
}

void
__wrap_nc_session_free(struct nc_session *session, void (*data_free)(void *))
{
    if (data_free) {
        data_free(session->data);
    }
    pthread_mutex_destroy(session->io_lock);
    free(session->io_lock);
    pthread_mutex_destroy(session->opts.server.rpc_lock);
    free(session->opts.server.rpc_lock);
    pthread_cond_destroy(session->opts.server.rpc_cond);
    free(session->opts.server.rpc_cond);
    free((int *)session->opts.server.rpc_inuse);
    free(session);
}

int
__wrap_nc_server_endpt_count(void)
{
    return 1;
}

/*
 * SERVER THREAD
 */
pthread_t server_tid;
static void *
server_thread(void *arg)
{
    (void)arg;
    char *argv[] = {"netopeer2-server", "-d", "-v2"};

    return (void *)(int64_t)server_main(3, argv);
}

/*
 * TEST
 */
static int
np_start(void **state)
{
    const char *ietf_if_xml =
    "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
      "<interface>"
      "<name>iface1/1</name>"
      "<description>iface1/1 dsc</description>"
      "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
      "<enabled>true</enabled>"
      "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
      "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
        "<enabled>true</enabled>"
        "<forwarding>true</forwarding>"
        "<mtu>68</mtu>"
        "<neighbor>"
        "<ip>10.0.0.2</ip>"
        "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
        "</neighbor>"
      "</ipv4>"
      "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
        "<enabled>true</enabled>"
        "<forwarding>false</forwarding>"
      "</ipv6>"
      "</interface>"
      "<interface>"
      "<name>'iface1/2'</name>"
      "<description>iface1/2 dsc</description>"
      "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
      "<enabled>true</enabled>"
      "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
      "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
        "<enabled>true</enabled>"
        "<forwarding>true</forwarding>"
        "<mtu>68</mtu>"
        "<neighbor>"
        "<ip>10.0.0.2</ip>"
        "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
        "</neighbor>"
      "</ipv4>"
      "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
        "<enabled>true</enabled>"
        "<forwarding>false</forwarding>"
      "</ipv6>"
      "</interface>"
    "</interfaces>";
    const char *simplified_melt_xml =
    "<melt xmlns=\"urn:ietf:params:xml:ns:yang:simplified-melt\">"
      "<pmd-profile>"
      "<name>melt-pmd-01</name>"
      "<measurement-class>melt-cdcr</measurement-class>"
      "</pmd-profile>"
    "</melt>";
    (void)state; /* unused */

    optind = 1;
    control = LOOP_CONTINUE;
    initialized = 0;
    assert_int_equal(pthread_create(&server_tid, NULL, server_thread, NULL), 0);

    if (sem_wait(&wait_for_init)) {
        perror("sem_wait");
        exit(1);
    }

    ietf_if_data = lyd_parse_mem(np2srv.ly_ctx, ietf_if_xml, LYD_XML, LYD_OPT_CONFIG);
    assert_ptr_not_equal(ietf_if_data, NULL);
    simplified_melt_data = lyd_parse_mem(np2srv.ly_ctx, simplified_melt_xml, LYD_XML, LYD_OPT_CONFIG);
    assert_ptr_not_equal(simplified_melt_data, NULL);

    return 0;
}

static int
np_stop(void **state)
{
    (void)state; /* unused */
    int64_t ret;

    lyd_free_withsiblings(ietf_if_data);
    lyd_free_withsiblings(simplified_melt_data);

    control = LOOP_STOP;
    assert_int_equal(pthread_join(server_tid, (void **)&ret), 0);

    close(pipes[0][0]);
    close(pipes[0][1]);
    close(pipes[1][0]);
    close(pipes[1][1]);

#ifdef NP2SRV_ENABLED_URL_CAPABILITY
    unlink(URL_TESTFILE);
#endif
    return ret;
}

static void
test_copy_config(void **state)
{
    (void)state; /* unused */
    const char *copy_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<copy-config>"
            "<target>"
                "<running/>"
            "</target>"
            "<source>"
                "<config>"
"<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
  "<interface>"
    "<name>iface1/1</name>"
    "<description>iface1/1 dsc</description>"
    "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
    "<enabled>true</enabled>"
    "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
    "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
      "<enabled>true</enabled>"
      "<forwarding>true</forwarding>"
      "<mtu>68</mtu>"
      "<neighbor>"
        "<ip>10.0.0.2</ip>"
        "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
      "</neighbor>"
    "</ipv4>"
    "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
      "<enabled>true</enabled>"
      "<forwarding>false</forwarding>"
    "</ipv6>"
  "</interface>"
  "<interface>"
    "<name>'iface1/2'</name>"
    "<description>iface1/2 dsc</description>"
    "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
    "<enabled>true</enabled>"
    "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
    "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
      "<enabled>true</enabled>"
      "<forwarding>true</forwarding>"
      "<mtu>68</mtu>"
      "<neighbor>"
        "<ip>10.0.0.2</ip>"
        "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
      "</neighbor>"
    "</ipv4>"
    "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
      "<enabled>true</enabled>"
      "<forwarding>false</forwarding>"
    "</ipv6>"
  "</interface>"
"</interfaces>"
                "</config>"
            "</source>"
        "</copy-config>"
    "</rpc>";
    const char *copy_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<ok/>"
    "</rpc-reply>";

    set_item_count = 0;
    test_write(p_out, copy_rpc, __LINE__);
    test_read(p_in, copy_rpl, __LINE__);
}

#ifdef NP2SRV_ENABLED_URL_CAPABILITY
static void
test_copy_config_from_url(void **state)
{
    (void)state; /* unused */
    const char *copy_data =
    "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
        "<interface>"
          "<name>iface1/1</name>"
          "<description>iface1/1 dsc</description>"
          "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
          "<enabled>true</enabled>"
          "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
          "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
            "<enabled>true</enabled>"
            "<forwarding>true</forwarding>"
            "<mtu>68</mtu>"
            "<neighbor>"
              "<ip>10.0.0.2</ip>"
              "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
            "</neighbor>"
          "</ipv4>"
          "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
            "<enabled>true</enabled>"
            "<forwarding>false</forwarding>"
          "</ipv6>"
        "</interface>"
        "<interface>"
          "<name>'iface1/2'</name>"
          "<description>iface1/2 dsc</description>"
          "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
          "<enabled>true</enabled>"
          "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
          "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
            "<enabled>true</enabled>"
            "<forwarding>true</forwarding>"
            "<mtu>68</mtu>"
            "<neighbor>"
              "<ip>10.0.0.2</ip>"
              "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
            "</neighbor>"
          "</ipv4>"
          "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
            "<enabled>true</enabled>"
            "<forwarding>false</forwarding>"
          "</ipv6>"
        "</interface>"
      "</interfaces>"
    "</config>";
    const char *copy_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<copy-config>"
            "<target>"
                "<running/>"
            "</target>"
            "<source>"
                "<url>file://" URL_TESTFILE "</url>"
            "</source>"
        "</copy-config>"
    "</rpc>";
    const char *copy_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<ok/>"
    "</rpc-reply>";

    FILE* xmlfile = fopen(URL_TESTFILE, "w");
    fprintf(xmlfile, "%s", copy_data);
    fclose(xmlfile);

    set_item_count = 0;
    test_write(p_out, copy_rpc, __LINE__);
    test_read(p_in, copy_rpl, __LINE__);
}

static void
test_copy_config_to_url(void **state)
{
    (void)state; /* unused */
    const char *copy_rpc =
    "<rpc msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<copy-config>"
            "<target>"
                "<url>file://" URL_TESTFILE "</url>"
            "</target>"
            "<source>"
                "<running/>"
            "</source>"
        "</copy-config>"
    "</rpc>";
    const char *copy_rpl =
    "<rpc-reply msgid=\"1\" xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
        "<ok/>"
    "</rpc-reply>";
    const char *copy_data =
    "<config xmlns=\"urn:ietf:params:xml:ns:netconf:base:1.0\">"
      "<interfaces xmlns=\"urn:ietf:params:xml:ns:yang:ietf-interfaces\">"
        "<interface>"
        "<name>iface1/1</name>"
        "<description>iface1/1 dsc</description>"
        "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
        "<enabled>true</enabled>"
        "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
        "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
          "<enabled>true</enabled>"
          "<forwarding>true</forwarding>"
          "<mtu>68</mtu>"
          "<neighbor>"
          "<ip>10.0.0.2</ip>"
          "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
          "</neighbor>"
        "</ipv4>"
        "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
          "<enabled>true</enabled>"
          "<forwarding>false</forwarding>"
        "</ipv6>"
        "</interface>"
        "<interface>"
        "<name>'iface1/2'</name>"
        "<description>iface1/2 dsc</description>"
        "<type xmlns:ianaift=\"urn:ietf:params:xml:ns:yang:iana-if-type\">ianaift:ethernetCsmacd</type>"
        "<enabled>true</enabled>"
        "<link-up-down-trap-enable>disabled</link-up-down-trap-enable>"
        "<ipv4 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
          "<enabled>true</enabled>"
          "<forwarding>true</forwarding>"
          "<mtu>68</mtu>"
          "<neighbor>"
          "<ip>10.0.0.2</ip>"
          "<link-layer-address>01:34:56:78:9a:bc:de:f0</link-layer-address>"
          "</neighbor>"
        "</ipv4>"
        "<ipv6 xmlns=\"urn:ietf:params:xml:ns:yang:ietf-ip\">"
          "<enabled>true</enabled>"
          "<forwarding>false</forwarding>"
        "</ipv6>"
        "</interface>"
      "</interfaces>"
      "<melt xmlns=\"urn:ietf:params:xml:ns:yang:simplified-melt\">"
        "<pmd-profile>"
          "<name>melt-pmd-01</name>"
          "<measurement-class>melt-cdcr</measurement-class>"
        "</pmd-profile>"
      "</melt>"
    "</config>";

    set_item_count = 0;
    test_write(p_out, copy_rpc, __LINE__);
    test_read(p_in, copy_rpl, __LINE__);
    test_file(URL_TESTFILE, copy_data, __LINE__);
}
#endif

static void
test_startstop(void **state)
{
    (void)state; /* unused */
    return;
}


int
main(void)
{
    if (sem_init(&wait_for_init, 0, 0)) {
        perror("sem_init");
        return 1;
    }
    const struct CMUnitTest tests[] = {
                    cmocka_unit_test_setup(test_startstop, np_start),
                    cmocka_unit_test(test_copy_config),
#ifdef NP2SRV_ENABLED_URL_CAPABILITY
                    cmocka_unit_test(test_copy_config_from_url),
                    cmocka_unit_test(test_copy_config_to_url),
#endif
                    cmocka_unit_test_teardown(test_startstop, np_stop),
    };

    if (setenv("CMOCKA_TEST_ABORT", "1", 1)) {
        fprintf(stderr, "Cannot set Cmocka thread environment variable.\n");
    }
    int res = cmocka_run_group_tests(tests, NULL, NULL);
    if (sem_destroy(&wait_for_init)) {
        perror("sem_destroy");
        return 1;
    }
    return res;
}
