/*
    SSSD

    Async LDAP Helper routines - initgroups operation

    Copyright (C) Simo Sorce <ssorce@redhat.com> - 2009
    Copyright (C) 2010, Ralf Haferkamp <rhafer@suse.de>, Novell Inc.
    Copyright (C) Jan Zeleny <jzeleny@redhat.com> - 2011

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "util/util.h"
#include "db/sysdb.h"
#include "providers/ldap/sdap_async_private.h"
#include "providers/ldap/ldap_common.h"

/* ==Save-fake-group-list=====================================*/
static errno_t sdap_add_incomplete_groups(struct sysdb_ctx *sysdb,
                                          struct sdap_options *opts,
                                          char **groupnames,
                                          struct sysdb_attrs **ldap_groups,
                                          int ldap_groups_count)
{
    TALLOC_CTX *tmp_ctx;
    struct ldb_message *msg;
    int i, mi, ai;
    const char *name;
    const char *original_dn;
    char **missing;
    gid_t gid;
    int ret;
    bool in_transaction = false;
    bool posix;
    time_t now;

    /* There are no groups in LDAP but we should add user to groups ?? */
    if (ldap_groups_count == 0) return EOK;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    missing = talloc_array(tmp_ctx, char *, ldap_groups_count+1);
    if (!missing) {
        ret = ENOMEM;
        goto fail;
    }
    mi = 0;

    ret = sysdb_transaction_start(sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Cannot start sysdb transaction [%d]: %s\n",
                   ret, strerror(ret)));
        goto fail;
    }
    in_transaction = true;

    for (i=0; groupnames[i]; i++) {
        ret = sysdb_search_group_by_name(tmp_ctx, sysdb, groupnames[i], NULL, &msg);
        if (ret == EOK) {
            continue;
        } else if (ret == ENOENT) {
            DEBUG(7, ("Group #%d [%s] is not cached, need to add a fake entry\n",
                       i, groupnames[i]));
            missing[mi] = groupnames[i];
            mi++;
            continue;
        } else if (ret != ENOENT) {
            DEBUG(1, ("search for group failed [%d]: %s\n",
                      ret, strerror(ret)));
            goto fail;
        }
    }
    missing[mi] = NULL;

    /* All groups are cached, nothing to do */
    if (mi == 0) {
        talloc_zfree(tmp_ctx);
        goto done;
    }

    now = time(NULL);
    for (i=0; missing[i]; i++) {
        /* The group is not in sysdb, need to add a fake entry */
        for (ai=0; ai < ldap_groups_count; ai++) {
            ret = sysdb_attrs_primary_name(sysdb, ldap_groups[ai],
                                           opts->group_map[SDAP_AT_GROUP_NAME].name,
                                           &name);
            if (ret != EOK) {
                DEBUG(1, ("The group has no name attribute\n"));
                goto fail;
            }

            if (strcmp(name, missing[i]) == 0) {
                posix = true;
                ret = sysdb_attrs_get_uint32_t(ldap_groups[ai],
                                               SYSDB_GIDNUM,
                                               &gid);
                if (ret == ENOENT || (ret == EOK && gid == 0)) {
                    DEBUG(9, ("The group %s gid was %s\n",
                              name, ret == ENOENT ? "missing" : "zero"));
                    DEBUG(8, ("Marking group %s as non-posix and setting GID=0!\n", name));
                    gid = 0;
                    posix = false;
                } else if (ret) {
                    DEBUG(1, ("The GID attribute is malformed\n"));
                    goto fail;
                }

                ret = sysdb_attrs_get_string(ldap_groups[ai],
                                             SYSDB_ORIG_DN,
                                             &original_dn);
                if (ret) {
                    DEBUG(5, ("The group has no name original DN\n"));
                    original_dn = NULL;
                }

                DEBUG(8, ("Adding fake group %s to sysdb\n", name));
                ret = sysdb_add_incomplete_group(sysdb, name, gid, original_dn,
                                                 posix, now);
                if (ret != EOK) {
                    goto fail;
                }
                break;
            }
        }

        if (ai == ldap_groups_count) {
            DEBUG(2, ("Group %s not present in LDAP\n", missing[i]));
            ret = EINVAL;
            goto fail;
        }
    }

done:
    ret = sysdb_transaction_commit(sysdb);
    if (ret != EOK) {
        DEBUG(1, ("sysdb_transaction_commit failed.\n"));
        goto fail;
    }
    in_transaction = false;
    ret = EOK;
fail:
    if (in_transaction) {
        sysdb_transaction_cancel(sysdb);
    }
    talloc_free(tmp_ctx);
    return ret;
}

static int sdap_initgr_common_store(struct sysdb_ctx *sysdb,
                                    struct sdap_options *opts,
                                    const char *name,
                                    enum sysdb_member_type type,
                                    char **sysdb_grouplist,
                                    struct sysdb_attrs **ldap_groups,
                                    int ldap_groups_count)
{
    TALLOC_CTX *tmp_ctx;
    char **ldap_grouplist = NULL;
    char **add_groups;
    char **del_groups;
    int ret, tret;
    bool in_transaction = false;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    if (ldap_groups_count == 0) {
        /* No groups for this user in LDAP.
         * We need to ensure that there are no groups
         * in the sysdb either.
         */
        ldap_grouplist = NULL;
    } else {
        ret = sysdb_attrs_primary_name_list(
                sysdb, tmp_ctx,
                ldap_groups, ldap_groups_count,
                opts->group_map[SDAP_AT_GROUP_NAME].name,
                &ldap_grouplist);
        if (ret != EOK) {
            DEBUG(1, ("sysdb_attrs_primary_name_list failed [%d]: %s\n",
                      ret, strerror(ret)));
            goto done;
        }
    }

    /* Find the differences between the sysdb and LDAP lists
     * Groups in the sysdb only must be removed.
     */
    ret = diff_string_lists(tmp_ctx, ldap_grouplist, sysdb_grouplist,
                            &add_groups, &del_groups, NULL);
    if (ret != EOK) goto done;

    ret = sysdb_transaction_start(sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto done;
    }
    in_transaction = true;

    /* Add fake entries for any groups the user should be added as
     * member of but that are not cached in sysdb
     */
    if (add_groups && add_groups[0]) {
        ret = sdap_add_incomplete_groups(sysdb, opts,
                                         add_groups, ldap_groups,
                                         ldap_groups_count);
        if (ret != EOK) {
            DEBUG(1, ("Adding incomplete users failed\n"));
            goto done;
        }
    }

    DEBUG(8, ("Updating memberships for %s\n", name));
    ret = sysdb_update_members(sysdb, name, type,
                               (const char *const *) add_groups,
                               (const char *const *) del_groups);
    if (ret != EOK) {
        DEBUG(1, ("Membership update failed [%d]: %s\n",
                  ret, strerror(ret)));
        goto done;
    }

    ret = sysdb_transaction_commit(sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to commit transaction\n"));
        goto done;
    }
    in_transaction = false;

    ret = EOK;
done:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    talloc_zfree(tmp_ctx);
    return ret;
}

/* ==Initgr-call-(groups-a-user-is-member-of)-RFC2307===================== */

struct sdap_initgr_rfc2307_state {
    struct tevent_context *ev;
    struct sysdb_ctx *sysdb;
    struct sdap_options *opts;
    struct sdap_handle *sh;
    const char **attrs;
    const char *name;
    const char *base_filter;
    const char *orig_dn;
    char *filter;
    int timeout;

    struct sdap_op *op;

    struct sysdb_attrs **ldap_groups;
    size_t ldap_groups_count;

    size_t base_iter;
    struct sdap_search_base **search_bases;
};

static errno_t sdap_initgr_rfc2307_next_base(struct tevent_req *req);
static void sdap_initgr_rfc2307_process(struct tevent_req *subreq);
struct tevent_req *sdap_initgr_rfc2307_send(TALLOC_CTX *memctx,
                                            struct tevent_context *ev,
                                            struct sdap_options *opts,
                                            struct sysdb_ctx *sysdb,
                                            struct sdap_handle *sh,
                                            const char *name)
{
    struct tevent_req *req;
    struct sdap_initgr_rfc2307_state *state;
    char *clean_name;
    errno_t ret;

    req = tevent_req_create(memctx, &state, struct sdap_initgr_rfc2307_state);
    if (!req) return NULL;

    state->ev = ev;
    state->opts = opts;
    state->sysdb = sysdb;
    state->sh = sh;
    state->op = NULL;
    state->timeout = dp_opt_get_int(state->opts->basic, SDAP_SEARCH_TIMEOUT);
    state->ldap_groups = NULL;
    state->ldap_groups_count = 0;
    state->base_iter = 0;
    state->search_bases = opts->group_search_bases;

    if (!state->search_bases) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Initgroups lookup request without a group search base\n"));
        ret = EINVAL;
        goto done;
    }

    state->name = talloc_strdup(state, name);
    if (!state->name) {
        talloc_zfree(req);
        return NULL;
    }

    ret = build_attrs_from_map(state, opts->group_map,
                               SDAP_OPTS_GROUP, &state->attrs);
    if (ret != EOK) {
        talloc_free(req);
        return NULL;
    }

    ret = sss_filter_sanitize(state, name, &clean_name);
    if (ret != EOK) {
        talloc_free(req);
        return NULL;
    }

    state->base_filter = talloc_asprintf(state,
                             "(&(%s=%s)(objectclass=%s)(%s=*)(&(%s=*)(!(%s=0))))",
                             opts->group_map[SDAP_AT_GROUP_MEMBER].name,
                             clean_name,
                             opts->group_map[SDAP_OC_GROUP].name,
                             opts->group_map[SDAP_AT_GROUP_NAME].name,
                             opts->group_map[SDAP_AT_GROUP_GID].name,
                             opts->group_map[SDAP_AT_GROUP_GID].name);
    if (!state->base_filter) {
        talloc_zfree(req);
        return NULL;
    }
    talloc_zfree(clean_name);

    ret = sdap_initgr_rfc2307_next_base(req);

