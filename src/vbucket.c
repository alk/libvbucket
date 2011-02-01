/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 NorthScale, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */



#include "config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libhashkit/hashkit.h>

#include "cJSON.h"
#include <libvbucket/vbucket.h>

#define MAX_CONFIG_SIZE 100 * 1048576
#define MAX_BUCKETS 65536
#define MAX_REPLICAS 4
#define STRINGIFY(X) #X

struct server_st {
    char *authority;        /* host:port */
    char *couchdb_api_base;
};

struct vbucket_st {
    int servers[MAX_REPLICAS + 1];
};

struct vbucket_config_st {
    hashkit_hash_algorithm_t hk_algorithm;
    int num_vbuckets;
    int mask;
    int num_servers;
    int num_replicas;
    char *user;
    char *password;
    struct server_st *servers;
    struct vbucket_st *fvbuckets;
    struct vbucket_st vbuckets[];
};

static char *errstr = NULL;

const char *vbucket_get_error() {
    return errstr;
}

static hashkit_hash_algorithm_t lookup_hash_algorithm(const char *s) {
    static char *hashes[HASHKIT_HASH_MAX];
    unsigned int i;
    hashes[HASHKIT_HASH_DEFAULT] = "default";
    hashes[HASHKIT_HASH_MD5] = "md5";
    hashes[HASHKIT_HASH_CRC] = "crc";
    hashes[HASHKIT_HASH_FNV1_64] = "fnv1_64";
    hashes[HASHKIT_HASH_FNV1A_64] = "fnv1a_64";
    hashes[HASHKIT_HASH_FNV1_32] = "fnv1_32";
    hashes[HASHKIT_HASH_FNV1A_32] = "fnv1a_32";
    hashes[HASHKIT_HASH_HSIEH] = "hsieh";
    hashes[HASHKIT_HASH_MURMUR] = "murmur";
    hashes[HASHKIT_HASH_JENKINS] = "jenkins";
    for (i = 0; i < sizeof(hashes); ++i) {
        if (hashes[i] != NULL && strcasecmp(s, hashes[i]) == 0) {
            return i;
        }
    }
    return HASHKIT_HASH_MAX;
}

static void set_username(struct vbucket_config_st *vbc, const char *user)
{
    if (vbc->user == NULL && user != NULL && strcmp(user, "default") != 0) {
        vbc->user = strdup(user);
    }
}

static void set_password(struct vbucket_config_st *vbc, const char *password)
{
    if (vbc->password == NULL && password != NULL) {
        vbc->password = strdup(password);
    }
}

static struct vbucket_config_st *config_create(char *hash_algorithm,
                                               int num_servers,
                                               int num_vbuckets,
                                               int num_replicas,
                                               char *user,
                                               char *password)
{
    struct vbucket_config_st *vb;
    hashkit_hash_algorithm_t ha = lookup_hash_algorithm(hash_algorithm);
    if (ha == HASHKIT_HASH_MAX) {
        errstr = "Bogus hash algorithm specified";
        return NULL;
    }

    vb = calloc(sizeof(struct vbucket_config_st) +
                sizeof(struct vbucket_st) * num_vbuckets, 1);
    if (vb == NULL) {
        errstr = "Failed to allocate vbucket config struct";
        return NULL;
    }

    vb->hk_algorithm = ha;

    vb->servers = calloc(sizeof(struct server_st), num_servers);
    if (vb->servers == NULL) {
        free(vb);
        errstr = "Failed to allocate servers array";
        return NULL;
    }
    vb->num_servers = num_servers;
    vb->num_vbuckets = num_vbuckets;
    vb->num_replicas = num_replicas;
    vb->mask = num_vbuckets - 1;
    set_username(vb, user);
    set_password(vb, password);

    return vb;
}

void vbucket_config_destroy(VBUCKET_CONFIG_HANDLE vb) {
    int i;
    for (i = 0; i < vb->num_servers; ++i) {
        free(vb->servers[i].authority);
        free(vb->servers[i].couchdb_api_base);
    }
    free(vb->servers);
    free(vb->user);
    free(vb->password);
    free(vb->fvbuckets);
    memset(vb, 0xff, sizeof(vb));
    free(vb);
}

static int populate_servers(struct vbucket_config_st *vb, cJSON *c) {
    int i;
    for (i = 0; i < vb->num_servers; ++i) {
        char *server;
        cJSON *jServer = cJSON_GetArrayItem(c, i);
        if (jServer == NULL || jServer->type != cJSON_String) {
            errstr = "Expected array of strings for serverList";
            return -1;
        }
        server = strdup(jServer->valuestring);
        if (server == NULL) {
            errstr = "Failed to allocate storage for server string";
            return -1;
        }
        vb->servers[i].authority = server;
    }
    return 0;
}

