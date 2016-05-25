/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/**
 * This file implements a simple in-RAM key database for long-term keys.  A key
 * is inserted into the database immediately after a successful pairing
 * procedure.  A key is retrieved from the database when the central performs
 * the encryption procedure (bonding).
 *
 * As this database is only stored in RAM, its contents are lost if bleprph is
 * restarted.
 */

#include <inttypes.h>
#include <string.h>

#include "console/console.h"
#include "host/ble_hs.h"

#include "bleprph.h"

#define STORE_MAX_SLV_LTKS   4
#define STORE_MAX_CCCDS      16

static struct ble_store_value_ltk store_slv_ltks[STORE_MAX_SLV_LTKS];
static int store_num_slv_ltks;

static struct ble_store_value_cccd store_cccds[STORE_MAX_CCCDS];
static int store_num_cccds;

/*****************************************************************************
 * $ltk                                                                      *
 *****************************************************************************/

static void
store_print_value_ltk(struct ble_store_value_ltk *ltk)
{
    BLEPRPH_LOG(INFO, "ediv=%u rand=%llu authenticated=%d ", ltk->ediv,
                   ltk->rand_num, ltk->authenticated);
    BLEPRPH_LOG(INFO, "ltk=");
    print_bytes(ltk->key, 16);
    BLEPRPH_LOG(INFO, "\n");
}

static void
store_print_key_ltk(struct ble_store_key_ltk *key_ltk)
{
    if (key_ltk->peer_addr_type != BLE_STORE_ADDR_TYPE_NONE) {
        BLEPRPH_LOG(INFO, "peer_addr_type=%d peer_addr=",
                       key_ltk->peer_addr_type);
        print_bytes(key_ltk->peer_addr, 6);
        BLEPRPH_LOG(INFO, " ");
    }
    if (key_ltk->ediv_present) {
        BLEPRPH_LOG(INFO, "ediv=0x%02x ", key_ltk->ediv);
    }
    if (key_ltk->rand_num_present) {
        BLEPRPH_LOG(INFO, "rand=0x%llx ", key_ltk->rand_num);
    }
}

static int
store_find_ltk(struct ble_store_key_ltk *key_ltk,
               struct ble_store_value_ltk *value_ltks, int num_value_ltks)
{
    struct ble_store_value_ltk *cur;
    int i;

    for (i = 0; i < num_value_ltks; i++) {
        cur = value_ltks + i;

        if (key_ltk->peer_addr_type != BLE_STORE_ADDR_TYPE_NONE) {
            if (cur->peer_addr_type != key_ltk->peer_addr_type) {
                continue;
            }

            if (memcmp(cur->peer_addr, key_ltk->peer_addr,
                       sizeof cur->peer_addr) != 0) {
                continue;
            }
        }

        if (key_ltk->ediv_present && cur->ediv != key_ltk->ediv) {
            continue;
        }

        if (key_ltk->rand_num_present && cur->rand_num != key_ltk->rand_num) {
            continue;
        }

        return i;
    }

    return -1;
}

static int
store_read_slv_ltk(struct ble_store_key_ltk *key_ltk,
                   struct ble_store_value_ltk *value_ltk)
{
    int idx;

    idx = store_find_ltk(key_ltk, store_slv_ltks, store_num_slv_ltks);
    if (idx == -1) {
        return BLE_HS_ENOENT;
    }

    *value_ltk = store_slv_ltks[idx];
    return 0;
}

static int
store_write_slv_ltk(struct ble_store_value_ltk *value_ltk)
{
    struct ble_store_key_ltk key_ltk;
    int idx;

    BLEPRPH_LOG(INFO, "persisting slv ltk; ");
    store_print_value_ltk(value_ltk);

    ble_store_key_from_value_ltk(&key_ltk, value_ltk);
    idx = store_find_ltk(&key_ltk, store_slv_ltks, store_num_slv_ltks);
    if (idx == -1) {
        if (store_num_slv_ltks >= STORE_MAX_SLV_LTKS) {
            BLEPRPH_LOG(INFO, "error persisting slv ltk; too many entries "
                              "(%d)\n", store_num_slv_ltks);
            return BLE_HS_ENOMEM;
        }

        idx = store_num_slv_ltks;
        store_num_slv_ltks++;
    }

    store_slv_ltks[idx] = *value_ltk;
    return 0;
}