done:
    if (ret != EOK) {
        tevent_req_error(req, ret);
        tevent_req_post(req, ev);
    }

    return req;
}

static errno_t sdap_initgr_rfc2307_next_base(struct tevent_req *req)
{
    struct tevent_req *subreq;
    struct sdap_initgr_rfc2307_state *state;

    state = tevent_req_data(req, struct sdap_initgr_rfc2307_state);

    talloc_zfree(state->filter);

    state->filter = sdap_get_id_specific_filter(
            state, state->base_filter,
            state->search_bases[state->base_iter]->filter);
    if (!state->filter) {
        return ENOMEM;
    }

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Searching for groups with base [%s]\n",
           state->search_bases[state->base_iter]->basedn));

    subreq = sdap_get_generic_send(
            state, state->ev, state->opts, state->sh,
            state->search_bases[state->base_iter]->basedn,
            state->search_bases[state->base_iter]->scope,
            state->filter, state->attrs,
            state->opts->group_map, SDAP_OPTS_GROUP,
            state->timeout,
            true);
    if (!subreq) {
        return ENOMEM;
    }

    tevent_req_set_callback(subreq, sdap_initgr_rfc2307_process, req);

    return EOK;
}

static void sdap_initgr_rfc2307_process(struct tevent_req *subreq)
{
    struct tevent_req *req;
    struct sdap_initgr_rfc2307_state *state;
    struct sysdb_attrs **ldap_groups;
    char **sysdb_grouplist = NULL;
    struct ldb_message *msg;
    struct ldb_message_element *groups;
    size_t count;
    const char *attrs[2];
    int ret;
    int i;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct sdap_initgr_rfc2307_state);

    ret = sdap_get_generic_recv(subreq, state, &count, &ldap_groups);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    /* Add this batch of groups to the list */
    if (count > 0) {
        state->ldap_groups =
                talloc_realloc(state,
                               state->ldap_groups,
                               struct sysdb_attrs *,
                               state->ldap_groups_count + count + 1);
        if (!state->ldap_groups) {
            tevent_req_error(req, ENOMEM);
            return;
        }

        /* Copy the new groups into the list.
         */
        for (i = 0; i < count; i++) {
            state->ldap_groups[state->ldap_groups_count + i] =
                talloc_steal(state->ldap_groups, ldap_groups[i]);
        }

        state->ldap_groups_count += count;

        state->ldap_groups[state->ldap_groups_count] = NULL;
    }

    state->base_iter++;

    /* Check for additional search bases, and iterate
     * through again.
     */
    if (state->search_bases[state->base_iter] != NULL) {
        ret = sdap_initgr_rfc2307_next_base(req);
        if (ret != EOK) {
            tevent_req_error(req, ret);
        }
        return;
    }

    /* Search for all groups for which this user is a member */
    attrs[0] = SYSDB_MEMBEROF;
    attrs[1] = NULL;

    ret = sysdb_search_user_by_name(state, state->sysdb, state->name,
                                    attrs, &msg);
    if (ret != EOK) {
        tevent_req_error(req, ret);
        return;
    }

    groups = ldb_msg_find_element(msg, SYSDB_MEMBEROF);
    if (!groups || groups->num_values == 0) {
        /* No groups for this user in sysdb currently */
        sysdb_grouplist = NULL;
    } else {
        sysdb_grouplist = talloc_array(state, char *, groups->num_values+1);
        if (!sysdb_grouplist) {
            tevent_req_error(req, ENOMEM);
            return;
        }

        /* Get a list of the groups by groupname only */
        for (i=0; i < groups->num_values; i++) {
            ret = sysdb_group_dn_name(state->sysdb,
                                      sysdb_grouplist,
                                      (const char *)groups->values[i].data,
                                      &sysdb_grouplist[i]);
            if (ret != EOK) {
                tevent_req_error(req, ret);
                return;
            }
        }
        sysdb_grouplist[groups->num_values] = NULL;
    }

    /* There are no nested groups here so we can just update the
     * memberships */
    ret = sdap_initgr_common_store(state->sysdb, state->opts,
                                   state->name,
                                   SYSDB_MEMBER_USER,
                                   sysdb_grouplist,
                                   state->ldap_groups,
                                   state->ldap_groups_count);
    if (ret != EOK) {
        tevent_req_error(req, ret);
        return;
    }

    tevent_req_done(req);
}

static int sdap_initgr_rfc2307_recv(struct tevent_req *req)
{
    TEVENT_REQ_RETURN_ON_ERROR(req);

    return EOK;
}

/* ==Common code for pure RFC2307bis and IPA/AD========================= */
static errno_t
sdap_nested_groups_store(struct sysdb_ctx *sysdb,
                         struct sdap_options *opts,
                         struct sysdb_attrs **groups,
                         unsigned long count)
{
    errno_t ret, tret;
    TALLOC_CTX *tmp_ctx;
    char **groupnamelist = NULL;
    bool in_transaction = false;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    if (count > 0) {
        ret = sysdb_attrs_primary_name_list(sysdb, tmp_ctx,
                                            groups, count,
                                            opts->group_map[SDAP_AT_GROUP_NAME].name,
                                            &groupnamelist);
        if (ret != EOK) {
            DEBUG(3, ("sysdb_attrs_primary_name_list failed [%d]: %s\n",
                    ret, strerror(ret)));
            goto done;
        }
    }

    ret = sysdb_transaction_start(sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto done;
    }
    in_transaction = true;

    ret = sdap_add_incomplete_groups(sysdb, opts, groupnamelist,
                                     groups, count);
    if (ret != EOK) {
        DEBUG(6, ("Could not add incomplete groups [%d]: %s\n",
                   ret, strerror(ret)));
        goto done;
    }

    ret = sysdb_transaction_commit(sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to commit transaction\n"));
        goto done;
    }
    in_transaction = false;

    ret = EOK;
done:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }

    talloc_free(tmp_ctx);
    return ret;
}

struct membership_diff {
    struct membership_diff *prev;
    struct membership_diff *next;

    const char *name;
    char **add;
    char **del;
};

static errno_t
build_membership_diff(TALLOC_CTX *mem_ctx, const char *name,
                      char **ldap_parent_names, char **sysdb_parent_names,
                      struct membership_diff **_mdiff)
{
    TALLOC_CTX *tmp_ctx;
    struct membership_diff *mdiff;
    errno_t ret;
    char **add_groups;
    char **del_groups;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    mdiff = talloc_zero(tmp_ctx, struct membership_diff);
    if (!mdiff) {
        ret = ENOMEM;
        goto done;
    }
    mdiff->name = talloc_strdup(mdiff, name);
    if (!mdiff->name) {
        ret = ENOMEM;
        goto done;
    }

    /* Find the differences between the sysdb and ldap lists
     * Groups in ldap only must be added to the sysdb;
     * groups in the sysdb only must be removed.
     */
    ret = diff_string_lists(tmp_ctx,
                            ldap_parent_names, sysdb_parent_names,
                            &add_groups, &del_groups, NULL);
    if (ret != EOK) {
        goto done;
    }
    mdiff->add = talloc_steal(mdiff, add_groups);
    mdiff->del = talloc_steal(mdiff, del_groups);

    ret = EOK;
    *_mdiff = talloc_steal(mem_ctx, mdiff);
done:
    talloc_free(tmp_ctx);
    return ret;
}

/* ==Initgr-call-(groups-a-user-is-member-of)-nested-groups=============== */

struct sdap_initgr_nested_state {
    struct tevent_context *ev;
    struct sysdb_ctx *sysdb;
    struct sdap_options *opts;
    struct sss_domain_info *dom;
    struct sdap_handle *sh;

    struct sysdb_attrs *user;
    const char *username;
    const char *orig_dn;

    const char **grp_attrs;

    struct ldb_message_element *memberof;
    char *filter;
    char **group_dns;
    int cur;

    struct sdap_op *op;

    struct sysdb_attrs **groups;
    int groups_cur;
};

static errno_t sdap_initgr_nested_deref_search(struct tevent_req *req);
static errno_t sdap_initgr_nested_noderef_search(struct tevent_req *req);
static void sdap_initgr_nested_search(struct tevent_req *subreq);
static void sdap_initgr_nested_store(struct tevent_req *req);
static struct tevent_req *sdap_initgr_nested_send(TALLOC_CTX *memctx,
                                                  struct tevent_context *ev,
                                                  struct sdap_options *opts,
                                                  struct sysdb_ctx *sysdb,
                                                  struct sss_domain_info *dom,
                                                  struct sdap_handle *sh,
                                                  struct sysdb_attrs *user,
                                                  const char **grp_attrs)
{
    struct tevent_req *req;
    struct sdap_initgr_nested_state *state;
    errno_t ret;
    int deref_threshold;

    req = tevent_req_create(memctx, &state, struct sdap_initgr_nested_state);
    if (!req) return NULL;

    state->ev = ev;
    state->opts = opts;
    state->sysdb = sysdb;
    state->dom = dom;
    state->sh = sh;
    state->grp_attrs = grp_attrs;
    state->user = user;
    state->op = NULL;

    ret = sysdb_attrs_primary_name(sysdb, user,
                                   opts->user_map[SDAP_AT_USER_NAME].name,
                                   &state->username);
    if (ret != EOK) {
        DEBUG(1, ("User entry had no username\n"));
        goto immediate;
    }

    ret = sysdb_attrs_get_el(state->user, SYSDB_MEMBEROF, &state->memberof);
    if (ret || !state->memberof || state->memberof->num_values == 0) {
        DEBUG(4, ("User entry lacks original memberof ?\n"));
        /* We can't find any groups for this user, so we'll
         * have to assume there aren't any. Just return
         * success here.
         */
        ret = EOK;
        goto immediate;
    }

    state->groups = talloc_zero_array(state, struct sysdb_attrs *,
                                      state->memberof->num_values + 1);;
    if (!state->groups) {
        ret = ENOMEM;
        goto immediate;
    }
    state->groups_cur = 0;

    deref_threshold = dp_opt_get_int(state->opts->basic,
                                     SDAP_DEREF_THRESHOLD);
    if (sdap_has_deref_support(state->sh, state->opts) &&
        deref_threshold < state->memberof->num_values) {
        ret = sysdb_attrs_get_string(user, SYSDB_ORIG_DN,
                                     &state->orig_dn);
        if (ret != EOK) goto immediate;

        ret = sdap_initgr_nested_deref_search(req);
        if (ret != EAGAIN) goto immediate;
    } else {
        ret = sdap_initgr_nested_noderef_search(req);
        if (ret != EAGAIN) goto immediate;
    }

    return req;

immediate:
    if (ret == EOK) {
        tevent_req_done(req);
    } else {
        tevent_req_error(req, ret);
    }
    tevent_req_post(req, ev);
    return req;
}