static int lookup_server_struct(struct vbucket_config_st *vb, cJSON *c) {
    char *authority = NULL, *hostname = NULL, *colon = NULL;
    int port = -1, idx = -1, ii;

    cJSON *obj = cJSON_GetObjectItem(c, "hostname");
    if (obj == NULL || obj->type != cJSON_String) {
        errstr = "Expected string for node's hostname";
        return -1;
    }
    hostname = obj->valuestring;
    obj = cJSON_GetObjectItem(c, "ports");
    if (obj == NULL || obj->type != cJSON_Object) {
        errstr = "Expected object for node's ports";
        return -1;
    }
    obj = cJSON_GetObjectItem(obj, "direct");
    if (obj == NULL || obj->type != cJSON_Number) {
        errstr = "Expected number for node's direct port";
        return -1;
    }
    port = obj->valueint;
    authority = calloc(strlen(hostname) + 6, sizeof(char)); /* hostname+port */
    if (authority == NULL) {
        errstr = "Failed to allocate storage for authority string";
        return -1;
    }
    sprintf(authority, "%s", hostname);
    colon = strchr(authority, ':');
    if (!colon) {
        colon = authority + strlen(authority);
    }
    sprintf(colon, ":%d", port);

    for (ii = 0; ii < vb->num_servers; ++ii) {
        if (strcmp(vb->servers[ii].authority, authority) == 0) {
            idx = ii;
        }
    }

    free(authority);
    return idx;
}

static int populate_node_info(struct vbucket_config_st *vb, cJSON *c) {
    int idx, ii;

    for (ii = 0; ii < cJSON_GetArraySize(c); ++ii) {
        cJSON *jNode = cJSON_GetArrayItem(c, ii);
        if (jNode) {
            if (jNode->type != cJSON_Object) {
                errstr = "Expected object for nodes array item";
                return -1;
            }

            if ((idx = lookup_server_struct(vb, jNode)) >= 0) {
                cJSON *obj = cJSON_GetObjectItem(jNode, "couchApiBase");
                if (obj != NULL) {
                    char *value = strdup(obj->valuestring);
                    if (value == NULL) {
                        errstr = "Failed to allocate storage for couchApiBase string";
                        return -1;
                    }
                    vb->servers[idx].couchdb_api_base = value;
                }
            }
        }
    }
    return 0;
}

static int populate_buckets(struct vbucket_config_st *vb, cJSON *c, int is_ft)
{
    int i, j;
    struct vbucket_st *vbucket_map = NULL;

    if (is_ft) {
        if (!(vb->fvbuckets = malloc(vb->num_vbuckets * sizeof(struct vbucket_st)))) {
            errstr = "Failed to allocate storage for forward vbucket map";
            return -1;
        }
    }

    vbucket_map = (is_ft ? vb->fvbuckets : vb->vbuckets);

    for (i = 0; i < vb->num_vbuckets; ++i) {
        cJSON *jBucket = cJSON_GetArrayItem(c, i);
        if (jBucket == NULL || jBucket->type != cJSON_Array ||
            cJSON_GetArraySize(jBucket) != vb->num_replicas + 1) {
            errstr = "Expected array of arrays each with numReplicas + 1 ints for vBucketMap";
            return -1;
        }
        for (j = 0; j < vb->num_replicas + 1; ++j) {
            cJSON *jServerId = cJSON_GetArrayItem(jBucket, j);
            if (jServerId == NULL || jServerId->type != cJSON_Number ||
                jServerId->valueint < -1 || jServerId->valueint >= vb->num_servers) {
                errstr = "Server ID must be >= -1 and < num_servers";
                return -1;
            }
            vbucket_map[i].servers[j] = jServerId->valueint;
        }
    }
    return 0;
}

static char *get_char_val(cJSON *c, const char *key) {
    cJSON *obj = cJSON_GetObjectItem(c, key);
    return (obj != NULL && obj->type == cJSON_String) ? obj->valuestring : NULL;
}