/*****************************************************************************
 * $cccd                                                                     *
 *****************************************************************************/

static int
store_find_cccd(struct ble_store_key_cccd *key)
{
    struct ble_store_value_cccd *cccd;
    int skipped;
    int i;

    skipped = 0;
    for (i = 0; i < store_num_cccds; i++) {
        cccd = store_cccds + i;

        if (key->peer_addr_type != BLE_STORE_ADDR_TYPE_NONE) {
            if (cccd->peer_addr_type != key->peer_addr_type) {
                continue;
            }

            if (memcmp(cccd->peer_addr, key->peer_addr, 6) != 0) {
                continue;
            }
        }

        if (key->chr_val_handle != 0) {
            if (cccd->chr_val_handle != key->chr_val_handle) {
                continue;
            }
        }

        if (key->idx > skipped) {
            skipped++;
            continue;
        }

        return i;
    }

    return -1;
}

static int
store_read_cccd(struct ble_store_key_cccd *key_cccd,
                struct ble_store_value_cccd *value_cccd)
{
    int idx;

    idx = store_find_cccd(key_cccd);
    if (idx == -1) {
        return BLE_HS_ENOENT;
    }

    *value_cccd = store_cccds[idx];
    return 0;
}

static int
store_write_cccd(struct ble_store_value_cccd *value_cccd)
{
    struct ble_store_key_cccd key_cccd;
    int idx;

    ble_store_key_from_value_cccd(&key_cccd, value_cccd);
    idx = store_find_cccd(&key_cccd);
    if (idx == -1) {
        if (store_num_cccds >= STORE_MAX_SLV_LTKS) {
            BLEPRPH_LOG(INFO, "error persisting cccd; too many entries (%d)\n",
                           store_num_cccds);
            return BLE_HS_ENOMEM;
        }

        idx = store_num_cccds;
        store_num_cccds++;
    }

    store_cccds[idx] = *value_cccd;
    return 0;
}

/*****************************************************************************
 * $api                                                                      *
 *****************************************************************************/

/**
 * Searches the database for an object matching the specified criteria.
 *
 * @return                      0 if a key was found; else BLE_HS_ENOENT.
 */
int
store_read(int obj_type, union ble_store_key *key,
           union ble_store_value *value)
{
    int rc;

    switch (obj_type) {
    case BLE_STORE_OBJ_TYPE_SLV_LTK:
        /* An encryption procedure (bonding) is being attempted.  The nimble
         * stack is asking us to look in our key database for a long-term key
         * corresponding to the specified ediv and random number.
         *
         * Perform a key lookup and populate the context object with the
         * result.  The nimble stack will use this key if this function returns
         * success.
         */
        BLEPRPH_LOG(INFO, "looking up slv ltk; ");
        store_print_key_ltk(&key->ltk);
        BLEPRPH_LOG(INFO, "\n");
        rc = store_read_slv_ltk(&key->ltk, &value->ltk);
        return rc;

    case BLE_STORE_OBJ_TYPE_CCCD:
        rc = store_read_cccd(&key->cccd, &value->cccd);
        return rc;

    default:
        return BLE_HS_ENOTSUP;
    }
}

/**
 * Adds the specified object to the database.
 *
 * @return                      0 on success; BLE_HS_ENOMEM if the database is
 *                                  full.
 */
int
store_write(int obj_type, union ble_store_value *val)
{
    int rc;

    switch (obj_type) {
    case BLE_STORE_OBJ_TYPE_SLV_LTK:
        rc = store_write_slv_ltk(&val->ltk);
        return rc;

    case BLE_STORE_OBJ_TYPE_CCCD:
        rc = store_write_cccd(&val->cccd);
        return rc;

    default:
        return BLE_HS_ENOTSUP;
    }
}