static errno_t sdap_initgr_nested_noderef_search(struct tevent_req *req)
{
    int i;
    struct tevent_req *subreq;
    struct sdap_initgr_nested_state *state;

    state = tevent_req_data(req, struct sdap_initgr_nested_state);

    state->group_dns = talloc_array(state, char *,
                                    state->memberof->num_values + 1);
    if (!state->group_dns) {
        return ENOMEM;
    }
    for (i = 0; i < state->memberof->num_values; i++) {
        state->group_dns[i] = talloc_strdup(state->group_dns,
                                    (char *)state->memberof->values[i].data);
        if (!state->group_dns[i]) {
            return ENOMEM;
        }
    }
    state->group_dns[i] = NULL; /* terminate */
    state->cur = 0;

    state->filter = talloc_asprintf(state, "(&(objectclass=%s)(%s=*))",
                            state->opts->group_map[SDAP_OC_GROUP].name,
                            state->opts->group_map[SDAP_AT_GROUP_NAME].name);
    if (!state->filter) {
        return ENOMEM;
    }

    subreq = sdap_get_generic_send(state, state->ev, state->opts, state->sh,
                                   state->group_dns[state->cur],
                                   LDAP_SCOPE_BASE,
                                   state->filter, state->grp_attrs,
                                   state->opts->group_map, SDAP_OPTS_GROUP,
                                   dp_opt_get_int(state->opts->basic,
                                                  SDAP_SEARCH_TIMEOUT),
                                   false);
    if (!subreq) {
        return ENOMEM;
    }
    tevent_req_set_callback(subreq, sdap_initgr_nested_search, req);

    return EAGAIN;
}

static void sdap_initgr_nested_deref_done(struct tevent_req *subreq);

static errno_t sdap_initgr_nested_deref_search(struct tevent_req *req)
{
    struct tevent_req *subreq;
    struct sdap_attr_map_info *maps;
    const int num_maps = 1;
    const char **sdap_attrs;
    errno_t ret;
    int timeout;
    struct sdap_initgr_nested_state *state;

    state = tevent_req_data(req, struct sdap_initgr_nested_state);

    maps = talloc_array(state, struct sdap_attr_map_info, num_maps+1);
    if (!maps) return ENOMEM;

    maps[0].map = state->opts->group_map;
    maps[0].num_attrs = SDAP_OPTS_GROUP;
    maps[1].map = NULL;

    ret = build_attrs_from_map(state, state->opts->group_map,
                               SDAP_OPTS_GROUP, &sdap_attrs);
    if (ret != EOK) goto fail;

    timeout = dp_opt_get_int(state->opts->basic, SDAP_SEARCH_TIMEOUT);

    subreq = sdap_deref_search_send(state, state->ev, state->opts,
                    state->sh, state->orig_dn,
                    state->opts->user_map[SDAP_AT_USER_MEMBEROF].name,
                    sdap_attrs, num_maps, maps, timeout);
    if (!subreq) {
        ret = EIO;
        goto fail;
    }
    talloc_steal(subreq, sdap_attrs);
    talloc_steal(subreq, maps);

    tevent_req_set_callback(subreq, sdap_initgr_nested_deref_done, req);
    return EAGAIN;

fail:
    talloc_free(sdap_attrs);
    talloc_free(maps);
    return ret;
}

static void sdap_initgr_nested_deref_done(struct tevent_req *subreq)
{
    errno_t ret;
    struct tevent_req *req;
    struct sdap_initgr_nested_state *state;
    size_t num_results;
    size_t i;
    struct sdap_deref_attrs **deref_result;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct sdap_initgr_nested_state);

    ret = sdap_deref_search_recv(subreq, state,
                                 &num_results,
                                 &deref_result);
    talloc_zfree(subreq);
    if (ret != EOK && ret != ENOENT) {
        tevent_req_error(req, ret);
        return;
    } else if (ret == ENOENT || deref_result == NULL) {
        /* Nothing could be dereferenced. Done. */
        tevent_req_done(req);
        return;
    }

    for (i=0; i < num_results; i++) {
        state->groups[i] = talloc_steal(state->groups,
                                        deref_result[i]->attrs);
    }

    state->groups_cur = num_results;
    sdap_initgr_nested_store(req);
}

static void sdap_initgr_nested_search(struct tevent_req *subreq)
{
    struct tevent_req *req;
    struct sdap_initgr_nested_state *state;
    struct sysdb_attrs **groups;
    size_t count;
    int ret;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct sdap_initgr_nested_state);

    ret = sdap_get_generic_recv(subreq, state, &count, &groups);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    if (count == 1) {
        state->groups[state->groups_cur] = talloc_steal(state->groups,
                                                        groups[0]);
        state->groups_cur++;
    } else {
        DEBUG(2, ("Search for group %s, returned %d results. Skipping\n",
                  state->group_dns[state->cur], count));
    }

    state->cur++;
    /* note that state->memberof->num_values is the count of original
     * memberOf which might not be only groups, but permissions, etc.
     * Use state->groups_cur for group index cap */
    if (state->cur < state->memberof->num_values) {
        subreq = sdap_get_generic_send(state, state->ev,
                                       state->opts, state->sh,
                                       state->group_dns[state->cur],
                                       LDAP_SCOPE_BASE,
                                       state->filter, state->grp_attrs,
                                       state->opts->group_map,
                                       SDAP_OPTS_GROUP,
                                       dp_opt_get_int(state->opts->basic,
                                                      SDAP_SEARCH_TIMEOUT),
                                       false);
        if (!subreq) {
            tevent_req_error(req, ENOMEM);
            return;
        }
        tevent_req_set_callback(subreq, sdap_initgr_nested_search, req);
    } else {
        sdap_initgr_nested_store(req);
    }
}

static errno_t
sdap_initgr_store_groups(struct sdap_initgr_nested_state *state);
static errno_t
sdap_initgr_store_group_memberships(struct sdap_initgr_nested_state *state);
static errno_t
sdap_initgr_store_user_memberships(struct sdap_initgr_nested_state *state);

static void sdap_initgr_nested_store(struct tevent_req *req)
{
    errno_t ret;
    struct sdap_initgr_nested_state *state;
    bool in_transaction = false;
    errno_t tret;

    state = tevent_req_data(req, struct sdap_initgr_nested_state);

    ret = sysdb_transaction_start(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto fail;
    }
    in_transaction = true;

    /* save the groups if they are not already */
    ret = sdap_initgr_store_groups(state);
    if (ret != EOK) {
        DEBUG(3, ("Could not save groups [%d]: %s\n",
                  ret, strerror(ret)));
        goto fail;
    }

    /* save the group memberships */
    ret = sdap_initgr_store_group_memberships(state);
    if (ret != EOK) {
        DEBUG(3, ("Could not save group memberships [%d]: %s\n",
                  ret, strerror(ret)));
        goto fail;
    }

    /* save the user memberships */
    ret = sdap_initgr_store_user_memberships(state);
    if (ret != EOK) {
        DEBUG(3, ("Could not save user memberships [%d]: %s\n",
                  ret, strerror(ret)));
        goto fail;
    }

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to commit transaction\n"));
        goto fail;
    }
    in_transaction = false;

    tevent_req_done(req);
    return;

fail:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(state->sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    tevent_req_error(req, ret);
    return;
}

static errno_t
sdap_initgr_store_groups(struct sdap_initgr_nested_state *state)
{
    return sdap_nested_groups_store(state->sysdb,
                                    state->opts, state->groups,
                                    state->groups_cur);
}

static errno_t
sdap_initgr_nested_get_membership_diff(TALLOC_CTX *mem_ctx,
                                       struct sysdb_ctx *sysdb,
                                       struct sdap_options *opts,
                                       struct sss_domain_info *dom,
                                       struct sysdb_attrs *group,
                                       struct sysdb_attrs **all_groups,
                                       int groups_count,
                                       struct membership_diff **mdiff);

static int sdap_initgr_nested_get_direct_parents(TALLOC_CTX *mem_ctx,
                                                 struct sysdb_attrs *attrs,
                                                 struct sysdb_attrs **groups,
                                                 int ngroups,
                                                 struct sysdb_attrs ***_direct_parents,
                                                 int *_ndirect);