static VBUCKET_CONFIG_HANDLE parse_cjson(cJSON *c)
{
    char *user;
    char *password;
    cJSON *body;
    cJSON *jHashAlgorithm;
    char *hashAlgorithm;
    cJSON *jNumReplicas;
    int numReplicas;
    cJSON *jServers;
    int numServers;
    cJSON *jBuckets;
    cJSON *jBucketsForward;
    int numBuckets;
    struct vbucket_config_st *vb;

    user = get_char_val(c, "name");
    password = get_char_val(c, "saslPassword");

    body = cJSON_GetObjectItem(c, "vBucketServerMap");
    if (body != NULL) {
        VBUCKET_CONFIG_HANDLE ret = parse_cjson(body); // Allows clients to have a JSON envelope.
        if (ret != NULL) {
            cJSON *jNodes;
            set_username(ret, user);
            set_password(ret, password);

            jNodes = cJSON_GetObjectItem(c, "nodes");
            if (jNodes) {
                if (jNodes->type != cJSON_Array) {
                    errstr = "Expected array for nodes";
                    return NULL;
                }
                if (populate_node_info(ret, jNodes) != 0) {
                    vbucket_config_destroy(ret);
                    return NULL;
                }
            }
        }
        return ret;
    }


    jHashAlgorithm = cJSON_GetObjectItem(c, "hashAlgorithm");
    if (jHashAlgorithm == NULL || jHashAlgorithm->type != cJSON_String) {
        errstr = "Expected string for hashAlgorithm";
        return NULL;
    }
    hashAlgorithm = jHashAlgorithm->valuestring;

    jNumReplicas = cJSON_GetObjectItem(c, "numReplicas");
    if (jNumReplicas == NULL || jNumReplicas->type != cJSON_Number ||
        jNumReplicas->valueint > MAX_REPLICAS) {
        errstr = "Expected number <= " STRINGIFY(MAX_REPLICAS) " for numReplicas";
        return NULL;
    }
    numReplicas = jNumReplicas->valueint;

    jServers = cJSON_GetObjectItem(c, "serverList");
    if (jServers == NULL || jServers->type != cJSON_Array) {
        errstr = "Expected array for serverList";
        return NULL;
    }

    numServers = cJSON_GetArraySize(jServers);
    if (numServers == 0) {
        errstr = "Empty serverList";
        return NULL;
    }

    jBuckets = cJSON_GetObjectItem(c, "vBucketMap");
    if (jBuckets == NULL || jBuckets->type != cJSON_Array) {
        errstr = "Expected array for vBucketMap";
        return NULL;
    }

    /* this could possibly be null */
    jBucketsForward = cJSON_GetObjectItem(c, "vBucketMapForward");
    if (jBuckets && jBuckets->type != cJSON_Array) {
        errstr = "Expected array for vBucketMap";
        return NULL;
    }


    numBuckets = cJSON_GetArraySize(jBuckets);
    if (numBuckets == 0 || (numBuckets & (numBuckets - 1)) != 0) {
        errstr = "Number of buckets must be a power of two > 0 and <= " STRINGIFY(MAX_BUCKETS);
        return NULL;
    }

    vb = config_create(hashAlgorithm, numServers, numBuckets, numReplicas,
                       user, password);
    if (vb == NULL) {
        return NULL;
    }

    if (populate_servers(vb, jServers) != 0) {
        vbucket_config_destroy(vb);
        return NULL;
    }

    if (populate_buckets(vb, jBuckets, 0) != 0) {
        vbucket_config_destroy(vb);
        return NULL;
    }

    if (jBucketsForward) {
        if (populate_buckets(vb, jBucketsForward, 1) !=0) {
            vbucket_config_destroy(vb);
            return NULL;
        }
    }

    return vb;
}

VBUCKET_CONFIG_HANDLE vbucket_config_parse_string(const char *data) {
    VBUCKET_CONFIG_HANDLE vb;
    cJSON *c = cJSON_Parse(data);
    errstr = "Failed to parse data";
    if (c == NULL) {
        return NULL;
    }

    vb = parse_cjson(c);

    cJSON_Delete(c);
    return vb;
}

VBUCKET_CONFIG_HANDLE vbucket_config_parse_file(const char *filename)
{
    long size;
    char *data;
    size_t nread;
    VBUCKET_CONFIG_HANDLE h;

    FILE *f = fopen(filename, "r");
    if (f == NULL) {
        errstr = "Unable to open file";
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size > MAX_CONFIG_SIZE) {
        fclose(f);
        errstr = "File too large";
        return NULL;
    }
    data = calloc(sizeof(char), size+1);
    if (data == NULL) {
        errstr = "Unable to allocate buffer to read file";
        return NULL;
    }
    nread = fread(data, sizeof(char), size+1, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(data);
        errstr = "Failed to read entire file";
        return NULL;
    }
    h = vbucket_config_parse_string(data);
    free(data);
    return h;
}

int vbucket_config_get_num_replicas(VBUCKET_CONFIG_HANDLE vb) {
    return vb->num_replicas;
}

int vbucket_config_get_num_vbuckets(VBUCKET_CONFIG_HANDLE vb) {
    return vb->num_vbuckets;
}

int vbucket_config_get_num_servers(VBUCKET_CONFIG_HANDLE vb) {
    return vb->num_servers;
}

const char *vbucket_config_get_couch_api_base(VBUCKET_CONFIG_HANDLE vb, int i) {
    return vb->servers[i].couchdb_api_base;
}

const char *vbucket_config_get_server(VBUCKET_CONFIG_HANDLE vb, int i) {
    return vb->servers[i].authority;
}

const char *vbucket_config_get_user(VBUCKET_CONFIG_HANDLE vb) {
    return vb->user;
}