static errno_t
sdap_initgr_store_group_memberships(struct sdap_initgr_nested_state *state)
{
    errno_t ret;
    int i, tret;
    TALLOC_CTX *tmp_ctx;
    struct membership_diff *miter;
    struct membership_diff *memberships = NULL;
    bool in_transaction = false;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    /* Compute the diffs first in order to keep the transaction as small
     * as possible
     */
    for (i=0; i < state->groups_cur; i++) {
        ret = sdap_initgr_nested_get_membership_diff(tmp_ctx, state->sysdb,
                                                     state->opts, state->dom,
                                                     state->groups[i],
                                                     state->groups,
                                                     state->groups_cur,
                                                     &miter);
        if (ret) {
            DEBUG(3, ("Could not compute memberships for group %d [%d]: %s\n",
                      i, ret, strerror(ret)));
            goto done;
        }

        DLIST_ADD(memberships, miter);
    }

    ret = sysdb_transaction_start(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto done;
    }
    in_transaction = true;

    DLIST_FOR_EACH(miter, memberships) {
        ret = sysdb_update_members(state->sysdb, miter->name,
                                   SYSDB_MEMBER_GROUP,
                                   (const char *const *) miter->add,
                                   (const char *const *) miter->del);
        if (ret != EOK) {
            DEBUG(3, ("Failed to update memberships\n"));
            goto done;
        }
    }

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to commit transaction\n"));
        goto done;
    }
    in_transaction = false;

    ret = EOK;
done:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(state->sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t
sdap_initgr_store_user_memberships(struct sdap_initgr_nested_state *state)
{
    errno_t ret;
    int tret;
    const char *orig_dn;

    char **sysdb_parent_name_list = NULL;
    char **ldap_parent_name_list = NULL;

    int nparents;
    struct sysdb_attrs **ldap_parentlist;
    struct ldb_message_element *el;
    int i, mi;
    char **add_groups;
    char **del_groups;
    TALLOC_CTX *tmp_ctx;
    bool in_transaction = false;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    /* Get direct LDAP parents */
    ret = sysdb_attrs_get_string(state->user, SYSDB_ORIG_DN, &orig_dn);
    if (ret != EOK) {
        DEBUG(2, ("The user has no original DN\n"));
        goto done;
    }

    ldap_parentlist = talloc_zero_array(tmp_ctx, struct sysdb_attrs *,
                                        state->groups_cur + 1);
    if (!ldap_parentlist) {
        ret = ENOMEM;
        goto done;
    }
    nparents = 0;

    for (i=0; i < state->groups_cur ; i++) {
        ret = sysdb_attrs_get_el(state->groups[i], SYSDB_MEMBER, &el);
        if (ret) {
            DEBUG(3, ("A group with no members during initgroups?\n"));
            goto done;
        }

        for (mi = 0; mi < el->num_values; mi++) {
            if (strcasecmp((const char *) el->values[mi].data, orig_dn) != 0) {
                continue;
            }

            ldap_parentlist[nparents] = state->groups[i];
            nparents++;
        }
    }

    DEBUG(7, ("The user %s is a direct member of %d LDAP groups\n",
              state->username, nparents));

    if (nparents == 0) {
        ldap_parent_name_list = NULL;
    } else {
        ret = sysdb_attrs_primary_name_list(state->sysdb, tmp_ctx,
                                            ldap_parentlist,
                                            nparents,
                                            state->opts->group_map[SDAP_AT_GROUP_NAME].name,
                                            &ldap_parent_name_list);
        if (ret != EOK) {
            DEBUG(1, ("sysdb_attrs_primary_name_list failed [%d]: %s\n",
                      ret, strerror(ret)));
            goto done;
        }
    }

    ret = sysdb_get_direct_parents(tmp_ctx, state->sysdb, state->dom,
                                   SYSDB_MEMBER_USER,
                                   state->username, &sysdb_parent_name_list);
    if (ret) {
        DEBUG(1, ("Could not get direct sysdb parents for %s: %d [%s]\n",
                   state->username, ret, strerror(ret)));
        goto done;
    }

    ret = diff_string_lists(tmp_ctx,
                            ldap_parent_name_list, sysdb_parent_name_list,
                            &add_groups, &del_groups, NULL);
    if (ret != EOK) {
        goto done;
    }

    ret = sysdb_transaction_start(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto done;
    }
    in_transaction = true;

    DEBUG(8, ("Updating memberships for %s\n", state->username));
    ret = sysdb_update_members(state->sysdb, state->username, SYSDB_MEMBER_USER,
                               (const char *const *) add_groups,
                               (const char *const *) del_groups);
    if (ret != EOK) {
        DEBUG(1, ("Could not update sysdb memberships for %s: %d [%s]\n",
                  state->username, ret, strerror(ret)));
        goto done;
    }

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret != EOK) {
        goto done;
    }
    in_transaction = false;

    ret = EOK;
done:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(state->sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    talloc_zfree(tmp_ctx);
    return ret;
}

static errno_t
sdap_initgr_nested_get_membership_diff(TALLOC_CTX *mem_ctx,
                                       struct sysdb_ctx *sysdb,
                                       struct sdap_options *opts,
                                       struct sss_domain_info *dom,
                                       struct sysdb_attrs *group,
                                       struct sysdb_attrs **all_groups,
                                       int groups_count,
                                       struct membership_diff **_mdiff)
{
    errno_t ret;
    struct membership_diff *mdiff;
    const char *group_name;

    struct sysdb_attrs **ldap_parentlist;
    int parents_count;

    char **ldap_parent_names_list = NULL;
    char **sysdb_parents_names_list = NULL;

    TALLOC_CTX *tmp_ctx;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    /* Get direct sysdb parents */
    ret = sysdb_attrs_primary_name(sysdb, group,
                                   opts->group_map[SDAP_AT_GROUP_NAME].name,
                                   &group_name);
    if (ret != EOK) {
        goto done;
    }

    ret = sysdb_get_direct_parents(tmp_ctx, sysdb, dom,
                                   SYSDB_MEMBER_GROUP,
                                   group_name, &sysdb_parents_names_list);
    if (ret) {
        DEBUG(1, ("Could not get direct sysdb parents for %s: %d [%s]\n",
                   group_name, ret, strerror(ret)));
        goto done;
    }

    /* For each group, filter only parents from full set */
    ret = sdap_initgr_nested_get_direct_parents(tmp_ctx,
                                                group,
                                                all_groups,
                                                groups_count,
                                                &ldap_parentlist,
                                                &parents_count);
    if (ret != EOK) {
        DEBUG(1, ("Cannot get parent groups for %s [%d]: %s\n",
                  group_name, ret, strerror(ret)));
        goto done;
    }
    DEBUG(7, ("The group %s is a direct member of %d LDAP groups\n",
               group_name, parents_count));

    if (parents_count > 0) {
        ret = sysdb_attrs_primary_name_list(sysdb, tmp_ctx,
                                            ldap_parentlist,
                                            parents_count,
                                            opts->group_map[SDAP_AT_GROUP_NAME].name,
                                            &ldap_parent_names_list);
        if (ret != EOK) {
            DEBUG(1, ("sysdb_attrs_primary_name_list failed [%d]: %s\n",
                        ret, strerror(ret)));
            goto done;
        }
    }

    ret = build_membership_diff(tmp_ctx, group_name, ldap_parent_names_list,
                                sysdb_parents_names_list, &mdiff);
    if (ret != EOK) {
        DEBUG(3, ("Could not build membership diff for %s [%d]: %s\n",
                  group_name, ret, strerror(ret)));
        goto done;
    }

    ret = EOK;
    *_mdiff = talloc_steal(mem_ctx, mdiff);
done:
    talloc_free(tmp_ctx);
    return ret;
}

static int sdap_initgr_nested_get_direct_parents(TALLOC_CTX *mem_ctx,
                                                 struct sysdb_attrs *attrs,
                                                 struct sysdb_attrs **groups,
                                                 int ngroups,
                                                 struct sysdb_attrs ***_direct_parents,
                                                 int *_ndirect)
{
    TALLOC_CTX *tmp_ctx;
    struct ldb_message_element *member;
    int i, mi;
    int ret;
    const char *orig_dn;

    int ndirect;
    struct sysdb_attrs **direct_groups;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    direct_groups = talloc_zero_array(tmp_ctx, struct sysdb_attrs *,
                                      ngroups + 1);
    if (!direct_groups) {
        ret = ENOMEM;
        goto done;
    }
    ndirect = 0;

    ret = sysdb_attrs_get_string(attrs, SYSDB_ORIG_DN, &orig_dn);
    if (ret != EOK) {
        DEBUG(3, ("Missing originalDN\n"));
        goto done;
    }
    DEBUG(9, ("Looking up direct parents for group [%s]\n", orig_dn));

    /* FIXME - Filter only parents from full set to avoid searching
     * through all members of huge groups. That requires asking for memberOf
     * with the group LDAP search
     */

    /* Filter only direct parents from the list of all groups */
    for (i=0; i < ngroups; i++) {
        ret = sysdb_attrs_get_el(groups[i], SYSDB_MEMBER, &member);
        if (ret) {
            DEBUG(7, ("A group with no members during initgroups?\n"));
            continue;
        }

        for (mi = 0; mi < member->num_values; mi++) {
            if (strcasecmp((const char *) member->values[mi].data, orig_dn) != 0) {
                continue;
            }

            direct_groups[ndirect] = groups[i];
            ndirect++;
        }
    }
    direct_groups[ndirect] = NULL;

    DEBUG(9, ("The group [%s] has %d direct parents\n", orig_dn, ndirect));

    *_direct_parents = talloc_steal(mem_ctx, direct_groups);
    *_ndirect = ndirect;
    ret = EOK;
done:
    talloc_zfree(tmp_ctx);
    return ret;
}

static int sdap_initgr_nested_recv(struct tevent_req *req)
{
    TEVENT_REQ_RETURN_ON_ERROR(req);

    return EOK;
}

/* ==Initgr-call-(groups-a-user-is-member-of)-RFC2307-BIS================= */
struct sdap_initgr_rfc2307bis_state {
    struct tevent_context *ev;
    struct sysdb_ctx *sysdb;
    struct sdap_options *opts;
    struct sss_domain_info *dom;
    struct sdap_handle *sh;
    const char *name;
    const char *base_filter;
    char *filter;
    const char **attrs;
    const char *orig_dn;

    int timeout;

    size_t base_iter;
    struct sdap_search_base **search_bases;

    struct sdap_op *op;

    hash_table_t *group_hash;
    size_t num_direct_parents;
    struct sysdb_attrs **direct_groups;
};

struct sdap_nested_group {
    struct sysdb_attrs *group;
    struct sysdb_attrs **ldap_parents;
    size_t parents_count;
};

static errno_t sdap_initgr_rfc2307bis_next_base(struct tevent_req *req);
static void sdap_initgr_rfc2307bis_process(struct tevent_req *subreq);
static void sdap_initgr_rfc2307bis_done(struct tevent_req *subreq);
errno_t save_rfc2307bis_user_memberships(
        struct sdap_initgr_rfc2307bis_state *state);
struct tevent_req *rfc2307bis_nested_groups_send(
        TALLOC_CTX *mem_ctx, struct tevent_context *ev,
        struct sdap_options *opts, struct sysdb_ctx *sysdb,
        struct sss_domain_info *dom, struct sdap_handle *sh,
        struct sysdb_attrs **groups, size_t num_groups,
        hash_table_t *group_hash, size_t nesting);
static errno_t rfc2307bis_nested_groups_recv(struct tevent_req *req);

static struct tevent_req *sdap_initgr_rfc2307bis_send(
        TALLOC_CTX *memctx,
        struct tevent_context *ev,
        struct sdap_options *opts,
        struct sysdb_ctx *sysdb,
        struct sss_domain_info *dom,
        struct sdap_handle *sh,
        const char *name,
        const char *orig_dn)
{
    errno_t ret;
    struct tevent_req *req;
    struct sdap_initgr_rfc2307bis_state *state;
    char *clean_orig_dn;

    req = tevent_req_create(memctx, &state, struct sdap_initgr_rfc2307bis_state);
    if (!req) return NULL;

    state->ev = ev;
    state->opts = opts;
    state->sysdb = sysdb;
    state->dom = dom;
    state->sh = sh;
    state->op = NULL;
    state->name = name;
    state->direct_groups = NULL;
    state->num_direct_parents = 0;
    state->timeout = dp_opt_get_int(state->opts->basic, SDAP_SEARCH_TIMEOUT);
    state->base_iter = 0;
    state->search_bases = opts->group_search_bases;
    state->orig_dn = orig_dn;

    if (!state->search_bases) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Initgroups lookup request without a group search base\n"));
        ret = EINVAL;
        goto done;
    }

    ret = sss_hash_create(state, 32, &state->group_hash);
    if (ret != EOK) {
        talloc_free(req);
        return NULL;
    }

    ret = build_attrs_from_map(state, opts->group_map,
                               SDAP_OPTS_GROUP, &state->attrs);
    if (ret != EOK) goto done;

    ret = sss_filter_sanitize(state, orig_dn, &clean_orig_dn);
    if (ret != EOK) goto done;

    state->base_filter =
            talloc_asprintf(state, "(&(%s=%s)(objectclass=%s)(%s=*))",
                            opts->group_map[SDAP_AT_GROUP_MEMBER].name,
                            clean_orig_dn,
                            opts->group_map[SDAP_OC_GROUP].name,
                            opts->group_map[SDAP_AT_GROUP_NAME].name);
    if (!state->base_filter) {
        ret = ENOMEM;
        goto done;
    }
    talloc_zfree(clean_orig_dn);

    ret = sdap_initgr_rfc2307bis_next_base(req);

done:
    if (ret != EOK) {
        tevent_req_error(req, ret);
        tevent_req_post(req, ev);
    }
    return req;
}

static errno_t sdap_initgr_rfc2307bis_next_base(struct tevent_req *req)
{
    struct tevent_req *subreq;
    struct sdap_initgr_rfc2307bis_state *state;

    state = tevent_req_data(req, struct sdap_initgr_rfc2307bis_state);

    talloc_zfree(state->filter);
    state->filter = sdap_get_id_specific_filter(
            state,
            state->base_filter,
            state->search_bases[state->base_iter]->filter);
    if (!state->filter) {
        return ENOMEM;
    }

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Searching for parent groups for user [%s] with base [%s]\n",
           state->orig_dn, state->search_bases[state->base_iter]->basedn));

    subreq = sdap_get_generic_send(
            state, state->ev, state->opts, state->sh,
            state->search_bases[state->base_iter]->basedn,
            state->search_bases[state->base_iter]->scope,
            state->filter, state->attrs,
            state->opts->group_map, SDAP_OPTS_GROUP,
            state->timeout,
            true);
    if (!subreq) {
        talloc_zfree(req);
        return ENOMEM;
    }
    tevent_req_set_callback(subreq, sdap_initgr_rfc2307bis_process, req);

    return EOK;
}

static void sdap_initgr_rfc2307bis_process(struct tevent_req *subreq)
{
    struct tevent_req *req;
    struct sdap_initgr_rfc2307bis_state *state;
    struct sysdb_attrs **ldap_groups;
    size_t count;
    size_t i;
    int ret;

    req = tevent_req_callback_data(subreq, struct tevent_req);
    state = tevent_req_data(req, struct sdap_initgr_rfc2307bis_state);

    ret = sdap_get_generic_recv(subreq, state,
                                &count,
                                &ldap_groups);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    /* Add this batch of groups to the list */
    if (count > 0) {
        state->direct_groups =
                talloc_realloc(state,
                               state->direct_groups,
                               struct sysdb_attrs *,
                               state->num_direct_parents + count + 1);
        if (!state->direct_groups) {
            tevent_req_error(req, ENOMEM);
            return;
        }

        /* Copy the new groups into the list.
         */
        for (i = 0; i < count; i++) {
            state->direct_groups[state->num_direct_parents + i] =
                    talloc_steal(state->direct_groups, ldap_groups[i]);
        }

        state->num_direct_parents += count;

        state->direct_groups[state->num_direct_parents] = NULL;
    }

    state->base_iter++;

    /* Check for additional search bases, and iterate
     * through again.
     */
    if (state->search_bases[state->base_iter] != NULL) {
        ret = sdap_initgr_rfc2307bis_next_base(req);
        if (ret != EOK) {
            tevent_req_error(req, ret);
        }
        return;
    }

    if (state->num_direct_parents == 0) {
        /* Start a transaction to look up the groups in the sysdb
         * and update them with LDAP data
         */
        ret = save_rfc2307bis_user_memberships(state);
        if (ret != EOK) {
            tevent_req_error(req, ret);
        } else {
            tevent_req_done(req);
        }
        return;
    }

    subreq = rfc2307bis_nested_groups_send(state, state->ev, state->opts,
                                           state->sysdb, state->dom,
                                           state->sh, state->direct_groups,
                                           state->num_direct_parents,
                                           state->group_hash, 0);
    if (!subreq) {
        tevent_req_error(req, EIO);
        return;
    }
    tevent_req_set_callback(subreq, sdap_initgr_rfc2307bis_done, req);
}

static errno_t
save_rfc2307bis_groups(struct sdap_initgr_rfc2307bis_state *state);
static errno_t
save_rfc2307bis_group_memberships(struct sdap_initgr_rfc2307bis_state *state);

static void sdap_initgr_rfc2307bis_done(struct tevent_req *subreq)
{
    errno_t ret;
    struct tevent_req *req =
            tevent_req_callback_data(subreq, struct tevent_req);
    struct sdap_initgr_rfc2307bis_state *state =
            tevent_req_data(req, struct sdap_initgr_rfc2307bis_state);
    bool in_transaction = false;
    errno_t tret;

    ret = rfc2307bis_nested_groups_recv(subreq);
    talloc_zfree(subreq);
    if (ret != EOK) {
        tevent_req_error(req, ret);
        return;
    }

    ret = sysdb_transaction_start(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto fail;
    }
    in_transaction = true;

    /* save the groups if they are not cached */
    ret = save_rfc2307bis_groups(state);
    if (ret != EOK) {
        DEBUG(3, ("Could not save groups memberships [%d]", ret));
        goto fail;
    }

    /* save the group membership */
    ret = save_rfc2307bis_group_memberships(state);
    if (ret != EOK) {
        DEBUG(3, ("Could not save group memberships [%d]", ret));
        goto fail;
    }

    /* save the user memberships */
    ret = save_rfc2307bis_user_memberships(state);
    if (ret != EOK) {
        DEBUG(3, ("Could not save user memberships [%d]", ret));
        goto fail;
    }

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to commit transaction\n"));
        goto fail;
    }
    in_transaction = false;

    tevent_req_done(req);
    return;

fail:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(state->sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    tevent_req_error(req, ret);
    return;
}

static int sdap_initgr_rfc2307bis_recv(struct tevent_req *req)
{
    TEVENT_REQ_RETURN_ON_ERROR(req);
    return EOK;
}

struct rfc2307bis_group_memberships_state {
    struct sysdb_ctx *sysdb;
    struct sdap_options *opts;
    struct sss_domain_info *dom;

    hash_table_t *group_hash;

    struct membership_diff *memberships;

    int ret;
};