const char *vbucket_config_get_password(VBUCKET_CONFIG_HANDLE vb) {
    return vb->password;
}

int vbucket_get_vbucket_by_key(VBUCKET_CONFIG_HANDLE vb, const void *key, size_t nkey) {
    uint32_t digest = libhashkit_digest(key, nkey, vb->hk_algorithm);
    return digest & vb->mask;
}

int vbucket_get_master(VBUCKET_CONFIG_HANDLE vb, int vbucket) {
    return vb->vbuckets[vbucket].servers[0];
}

int vbucket_get_replica(VBUCKET_CONFIG_HANDLE vb, int vbucket, int i) {
    return vb->vbuckets[vbucket].servers[i+1];
}

int vbucket_found_incorrect_master(VBUCKET_CONFIG_HANDLE vb, int vbucket,
                                   int wrongserver) {
    int mappedServer = vb->vbuckets[vbucket].servers[0];
    int rv = mappedServer;
    /*
     * if a forward table exists, then return the vbucket id from the forward table
     * and update that information in the current table. We also need to Update the
     * replica information for that vbucket
     */
    if (vb->fvbuckets) {
        int i = 0;
        rv = vb->vbuckets[vbucket].servers[0] = vb->fvbuckets[vbucket].servers[0];
        for (i = 0; i < vb->num_replicas; i++) {
            vb->vbuckets[vbucket].servers[i+1] = vb->fvbuckets[vbucket].servers[i+1];
        }
    } else if (mappedServer == wrongserver) {
        rv = (rv + 1) % vb->num_servers;
        vb->vbuckets[vbucket].servers[0] = rv;
    }

    return rv;
}

static void compute_vb_list_diff(VBUCKET_CONFIG_HANDLE from,
                                 VBUCKET_CONFIG_HANDLE to,
                                 char **out) {
    int offset = 0;
    int i, j;
    for (i = 0; i < to->num_servers; i++) {
        bool found = false;
        const char *sn = vbucket_config_get_server(to, i);
        for (j = 0; !found && j < from->num_servers; j++) {
            const char *sn2 = vbucket_config_get_server(from, j);
            found |= (strcmp(sn2, sn) == 0);
        }
        if (!found) {
            out[offset] = strdup(sn);
            assert(out[offset]);
            ++offset;
        }
    }
}

VBUCKET_CONFIG_DIFF* vbucket_compare(VBUCKET_CONFIG_HANDLE from,
                                     VBUCKET_CONFIG_HANDLE to) {
    VBUCKET_CONFIG_DIFF *rv = calloc(1, sizeof(VBUCKET_CONFIG_DIFF));
    int num_servers = (from->num_servers > to->num_servers
                       ? from->num_servers : to->num_servers) + 1;
    assert(rv);
    rv->servers_added = calloc(num_servers, sizeof(char*));
    rv->servers_removed = calloc(num_servers, sizeof(char*));

    /* Compute the added and removed servers */
    compute_vb_list_diff(from, to, rv->servers_added);
    compute_vb_list_diff(to, from, rv->servers_removed);

    /* Verify the servers are equal in their positions */
    if (to->num_servers == from->num_servers) {
        int i;
        rv->sequence_changed = false;
        for (i = 0; i < from->num_servers; i++) {
            rv->sequence_changed |= (0 != strcmp(vbucket_config_get_server(from, i),
                                                 vbucket_config_get_server(to, i)));

        }
    } else {
        /* Just say yes */
        rv->sequence_changed = true;
    }

    /* Consider the sequence changed if the auth credentials changed */
    if (from->user != NULL && to->user != NULL) {
        rv->sequence_changed |= (strcmp(from->user, to->user) != 0);
    } else {
        rv->sequence_changed |= ((from->user != NULL) ^ (to->user != NULL));
    }

    if (from->password != NULL && to->password != NULL) {
        rv->sequence_changed |= (strcmp(from->password, to->password) != 0);
    } else {
        rv->sequence_changed |= ((from->password != NULL) ^ (to->password != NULL));
    }

    /* Count the number of vbucket differences */
    if (to->num_vbuckets == from->num_vbuckets) {
        int i;
        for (i = 0; i < to->num_vbuckets; i++) {
            rv->n_vb_changes += (vbucket_get_master(from, i)
                                 == vbucket_get_master(to, i)) ? 0 : 1;
        }
    } else {
        rv->n_vb_changes = -1;
    }

    return rv;
}

static void free_array_helper(char **l) {
    int i;
    for (i = 0; l[i]; i++) {
        free(l[i]);
    }
    free(l);
}

void vbucket_free_diff(VBUCKET_CONFIG_DIFF *diff) {
    assert(diff);
    free_array_helper(diff->servers_added);
    free_array_helper(diff->servers_removed);
    free(diff);
}