static errno_t
save_rfc2307bis_groups(struct sdap_initgr_rfc2307bis_state *state)
{
    struct sysdb_attrs **groups = NULL;
    unsigned long count;
    hash_value_t *values;
    int hret, i;
    errno_t ret;
    TALLOC_CTX *tmp_ctx;
    struct sdap_nested_group *gr;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    hret = hash_values(state->group_hash, &count, &values);
    if (hret != HASH_SUCCESS) {
        ret = EIO;
        goto done;
    }

    groups = talloc_array(tmp_ctx, struct sysdb_attrs *, count);
    if (!groups) {
        ret = ENOMEM;
        goto done;
    }

    for (i = 0; i < count; i++) {
        gr = talloc_get_type(values[i].ptr,
                             struct sdap_nested_group);
        groups[i] = gr->group;
    }
    talloc_zfree(values);

    ret = sdap_nested_groups_store(state->sysdb, state->opts,
                                   groups, count);
    if (ret != EOK) {
        DEBUG(3, ("Could not save groups [%d]: %s\n",
                  ret, strerror(ret)));
        goto done;
    }

    ret = EOK;
done:
    talloc_free(tmp_ctx);
    return ret;
}

static bool rfc2307bis_group_memberships_build(hash_entry_t *item, void *user_data);

static errno_t
save_rfc2307bis_group_memberships(struct sdap_initgr_rfc2307bis_state *state)
{
    errno_t ret, tret;
    int hret;
    TALLOC_CTX *tmp_ctx;
    struct rfc2307bis_group_memberships_state *membership_state;
    struct membership_diff *iter;
    bool in_transaction = false;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) return ENOMEM;

    membership_state = talloc_zero(tmp_ctx,
                                struct rfc2307bis_group_memberships_state);
    if (!membership_state) {
        ret = ENOMEM;
        goto done;
    }

    membership_state->sysdb = state->sysdb;
    membership_state->dom = state->dom;
    membership_state->opts = state->opts;
    membership_state->group_hash = state->group_hash;

    hret = hash_iterate(state->group_hash,
                        rfc2307bis_group_memberships_build,
                        membership_state);
    if (hret != HASH_SUCCESS) {
        ret = membership_state->ret;
        goto done;
    }

    ret = sysdb_transaction_start(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to start transaction\n"));
        goto done;
    }
    in_transaction = true;

    DLIST_FOR_EACH(iter, membership_state->memberships) {
        ret = sysdb_update_members(state->sysdb, iter->name,
                                   SYSDB_MEMBER_GROUP,
                                  (const char *const *) iter->add,
                                  (const char *const *) iter->del);
        if (ret != EOK) {
            DEBUG(3, ("Failed to update memberships\n"));
            goto done;
        }
    }

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret != EOK) {
        DEBUG(1, ("Failed to commit transaction\n"));
        goto done;
    }
    in_transaction = false;

    ret = EOK;
done:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(state->sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    talloc_free(tmp_ctx);
    return ret;
}

static bool
rfc2307bis_group_memberships_build(hash_entry_t *item, void *user_data)
{
    struct rfc2307bis_group_memberships_state *mstate = talloc_get_type(
                        user_data, struct rfc2307bis_group_memberships_state);
    struct sdap_nested_group *group;
    char *group_name;
    TALLOC_CTX *tmp_ctx;
    errno_t ret;
    char **sysdb_parents_names_list;
    char **ldap_parents_names_list = NULL;

    struct membership_diff *mdiff;

    group_name = (char *) item->key.str;
    group = (struct sdap_nested_group *) item->value.ptr;

    tmp_ctx = talloc_new(NULL);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_get_direct_parents(tmp_ctx, mstate->sysdb, mstate->dom,
                                   SYSDB_MEMBER_GROUP,
                                   group_name, &sysdb_parents_names_list);
    if (ret) {
        DEBUG(1, ("Could not get direct sysdb parents for %s: %d [%s]\n",
                  group_name, ret, strerror(ret)));
        goto done;
    }

    if (group->parents_count > 0) {
        ret = sysdb_attrs_primary_name_list(mstate->sysdb, tmp_ctx,
                            group->ldap_parents, group->parents_count,
                            mstate->opts->group_map[SDAP_AT_GROUP_NAME].name,
                            &ldap_parents_names_list);
        if (ret != EOK) {
            goto done;
        }
    }

    ret = build_membership_diff(tmp_ctx, group_name, ldap_parents_names_list,
                                sysdb_parents_names_list, &mdiff);
    if (ret != EOK) {
        DEBUG(3, ("Could not build membership diff for %s [%d]: %s\n",
                  group_name, ret, strerror(ret)));
        goto done;
    }

    talloc_steal(mstate, mdiff);
    DLIST_ADD(mstate->memberships, mdiff);
    ret = EOK;
done:
    talloc_free(tmp_ctx);
    mstate->ret = ret;
    return ret == EOK ? true : false;
}

errno_t save_rfc2307bis_user_memberships(
        struct sdap_initgr_rfc2307bis_state *state)
{
    errno_t ret, tret;
    char **ldap_grouplist;
    char **sysdb_parent_name_list;
    char **add_groups;
    char **del_groups;
    bool in_transaction = false;

    TALLOC_CTX *tmp_ctx = talloc_new(NULL);
    if(!tmp_ctx) {
        return ENOMEM;
    }

    DEBUG(7, ("Save parent groups to sysdb\n"));
    ret = sysdb_transaction_start(state->sysdb);
    if (ret != EOK) {
        goto error;
    }
    in_transaction = true;

    ret = sysdb_get_direct_parents(tmp_ctx, state->sysdb, state->dom,
                                   SYSDB_MEMBER_USER,
                                   state->name, &sysdb_parent_name_list);
    if (ret) {
        DEBUG(1, ("Could not get direct sysdb parents for %s: %d [%s]\n",
                   state->name, ret, strerror(ret)));
        goto error;
    }

    if (state->num_direct_parents == 0) {
        ldap_grouplist = NULL;
    }
    else {
        ret = sysdb_attrs_primary_name_list(
                state->sysdb, tmp_ctx,
                state->direct_groups, state->num_direct_parents,
                state->opts->group_map[SDAP_AT_GROUP_NAME].name,
                &ldap_grouplist);
        if (ret != EOK) {
            goto error;
        }
    }

    /* Find the differences between the sysdb and ldap lists
     * Groups in ldap only must be added to the sysdb;
     * groups in the sysdb only must be removed.
     */
    ret = diff_string_lists(tmp_ctx,
                            ldap_grouplist, sysdb_parent_name_list,
                            &add_groups, &del_groups, NULL);
    if (ret != EOK) {
        goto error;
    }

    DEBUG(8, ("Updating memberships for %s\n", state->name));
    ret = sysdb_update_members(state->sysdb, state->name, SYSDB_MEMBER_USER,
                               (const char *const *)add_groups,
                               (const char *const *)del_groups);
    if (ret != EOK) {
        goto error;
    }

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret != EOK) {
        goto error;
    }
    in_transaction = false;

    talloc_free(tmp_ctx);
    return EOK;

error:
    if (in_transaction) {
        tret = sysdb_transaction_cancel(state->sysdb);
        if (tret != EOK) {
            DEBUG(1, ("Failed to cancel transaction\n"));
        }
    }
    talloc_free(tmp_ctx);
    return ret;
}

struct sdap_rfc2307bis_nested_ctx {
    struct tevent_context *ev;
    struct sdap_options *opts;
    struct sysdb_ctx *sysdb;
    struct sss_domain_info *dom;
    struct sdap_handle *sh;
    int timeout;
    const char *base_filter;
    char *filter;
    const char *orig_dn;
    const char **attrs;
    struct sysdb_attrs **groups;
    size_t num_groups;

    size_t nesting_level;

    size_t group_iter;
    struct sysdb_attrs **ldap_parents;
    size_t parents_count;

    hash_table_t *group_hash;
    const char *primary_name;

    struct sysdb_handle *handle;

    size_t base_iter;
    struct sdap_search_base **search_bases;
};

static errno_t rfc2307bis_nested_groups_step(struct tevent_req *req);
struct tevent_req *rfc2307bis_nested_groups_send(
        TALLOC_CTX *mem_ctx, struct tevent_context *ev,
        struct sdap_options *opts, struct sysdb_ctx *sysdb,
        struct sss_domain_info *dom, struct sdap_handle *sh,
        struct sysdb_attrs **groups, size_t num_groups,
        hash_table_t *group_hash, size_t nesting)
{
    errno_t ret;
    struct tevent_req *req;
    struct sdap_rfc2307bis_nested_ctx *state;

    req = tevent_req_create(mem_ctx, &state,
                            struct sdap_rfc2307bis_nested_ctx);
    if (!req) return NULL;

    if ((num_groups == 0) ||
        (nesting > dp_opt_get_int(opts->basic, SDAP_NESTING_LEVEL))) {
        /* No parent groups to process or too deep*/
        tevent_req_done(req);
        tevent_req_post(req, ev);
        return req;
    }

    state->ev = ev;
    state->opts = opts;
    state->sysdb = sysdb;
    state->dom = dom;
    state->sh = sh;
    state->groups = groups;
    state->num_groups = num_groups;
    state->group_iter = 0;
    state->nesting_level = nesting;
    state->group_hash = group_hash;
    state->filter = NULL;
    state->timeout = dp_opt_get_int(state->opts->basic,
                                    SDAP_SEARCH_TIMEOUT);
    state->base_iter = 0;
    state->search_bases = opts->group_search_bases;
    if (!state->search_bases) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Initgroups nested lookup request "
               "without a group search base\n"));
        ret = EINVAL;
        goto done;
    }

    ret = rfc2307bis_nested_groups_step(req);

done:
    if (ret == EOK) {
        /* All parent groups were already processed */
        tevent_req_done(req);
        tevent_req_post(req, ev);
    } else if (ret != EAGAIN) {
        tevent_req_error(req, ret);
        tevent_req_post(req, ev);
    }

    /* EAGAIN means a lookup is in progress */
    return req;
}

static errno_t rfc2307bis_nested_groups_next_base(struct tevent_req *req);
static void rfc2307bis_nested_groups_process(struct tevent_req *subreq);
static errno_t rfc2307bis_nested_groups_step(struct tevent_req *req)
{
    errno_t ret;
    TALLOC_CTX *tmp_ctx = NULL;
    char *clean_orig_dn;
    hash_key_t key;
    struct sdap_rfc2307bis_nested_ctx *state =
            tevent_req_data(req, struct sdap_rfc2307bis_nested_ctx);

    tmp_ctx = talloc_new(state);
    if (!tmp_ctx) {
        ret = ENOMEM;
        goto done;
    }

    ret = sysdb_attrs_primary_name(
            state->sysdb,
            state->groups[state->group_iter],
            state->opts->group_map[SDAP_AT_GROUP_NAME].name,
            &state->primary_name);
    if (ret != EOK) {
        goto done;
    }

    key.type = HASH_KEY_STRING;
    key.str = talloc_strdup(state, state->primary_name);
    if (!key.str) {
        ret = ENOMEM;
        goto done;
    }

    DEBUG(6, ("Processing group [%s]\n", state->primary_name));

    if (hash_has_key(state->group_hash, &key)) {
        talloc_free(key.str);
        ret = EOK;
        goto done;
    }

    /* Get any parent groups for this group */
    ret = sysdb_attrs_get_string(state->groups[state->group_iter],
                                 SYSDB_ORIG_DN,
                                 &state->orig_dn);
    if (ret != EOK) {
        goto done;
    }

    ret = build_attrs_from_map(state, state->opts->group_map,
                               SDAP_OPTS_GROUP, &state->attrs);
    if (ret != EOK) {
        goto done;
    }

    ret = sss_filter_sanitize(tmp_ctx, state->orig_dn, &clean_orig_dn);
    if (ret != EOK) {
        goto done;
    }

    state->base_filter = talloc_asprintf(
            state, "(&(%s=%s)(objectclass=%s)(%s=*))",
            state->opts->group_map[SDAP_AT_GROUP_MEMBER].name,
            clean_orig_dn,
            state->opts->group_map[SDAP_OC_GROUP].name,
            state->opts->group_map[SDAP_AT_GROUP_NAME].name);
    if (!state->base_filter) {
        ret = ENOMEM;
        goto done;
    }

    ret = rfc2307bis_nested_groups_next_base(req);
    if (ret != EOK) goto done;

    /* Still processing parent groups */
    ret = EAGAIN;

done:
    talloc_free(tmp_ctx);
    return ret;
}

static errno_t rfc2307bis_nested_groups_next_base(struct tevent_req *req)
{
    struct tevent_req *subreq;
    struct sdap_rfc2307bis_nested_ctx *state;

    state = tevent_req_data(req, struct sdap_rfc2307bis_nested_ctx);

    talloc_zfree(state->filter);
    state->filter = sdap_get_id_specific_filter(
            state, state->base_filter,
            state->search_bases[state->base_iter]->filter);
    if (!state->filter) {
        return ENOMEM;
    }

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Searching for parent groups of group [%s] with base [%s]\n",
           state->orig_dn,
           state->search_bases[state->base_iter]->basedn));

    subreq = sdap_get_generic_send(
            state, state->ev, state->opts, state->sh,
            state->search_bases[state->base_iter]->basedn,
            state->search_bases[state->base_iter]->scope,
            state->filter, state->attrs,
            state->opts->group_map, SDAP_OPTS_GROUP,
            state->timeout,
            true);
    if (!subreq) {
        return ENOMEM;
    }
    tevent_req_set_callback(subreq,
                            rfc2307bis_nested_groups_process,
                            req);

    return EOK;
}


static void rfc2307bis_nested_groups_done(struct tevent_req *subreq);
static void rfc2307bis_nested_groups_process(struct tevent_req *subreq)
{
    errno_t ret;
    struct tevent_req *req =
            tevent_req_callback_data(subreq, struct tevent_req);
    struct sdap_rfc2307bis_nested_ctx *state =
            tevent_req_data(req, struct sdap_rfc2307bis_nested_ctx);
    size_t count;
    size_t i;
    struct sysdb_attrs **ldap_groups;
    struct sdap_nested_group *ngr;
    hash_value_t value;
    hash_key_t key;
    int hret;

    ret = sdap_get_generic_recv(subreq, state,
                                &count,
                                &ldap_groups);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    /* Add this batch of groups to the list */
    if (count > 0) {
        state->ldap_parents =
                talloc_realloc(state,
                               state->ldap_parents,
                               struct sysdb_attrs *,
                               state->parents_count + count + 1);
        if (!state->ldap_parents) {
            tevent_req_error(req, ret);
            return;
        }

        /* Copy the new groups into the list.
         * They're allocated on 'state' so we need to move them
         * onto ldap_parents so that the data won't disappear when
         * we finish this nesting level.
         */
        for (i = 0; i < count; i++) {
            state->ldap_parents[state->parents_count + i] =
                talloc_steal(state->ldap_parents, ldap_groups[i]);
        }

        state->parents_count += count;

        state->ldap_parents[state->parents_count] = NULL;
    }

    state->base_iter++;

    /* Check for additional search bases, and iterate
     * through again.
     */
    if (state->search_bases[state->base_iter] != NULL) {
        ret = rfc2307bis_nested_groups_next_base(req);
        if (ret != EOK) {
            tevent_req_error(req, ret);
        }
        return;
    }

    /* Reset the base iterator for future lookups */
    state->base_iter = 0;

    ngr = talloc_zero(state->group_hash, struct sdap_nested_group);
    if (!ngr) {
        tevent_req_error(req, ENOMEM);
        return;
    }

    ngr->group = talloc_steal(ngr, state->groups[state->group_iter]);
    ngr->ldap_parents = talloc_steal(ngr, state->ldap_parents);
    ngr->parents_count = state->parents_count;

    key.type = HASH_KEY_STRING;
    key.str = talloc_strdup(state, state->primary_name);
    if (!key.str) {
        tevent_req_error(req, ENOMEM);
        return;
    }

    value.type = HASH_VALUE_PTR;
    value.ptr = ngr;

    hret = hash_enter(state->group_hash, &key, &value);
    if (hret != HASH_SUCCESS) {
        talloc_free(key.str);
        tevent_req_error(req, EIO);
        return;
    }
    talloc_free(key.str);

    if (state->parents_count == 0) {
        /* No parent groups for this group in LDAP
         * Move on to the next group
         */
        state->group_iter++;
        while (state->group_iter < state->num_groups) {
            ret = rfc2307bis_nested_groups_step(req);
            if (ret == EAGAIN) {
                /* Looking up parent groups.. */
                return;
            } else if (ret != EOK) {
                tevent_req_error(req, ret);
                return;
            }

            /* EOK means this group has already been processed
             * in another nesting level */
            state->group_iter++;
        }

        if (state->group_iter == state->num_groups) {
            /* All groups processed. Done. */
            tevent_req_done(req);
        }
        return;
    }

    /* Otherwise, recurse into the groups */
    subreq = rfc2307bis_nested_groups_send(
            state, state->ev, state->opts, state->sysdb,
            state->dom, state->sh,
            state->ldap_parents,
            state->parents_count,
            state->group_hash,
            state->nesting_level+1);
    if (!subreq) {
        tevent_req_error(req, EIO);
        return;
    }
    tevent_req_set_callback(subreq, rfc2307bis_nested_groups_done, req);
}

static errno_t rfc2307bis_nested_groups_recv(struct tevent_req *req)
{
    TEVENT_REQ_RETURN_ON_ERROR(req);
    return EOK;
}

static void rfc2307bis_nested_groups_done(struct tevent_req *subreq)
{
    errno_t ret;
    struct tevent_req *req =
            tevent_req_callback_data(subreq, struct tevent_req);
    struct sdap_rfc2307bis_nested_ctx *state =
            tevent_req_data(req, struct sdap_rfc2307bis_nested_ctx);

    ret = rfc2307bis_nested_groups_recv(subreq);
    talloc_zfree(subreq);
    if (ret != EOK) {
        DEBUG(6, ("rfc2307bis_nested failed [%d][%s]\n",
                  ret, strerror(ret)));
        tevent_req_error(req, ret);
        return;
    }

    state->group_iter++;
    while (state->group_iter < state->num_groups) {
        ret = rfc2307bis_nested_groups_step(req);
        if (ret == EAGAIN) {
            /* Looking up parent groups.. */
            return;
        } else if (ret != EOK) {
            tevent_req_error(req, ret);
            return;
        }

        /* EOK means this group has already been processed
         * in another nesting level */
        state->group_iter++;
    }

    if (state->group_iter == state->num_groups) {
        /* All groups processed. Done. */
        tevent_req_done(req);
        return;
    }
}

/* ==Initgr-call-(groups-a-user-is-member-of)============================= */

struct sdap_get_initgr_state {
    struct tevent_context *ev;
    struct sysdb_ctx *sysdb;
    struct sdap_options *opts;
    struct sss_domain_info *dom;
    struct sdap_handle *sh;
    struct sdap_id_ctx *id_ctx;
    const char *name;
    const char **grp_attrs;
    const char **user_attrs;
    const char *user_base_filter;
    char *filter;
    int timeout;

    struct sysdb_attrs *orig_user;

    size_t user_base_iter;
    struct sdap_search_base **user_search_bases;
};

static errno_t sdap_get_initgr_next_base(struct tevent_req *req);
static void sdap_get_initgr_user(struct tevent_req *subreq);
static void sdap_get_initgr_done(struct tevent_req *subreq);

struct tevent_req *sdap_get_initgr_send(TALLOC_CTX *memctx,
                                        struct tevent_context *ev,
                                        struct sdap_handle *sh,
                                        struct sdap_id_ctx *id_ctx,
                                        const char *name,
                                        const char **grp_attrs)
{
    struct tevent_req *req;
    struct sdap_get_initgr_state *state;
    int ret;
    char *clean_name;

    DEBUG(9, ("Retrieving info for initgroups call\n"));

    req = tevent_req_create(memctx, &state, struct sdap_get_initgr_state);
    if (!req) return NULL;

    state->ev = ev;
    state->opts = id_ctx->opts;
    state->sysdb = id_ctx->be->sysdb;
    state->dom = id_ctx->be->domain;
    state->sh = sh;
    state->id_ctx = id_ctx;
    state->name = name;
    state->grp_attrs = grp_attrs;
    state->orig_user = NULL;
    state->timeout = dp_opt_get_int(state->opts->basic, SDAP_SEARCH_TIMEOUT);
    state->user_base_iter = 0;
    state->user_search_bases = id_ctx->opts->user_search_bases;
    if (!state->user_search_bases) {
        DEBUG(SSSDBG_CRIT_FAILURE,
              ("Initgroups lookup request without a user search base\n"));
        ret = EINVAL;
        goto done;
    }

    ret = sss_filter_sanitize(state, name, &clean_name);
    if (ret != EOK) {
        talloc_zfree(req);
        return NULL;
    }

    state->user_base_filter =
            talloc_asprintf(state, "(&(%s=%s)(objectclass=%s))",
                            state->opts->user_map[SDAP_AT_USER_NAME].name,
                            clean_name,
                            state->opts->user_map[SDAP_OC_USER].name);
    if (!state->user_base_filter) {
        talloc_zfree(req);
        return NULL;
    }

    ret = build_attrs_from_map(state, state->opts->user_map,
                               SDAP_OPTS_USER, &state->user_attrs);
    if (ret) {
        talloc_zfree(req);
        return NULL;
    }

    ret = sdap_get_initgr_next_base(req);

done:
    if (ret != EOK) {
        tevent_req_error(req, ret);
        tevent_req_post(req, ev);
    }

    return req;
}

static errno_t sdap_get_initgr_next_base(struct tevent_req *req)
{
    struct tevent_req *subreq;
    struct sdap_get_initgr_state *state;

    state = tevent_req_data(req, struct sdap_get_initgr_state);

    talloc_zfree(state->filter);
    state->filter = sdap_get_id_specific_filter(
            state,
            state->user_base_filter,
            state->user_search_bases[state->user_base_iter]->filter);
    if (!state->filter) {
        return ENOMEM;
    }

    DEBUG(SSSDBG_TRACE_FUNC,
          ("Searching for users with base [%s]\n",
           state->user_search_bases[state->user_base_iter]->basedn));

    subreq = sdap_get_generic_send(
            state, state->ev, state->opts, state->sh,
            state->user_search_bases[state->user_base_iter]->basedn,
            state->user_search_bases[state->user_base_iter]->scope,
            state->filter, state->user_attrs,
            state->opts->user_map, SDAP_OPTS_USER,
            state->timeout,
            false);
    if (!subreq) {
        return ENOMEM;
    }
    tevent_req_set_callback(subreq, sdap_get_initgr_user, req);
    return EOK;
}

static struct tevent_req *sdap_initgr_rfc2307bis_send(
        TALLOC_CTX *memctx,
        struct tevent_context *ev,
        struct sdap_options *opts,
        struct sysdb_ctx *sysdb,
        struct sss_domain_info *dom,
        struct sdap_handle *sh,
        const char *name,
        const char *orig_dn);
static void sdap_get_initgr_user(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct sdap_get_initgr_state *state = tevent_req_data(req,
                                               struct sdap_get_initgr_state);
    struct sysdb_attrs **usr_attrs;
    size_t count;
    int ret;
    const char *orig_dn;
    const char *cname;

    DEBUG(9, ("Receiving info for the user\n"));

    ret = sdap_get_generic_recv(subreq, state, &count, &usr_attrs);
    talloc_zfree(subreq);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    if (count == 0) {
        /* No users found in this search */
        state->user_base_iter++;
        if (state->user_search_bases[state->user_base_iter]) {
            /* There are more search bases to try */
            ret = sdap_get_initgr_next_base(req);
            if (ret != EOK) {
                tevent_req_error(req, ret);
            }
            return;
        }

        tevent_req_error(req, ENOENT);
        return;
    } else if (count != 1) {
        DEBUG(2, ("Expected one user entry and got %d\n", count));
        tevent_req_error(req, EINVAL);
        return;
    }

    state->orig_user = usr_attrs[0];

    ret = sysdb_transaction_start(state->sysdb);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    DEBUG(9, ("Storing the user\n"));

    ret = sdap_save_user(state, state->sysdb,
                         state->opts, state->dom,
                         state->orig_user,
                         true, NULL, 0);
    if (ret) {
        sysdb_transaction_cancel(state->sysdb);
        tevent_req_error(req, ret);
        return;
    }

    DEBUG(9, ("Commit change\n"));

    ret = sysdb_transaction_commit(state->sysdb);
    if (ret) {
        tevent_req_error(req, ret);
        return;
    }

    ret = sysdb_get_real_name(state, state->sysdb, state->name, &cname);
    if (ret != EOK) {
        DEBUG(SSSDBG_OP_FAILURE, ("Cannot canonicalize username\n"));
        tevent_req_error(req, ret);
        return;
    }

    DEBUG(9, ("Process user's groups\n"));

    switch (state->opts->schema_type) {
    case SDAP_SCHEMA_RFC2307:
        ret = sdap_check_aliases(state->sysdb, state->orig_user, state->dom,
                                 state->opts, false);
        if (ret != EOK) {
            tevent_req_error(req, ret);
            return;
        }

        subreq = sdap_initgr_rfc2307_send(state, state->ev, state->opts,
                                          state->sysdb, state->sh,
                                          cname);
        if (!subreq) {
            tevent_req_error(req, ENOMEM);
            return;
        }
        tevent_req_set_callback(subreq, sdap_get_initgr_done, req);
        break;

    case SDAP_SCHEMA_RFC2307BIS:
        ret = sysdb_attrs_get_string(state->orig_user,
                                     SYSDB_ORIG_DN,
                                     &orig_dn);
        if (ret != EOK) {
            tevent_req_error(req, ret);
            return;
        }

        subreq = sdap_initgr_rfc2307bis_send(
                state, state->ev, state->opts, state->sysdb,
                state->dom, state->sh,
                cname, orig_dn);
        if (!subreq) {
            tevent_req_error(req, ENOMEM);
            return;
        }
        talloc_steal(subreq, orig_dn);
        tevent_req_set_callback(subreq, sdap_get_initgr_done, req);
        break;
    case SDAP_SCHEMA_IPA_V1:
    case SDAP_SCHEMA_AD:
        /* TODO: AD uses a different member/memberof schema
         *       We need an AD specific call that is able to unroll
         *       nested groups by doing extensive recursive searches */

        subreq = sdap_initgr_nested_send(state, state->ev, state->opts,
                                         state->sysdb, state->dom, state->sh,
                                         state->orig_user, state->grp_attrs);
        if (!subreq) {
            tevent_req_error(req, ENOMEM);
            return;
        }
        tevent_req_set_callback(subreq, sdap_get_initgr_done, req);
        return;

    default:
        tevent_req_error(req, EINVAL);
        return;
    }
}

static int sdap_initgr_rfc2307bis_recv(struct tevent_req *req);
static void sdap_get_initgr_pgid(struct tevent_req *req);
static void sdap_get_initgr_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct sdap_get_initgr_state *state = tevent_req_data(req,
                                               struct sdap_get_initgr_state);
    int ret;
    gid_t primary_gid;
    char *gid;

    DEBUG(9, ("Initgroups done\n"));

    switch (state->opts->schema_type) {
    case SDAP_SCHEMA_RFC2307:
        ret = sdap_initgr_rfc2307_recv(subreq);
        break;

    case SDAP_SCHEMA_RFC2307BIS:
        ret = sdap_initgr_rfc2307bis_recv(subreq);
        break;

    case SDAP_SCHEMA_IPA_V1:
    case SDAP_SCHEMA_AD:
        ret = sdap_initgr_nested_recv(subreq);
        break;

    default:

        ret = EINVAL;
        break;
    }

    talloc_zfree(subreq);
    if (ret) {
        DEBUG(9, ("Error in initgroups: [%d][%s]\n",
                  ret, strerror(ret)));
        tevent_req_error(req, ret);
        return;
    }

    /* We also need to update the user's primary group, since
     * the user may not be an explicit member of that group
     */
    ret = sysdb_attrs_get_uint32_t(state->orig_user, SYSDB_GIDNUM, &primary_gid);
    if (ret != EOK) {
        DEBUG(6, ("Could not find user's primary GID\n"));
        tevent_req_error(req, ret);
        return;
    }

    gid = talloc_asprintf(state, "%lu", (unsigned long)primary_gid);
    if (gid == NULL) {
        tevent_req_error(req, ENOMEM);
        return;
    }

    subreq = groups_get_send(req, state->ev, state->id_ctx, gid,
                             BE_FILTER_IDNUM, BE_ATTR_ALL);
    if (!subreq) {
        tevent_req_error(req, ENOMEM);
        return;
    }
    tevent_req_set_callback(subreq, sdap_get_initgr_pgid, req);

    tevent_req_done(req);
}

static void sdap_get_initgr_pgid(struct tevent_req *subreq)
{
    struct tevent_req *req =
            tevent_req_callback_data(subreq, struct tevent_req);
    errno_t ret;

    ret = groups_get_recv(subreq, NULL);
    talloc_zfree(subreq);
    if (ret != EOK) {
        tevent_req_error(req, ret);
        return;
    }

    tevent_req_done(req);
}

int sdap_get_initgr_recv(struct tevent_req *req)
{
    TEVENT_REQ_RETURN_ON_ERROR(req);

    return EOK;
}

