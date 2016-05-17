/**
 * \file dash/dart/base/locality.c
 */

/*
 * Include config and utmpx.h first to prevent previous include of utmpx.h
 * without _GNU_SOURCE in included headers:
 */
#include <dash/dart/base/config.h>
#ifdef DART__PLATFORM__LINUX
#  define _GNU_SOURCE
#  include <utmpx.h>
#endif

#include <dash/dart/base/locality.h>
#include <dash/dart/base/macro.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/logging.h>
#include <dash/dart/base/assert.h>
#include <dash/dart/base/hwinfo.h>

#include <dash/dart/base/internal/host_topology.h>
#include <dash/dart/base/internal/unit_locality.h>
#include <dash/dart/base/internal/domain_locality.h>

#include <dash/dart/base/string.h>

#include <dash/dart/if/dart_types.h>
#include <dash/dart/if/dart_locality.h>
#include <dash/dart/if/dart_communication.h>

#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sched.h>
#include <limits.h>

/* ====================================================================== *
 * Private Data                                                           *
 * ====================================================================== */

#define DART__BASE__LOCALITY__MAX_TEAM_DOMAINS 32

dart_host_topology_t *
dart__base__locality__host_topology_[DART__BASE__LOCALITY__MAX_TEAM_DOMAINS];

dart_unit_mapping_t *
dart__base__locality__unit_mapping_[DART__BASE__LOCALITY__MAX_TEAM_DOMAINS];

dart_domain_locality_t *
dart__base__locality__global_domain_[DART__BASE__LOCALITY__MAX_TEAM_DOMAINS];

/* ====================================================================== *
 * Private Functions                                                      *
 * ====================================================================== */

static int cmpstr_(const void * p1, const void * p2) {
  return strcmp(* (char * const *) p1, * (char * const *) p2);
}

dart_ret_t dart__base__locality__scope_domains_rec(
  const dart_domain_locality_t   * domain,
  dart_locality_scope_t            scope,
  int                            * num_domains_out,
  char                         *** domain_tags_out);

dart_locality_scope_t dart__base__locality__scope_parent(
  dart_locality_scope_t      scope);

dart_locality_scope_t dart__base__locality__scope_child(
  dart_locality_scope_t      scope);

/* ====================================================================== *
 * Init / Finalize                                                        *
 * ====================================================================== */

dart_ret_t dart__base__locality__init()
{
  return dart__base__locality__create(DART_TEAM_ALL);
}

dart_ret_t dart__base__locality__finalize()
{
  for (dart_team_t t = 0; t < DART__BASE__LOCALITY__MAX_TEAM_DOMAINS; ++t) {
    dart__base__locality__delete(t);
  }

  dart_barrier(DART_TEAM_ALL);
  return DART_OK;
}

/* ====================================================================== *
 * Create / Delete                                                        *
 * ====================================================================== */

dart_ret_t dart__base__locality__create(
  dart_team_t team)
{
  DART_LOG_DEBUG("dart__base__locality__create() team(%d)", team);

  dart_hwinfo_t * hwinfo;
  DART_ASSERT_RETURNS(dart_hwinfo(&hwinfo), DART_OK);

  for (int td = 0; td < DART__BASE__LOCALITY__MAX_TEAM_DOMAINS; ++td) {
    dart__base__locality__global_domain_[td] = NULL;
    dart__base__locality__host_topology_[td] = NULL;
  }

  dart_domain_locality_t * team_global_domain =
    malloc(sizeof(dart_domain_locality_t));
  dart__base__locality__global_domain_[team] =
    team_global_domain;

  /* Initialize the global domain as the root entry in the locality
   * hierarchy: */
  team_global_domain->scope          = DART_LOCALITY_SCOPE_GLOBAL;
  team_global_domain->level          = 0;
  team_global_domain->relative_index = 0;
  team_global_domain->team           = team;
  team_global_domain->parent         = NULL;
  team_global_domain->num_domains    = 0;
  team_global_domain->domains        = NULL;
  team_global_domain->hwinfo         = *hwinfo;
  team_global_domain->num_units      = 0;
  team_global_domain->host[0]        = '\0';
  team_global_domain->domain_tag[0]  = '.';
  team_global_domain->domain_tag[1]  = '\0';

  size_t num_units = 0;
  DART_ASSERT_RETURNS(dart_team_size(team, &num_units), DART_OK);
  team_global_domain->num_units = num_units;

  team_global_domain->unit_ids =
    malloc(num_units * sizeof(dart_unit_t));
  for (size_t u = 0; u < num_units; ++u) {
    team_global_domain->unit_ids[u] = u;
  }

  /* Exchange unit locality information between all units: */
  dart_unit_mapping_t * unit_mapping;
  DART_ASSERT_RETURNS(
    dart__base__unit_locality__create(team, &unit_mapping),
    DART_OK);
  dart__base__locality__unit_mapping_[team] = unit_mapping;

  /* Copy host names of all units into array: */
  const int max_host_len = DART_LOCALITY_HOST_MAX_SIZE;
  DART_LOG_TRACE("dart__base__locality__create: copying host names");
  char ** hosts = malloc(sizeof(char *) * num_units);
  for (size_t u = 0; u < num_units; ++u) {
    hosts[u] = malloc(sizeof(char) * max_host_len);
    dart_unit_locality_t * ul;
    DART_ASSERT_RETURNS(
      dart__base__unit_locality__at(unit_mapping, u, &ul),
      DART_OK);
    strncpy(hosts[u], ul->host, max_host_len);
  }

  dart_host_topology_t * topo = malloc(sizeof(dart_host_topology_t));
  DART_ASSERT_RETURNS(
    dart__base__host_topology__create(
      hosts, team, unit_mapping, topo),
    DART_OK);
  dart__base__locality__host_topology_[team] = topo;
  size_t num_nodes = topo->num_nodes;
  DART_LOG_TRACE("dart__base__locality__create: nodes: %d", num_nodes);

  team_global_domain->num_nodes = num_nodes;

#ifdef DART_ENABLE_LOGGING
  for (int h = 0; h < topo->num_hosts; ++h) {
    dart_node_units_t * node_units = &topo->node_units[h];
    char * hostname = topo->host_names[h];
    DART_LOG_TRACE("dart__base__locality__create: "
                   "host %s: units:%d level:%d parent:%s", hostname,
                   node_units->num_units, node_units->level,
                   node_units->parent);
    for (int u = 0; u < node_units->num_units; ++u) {
      DART_LOG_TRACE("dart__base__locality__create: %s unit[%d]: %d",
                     hostname, u, node_units->units[u]);
    }
  }
#endif

  /* recursively create locality information of the global domain's
   * sub-domains: */
  DART_ASSERT_RETURNS(
    dart__base__locality__domain__create_subdomains(
      dart__base__locality__global_domain_[team],
      dart__base__locality__host_topology_[team],
      dart__base__locality__unit_mapping_[team]),
    DART_OK);

  DART_LOG_DEBUG("dart__base__locality__create >");
  return DART_OK;
}

dart_ret_t dart__base__locality__delete(
  dart_team_t team)
{
  dart_ret_t ret = DART_OK;

  if (dart__base__locality__global_domain_[team] == NULL &&
      dart__base__locality__host_topology_[team] == NULL &&
      dart__base__locality__unit_mapping_[team]  == NULL) {
    return ret;
  }

  DART_LOG_DEBUG("dart__base__locality__delete() team(%d)", team);

  if (dart__base__locality__global_domain_[team] != NULL) {
    ret = dart__base__locality__domain__delete(
            dart__base__locality__global_domain_[team]);
    if (ret != DART_OK) {
      DART_LOG_ERROR("dart__base__locality__delete ! "
                     "dart__base__locality__domain_delete failed: %d", ret);
      return ret;
    }
    dart__base__locality__global_domain_[team] = NULL;
  }

  if (dart__base__locality__host_topology_[team] != NULL) {
    ret = dart__base__host_topology__delete(
            dart__base__locality__host_topology_[team]);
    if (ret != DART_OK) {
      DART_LOG_ERROR("dart__base__locality__delete ! "
                     "dart__base__host_topology__delete failed: %d", ret);
      return ret;
    }
    dart__base__locality__host_topology_[team] = NULL;
  }

  if (dart__base__locality__unit_mapping_[team] != NULL) {
    ret = dart__base__unit_locality__delete(
            dart__base__locality__unit_mapping_[team]);
    if (ret != DART_OK) {
      DART_LOG_ERROR("dart__base__locality__delete ! "
                     "dart__base__unit_locality__delete failed: %d", ret);
      return ret;
    }
    dart__base__locality__unit_mapping_[team] = NULL;
  }

  DART_LOG_DEBUG("dart__base__locality__delete > team(%d)", team);
  return DART_OK;
}

/* ====================================================================== *
 * Domain Locality                                                        *
 * ====================================================================== */

dart_ret_t dart__base__locality__team_domain(
  dart_team_t                team,
  dart_domain_locality_t  ** domain_out)
{
  DART_LOG_DEBUG("dart__base__locality__team_domain() team(%d)", team);
  dart_ret_t ret = DART_ERR_NOTFOUND;

  *domain_out = NULL;
  dart_domain_locality_t * domain =
    dart__base__locality__global_domain_[team];

  ret = dart__base__locality__domain(domain, ".", domain_out);

  DART_LOG_DEBUG("dart__base__locality__team_domain > "
                 "team(%d) -> domain(%p)", team, *domain_out);
  return ret;
}

dart_ret_t dart__base__locality__domain(
  dart_domain_locality_t   * domain_in,
  const char               * domain_tag,
  dart_domain_locality_t  ** domain_out)
{
  DART_LOG_DEBUG("dart__base__locality__domain() "
                 "domain_in(%s) domain_tag(%s)",
                 domain_in, domain_tag);

  *domain_out = NULL;
  dart_domain_locality_t * domain = domain_in;

  /* pointer to dot separator in front of tag part:  */
  char * dot_begin  = strchr(domain_tag, '.');
  /* pointer to begin of tag part: */
  char * part_begin = dot_begin + 1;
  /* Iterate tag (.1.2.3) by parts (1, 2, 3):        */
  int    level     = 0;
  while (dot_begin != NULL && *dot_begin != '\0' && *part_begin != '\0') {
    char * dot_end;
    /* domain tag part converted to int is relative index of child: */
    long   tag_part      = strtol(part_begin, &dot_end, 10);
    int    subdomain_idx = (int)(tag_part);
    if (domain == NULL) {
      /* tree leaf node reached before last tag part: */
      DART_LOG_ERROR("dart__base__locality__domain ! "
                     "domain(%s) domain_tag(%s): "
                     "subdomain at index %d in level %d is undefined",
                     domain->domain_tag, domain_tag, subdomain_idx, level);
      return DART_ERR_NOTFOUND;
    }
    if (domain->num_domains <= subdomain_idx) {
      /* child index out of range: */
      DART_LOG_ERROR("dart__base__locality__domain ! "
                     "domain(%s) domain_tag(%s): "
                     "subdomain at index %d in level %d is out of bounds "
                     "(number of subdomains: %d)",
                     domain->domain_tag, domain_tag, subdomain_idx, level,
                     domain->num_domains);
      return DART_ERR_NOTFOUND;
    }
    /* descend to child at relative index: */
    domain     = domain->domains + subdomain_idx;
    /* continue scanning of domain tag at next part: */
    dot_begin  = dot_end;
    part_begin = dot_end+1;
    level++;
  }
  *domain_out = domain;
  DART_LOG_DEBUG("dart__base__locality__domain > "
                 "domain_in(%s) domain_tag(%s) -> %p",
                 domain_in->domain_tag, domain_tag, domain);
  return DART_OK;
}

dart_ret_t dart__base__locality__scope_domains(
  const dart_domain_locality_t     * domain_in,
  dart_locality_scope_t              scope,
  int                              * num_domains_out,
  char                           *** domain_tags_out)
{
  *num_domains_out = 0;
  *domain_tags_out = NULL;
  return dart__base__locality__scope_domains_rec(
           domain_in, scope, num_domains_out, domain_tags_out);
}

dart_ret_t dart__base__locality__domain_split_tags(
  const dart_domain_locality_t     * domain_in,
  dart_locality_scope_t              scope,
  int                                num_parts,
  int                             ** group_sizes_out,
  char                          **** group_domain_tags_out)
{
  /* For 4 domains in the specified scope, a split into 2 parts results
   * in a domain hierarchy like:
   *
   *   group_domain_tags[g][d] -> {
   *                                0: [ domain_0, domain_1 ],
   *                                1: [ domain_2, domain_3 ], ...
   *                              }
   *
   */

  DART_LOG_TRACE("dart__base__locality__domain_split_tags() "
                 "team(%d) domain(%s) scope(%d) parts(%d)",
                 domain_in->team, domain_in->domain_tag, scope, num_parts);

  /* Subdomains of global domain.
   * Domains of split parts, grouping domains at split scope. */
  char *** group_domain_tags = malloc(num_parts * sizeof(char **));
  int    * group_sizes       = malloc(num_parts * sizeof(int));

  /* Get total number and tags of domains in split scope: */
  int     num_domains;
  char ** domain_tags;
  DART_ASSERT_RETURNS(
    dart__base__locality__scope_domains(
      domain_in, scope, &num_domains, &domain_tags),
    DART_OK);

  /* Group domains in split scope into specified number of parts: */
  int max_group_domains      = (num_domains + (num_parts-1)) / num_parts;
  int group_first_domain_idx = 0;
  /*
   * TODO: Preliminary implementation, should balance number of units in
   *       groups.
   */
  for (int g = 0; g < num_parts; ++g) {
    int num_group_subdomains = max_group_domains;
    if ((g+1) * max_group_domains > num_domains) {
      num_group_subdomains = (g * max_group_domains) - num_domains;
    }
    DART_LOG_TRACE("dart__base__locality__domain_split_tags: "
                   "domains in group %d: %d", g, num_group_subdomains);

    group_sizes[g]       = num_group_subdomains;
    group_domain_tags[g] = malloc(sizeof(char *) * num_group_subdomains);

    for (int d_rel = 0; d_rel < num_group_subdomains; ++d_rel) {
      int d_abs   = group_first_domain_idx + d_rel;
      int tag_len = strlen(domain_tags[d_abs]);
      group_domain_tags[g][d_rel] = malloc(sizeof(char) * (tag_len + 1));
      strncpy(group_domain_tags[g][d_rel], domain_tags[d_abs],
              DART_LOCALITY_DOMAIN_TAG_MAX_SIZE);
      group_domain_tags[g][d_rel][tag_len] = '\0';
    }
    /* Create domain of group: */
    group_first_domain_idx += num_group_subdomains;
  }

  *group_sizes_out       = group_sizes;
  *group_domain_tags_out = group_domain_tags;

  DART_LOG_TRACE("dart__base__locality__domain_split_tags >");
  return DART_OK;
}

dart_ret_t dart__base__locality__domain_group(
  dart_domain_locality_t   * domain,
  int                        num_groups,
  const int                * group_sizes,
  const char             *** group_domain_tags)
{
  DART_LOG_TRACE("dart__base__locality__domain_group() "
                 "domain_in: (%s: %d @ %d) num_groups: %d",
                 domain->domain_tag, domain->scope, domain->level,
                 num_groups);
#ifdef DART_ENABLE_LOGGING
  for (int g = 0; g < num_groups; g++) {
    for (int sd = 0; sd < group_sizes[g]; sd++) {
      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "group_domain_tags[%d][%d]: %s",
                     g, sd, group_domain_tags[g][sd]);
    }
  }
#endif

  if (num_groups < 1) {
    return DART_ERR_INVAL;
  }

  dart_ret_t ret;
  for (int g = 0; g < num_groups; g++) {
    DART_LOG_TRACE("dart__base__locality__domain_group: "
                   "group[%d] size: %d", g, group_sizes[g]);

    /* The group's parent domain: */
    dart_domain_locality_t * group_parent_domain;
    dart__base__locality__domain__parent(
      domain, group_domain_tags[g], group_sizes[g], &group_parent_domain);

    DART_LOG_TRACE("dart__base__locality__domain_group: "
                   "group[%d] parent: %s",
                   g, group_parent_domain->domain_tag);

    /* Find parents of specified subdomains that are an immediate child node
     * of the input domain.
     */
    int immediate_subdomains_group = 1;
    int num_group_parent_domain_tag_parts =
          dart__base__strcnt(group_parent_domain->domain_tag, '.');
    for (int sd = 0; sd < group_sizes[g]; sd++) {
      const char * group_domain_tag = group_domain_tags[g][sd];
      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "    group_domain_tags[%d][%d]: %s",
                     g, sd, group_domain_tag);
      if (dart__base__strcnt(group_domain_tag, '.') !=
          num_group_parent_domain_tag_parts + 1) {
        immediate_subdomains_group = 0;
        break;
      }
    }
    if (immediate_subdomains_group) {
      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "group[%d]: group of immediate child domains", g);
      /* Subdomains in group are immediate child nodes of group parent
       * domain:
       */
      ret = dart__base__locality__group_subdomains(
              group_parent_domain, group_domain_tags[g], group_sizes[g]);
      if (ret != DART_OK) {
        return ret;
      }
    } else {
      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "group[%d]: group of indirect child domains", g);

      /* Subdomains in group are in indirect child nodes of group parent
       * domain.
       * Find immediate child nodes that are parents of group subdomains.
       * Example:
       *
       *     parent:        .0
       *     group domains: { .0.1.2, .0.1.3, .0.2.0 }
       *
       *     --> { .0.1, .0.1, .0.2 }
       *
       *     --> groups:  { .0.1, .0.2 }
       */
      char ** immediate_subdomain_tags    = malloc(sizeof(char *) *
                                                   group_sizes[g]);
      char *  group_parent_domain_tag     = group_parent_domain->domain_tag;
      int     group_parent_domain_tag_len = strlen(group_parent_domain_tag);
      DART_LOG_TRACE("dart__base__locality__domain_group: parent: %s",
                     group_parent_domain_tag);
      for (int sd = 0; sd < group_sizes[g]; sd++) {
        /* Resolve relative index of subdomain: */
        immediate_subdomain_tags[sd] =
          calloc(sizeof(char) * DART_LOCALITY_DOMAIN_TAG_MAX_SIZE,
                 sizeof(char));

        char * dot_pos = strchr(group_domain_tags[g][sd] +
                                group_parent_domain_tag_len + 1, '.');
        int immediate_subdomain_tag_len;
        if (dot_pos == NULL) {
          /* subdomain is immediate child of parent: */
          immediate_subdomain_tag_len = strlen(group_domain_tags[g][sd]);
        } else {
          /* subdomain is indirect child of parent: */
          immediate_subdomain_tag_len = dot_pos -
                                        group_domain_tags[g][sd];
        }
        strncpy(immediate_subdomain_tags[sd], group_domain_tags[g][sd],
                immediate_subdomain_tag_len);
        immediate_subdomain_tags[sd][immediate_subdomain_tag_len] = '\0';
      }
      int num_group_subdomains = dart__base__strsunique(
                                   immediate_subdomain_tags,
                                   group_sizes[g]);
      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "num_group_subdomains: %d", num_group_subdomains);
#ifdef DART_ENABLE_LOGGING
      for (int gsd = 0; gsd < num_group_subdomains; gsd++) {
        DART_LOG_TRACE("dart__base__locality__domain_group: "
                       "group[%d].subdomain[%d]: %s",
                       g, gsd, immediate_subdomain_tags[gsd]);
      }
#endif

      /*
       * Note: Required to append group domain at the end of the group
       *       parent's subdomain list to ensure that tags of domains not
       *       included in group remain valid.
       */
      group_parent_domain->domains =
        realloc(group_parent_domain->domains,
                sizeof(dart_domain_locality_t) *
                  (group_parent_domain->num_domains + 1));
      dart_domain_locality_t * group_domain =
        group_parent_domain->domains + group_parent_domain->num_domains;

      dart__base__locality__domain__init(group_domain);

      group_domain->team           = group_parent_domain->team;
      group_domain->scope          = DART_LOCALITY_SCOPE_GROUP;
      group_domain->level          = group_parent_domain->level + 1;
      group_domain->parent         = group_parent_domain;
      group_domain->relative_index = group_parent_domain->num_domains;
      group_domain->num_nodes      = group_parent_domain->num_nodes;
      group_domain->num_units      = 0;
      group_domain->unit_ids       = NULL;
      group_domain->num_domains    = 0;
      group_domain->domains        = malloc(sizeof(dart_domain_locality_t) *
                                            num_group_subdomains);

      strncpy(group_domain->domain_tag, group_parent_domain_tag,
              group_parent_domain_tag_len);
      group_domain->domain_tag[group_parent_domain_tag_len] = '\0';
      int group_domain_tag_len =
        sprintf(group_domain->domain_tag + group_parent_domain_tag_len, ".%d",
                group_domain->relative_index) +
        group_parent_domain_tag_len;
      group_domain->domain_tag[group_domain_tag_len] = '\0';

      /* Initialize group subdomains:
       */
      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "initialize %d subdomains of group[%d] (%s)",
                     num_group_subdomains, g, group_domain->domain_tag);

      for (int gsd = 0; gsd < num_group_subdomains; gsd++) {
        dart_domain_locality_t * group_subdomain_in;

        /* Copy
         *   domain.domains[domain_tag = group[g].immediate_subdomains[gsd]]
         * to
         *   group[g].domains[gsd]:
         */
        DART_LOG_TRACE("dart__base__locality__domain_group: "
                       "load domain.domains[domain_tag = "
                       "(group[%d].immediate_subdomain_tags[%d] = %s])",
                       g, gsd, immediate_subdomain_tags[gsd]);

        ret = dart__base__locality__domain(
                domain, immediate_subdomain_tags[gsd],
                &group_subdomain_in);

        DART_LOG_TRACE("dart__base__locality__domain_group: "
                       "copy domain.domains[domain_tag = %s] to "
                       "group[%d].domains[%d]",
                       immediate_subdomain_tags[gsd], g, gsd);
        ret = dart__base__locality__domain__copy(
                group_subdomain_in,
                group_domain->domains + gsd);

        group_domain->num_domains++;

        /* Set parent of group subdomains to group domain:
         */
        group_domain->domains[gsd].parent = group_domain;

        /* Remove entries from group domain that are not part of the group:
         */
        DART_LOG_TRACE("dart__base__locality__domain_group: "
                       "select %d subdomains in group[%d].domains[%d] = %s",
                       group_sizes[g], g, gsd,
                       group_domain->domains[gsd].domain_tag);
        ret = dart__base__locality__domain__select_subdomains(
                group_domain->domains + gsd,
                group_domain_tags[g],
                group_sizes[g]);

      } /* for group_domain.domains */

      DART_LOG_TRACE("dart__base__locality__domain_group: "
                     "update group[%d] (%s) after adding subdomains",
                     g, group_domain->domain_tag);
      ret = dart__base__locality__domain__update_subdomains(
              group_domain);

#if 0
      /* Remove subdomains in groups from ungrouped subdomains:
       */
      for (int sd = 0; sd < group_parent_domain->num_domains; sd++) {
        int ungrouped_subdomain = 1;
        for (int gd = 0; gd < group_sizes[g]; ++gd) {
          if (strcmp(group_parent_domain->domains[sd].domain_tag,
                     immediate_subdomain_tags[gd]) == 0) {
            ungrouped_subdomain = 0;
            break;
          }
        }
        if (ungrouped_subdomain) {
          dart__base__locality__domain__remove_subdomains(
            &group_parent_domain->domains[sd],
            group_domain_tags[g],
            group_sizes[g]);
        }
      }
#endif
      for (int sd = 0; sd < group_sizes[g]; sd++) {
        free(immediate_subdomain_tags[sd]);
      }
      free(immediate_subdomain_tags);

      group_parent_domain->num_domains++;

      if (ret != DART_OK) {
        return ret;
      }
    }
  }

  DART_LOG_TRACE("dart__base__locality__domain_group >");
  return DART_OK;
}

/**
 * Move subset of a domain's immediate child nodes into a group subdomain.
 */
dart_ret_t dart__base__locality__group_subdomains(
  dart_domain_locality_t   * domain,
  const char              ** group_subdomain_tags,
  int                        num_group_subdomain_tags)
{
  DART_LOG_TRACE("dart__base__locality__group_subdomains() "
                 "group parent domain: %s num domains: %d "
                 "num_group_subdomain_tags: %d",
                 domain->domain_tag, domain->num_domains,
                 num_group_subdomain_tags);

  int num_grouped        = num_group_subdomain_tags;
  int num_ungrouped      = domain->num_domains - num_grouped;
  int num_subdomains_new = num_ungrouped + 1;

  /* Child nodes are ordered by domain tag.
   * Create sorted copy of subdomain tags to partition child nodes in a
   * single pass:
   */
  char ** group_subdomain_tags_sorted =
    malloc(num_group_subdomain_tags * sizeof(char *));
  for (int sdt = 0; sdt < num_group_subdomain_tags; ++sdt) {
    group_subdomain_tags_sorted[sdt] =
      malloc((strlen(group_subdomain_tags[sdt]) + 1) * sizeof(char));
    strcpy(group_subdomain_tags_sorted[sdt], group_subdomain_tags[sdt]);
  }
  qsort(group_subdomain_tags_sorted, num_group_subdomain_tags,
        sizeof(char*), cmpstr_);

  int num_existing_domain_groups = 0;
  for (int sd = 0; sd < domain->num_domains; sd++) {
    if (domain->domains[sd].scope == DART_LOCALITY_SCOPE_GROUP) {
      num_existing_domain_groups++;
    }
  }
  num_ungrouped -= num_existing_domain_groups;

  /* Partition child nodes of domain into grouped and ungrouped
   * subdomains:
   */
  dart_domain_locality_t * group_domains =
    malloc(sizeof(dart_domain_locality_t) * num_existing_domain_groups);
  dart_domain_locality_t * grouped_domains =
    malloc(sizeof(dart_domain_locality_t) * num_grouped);
  dart_domain_locality_t * ungrouped_domains =
    malloc(sizeof(dart_domain_locality_t) * num_ungrouped);

  /* Copy child nodes into partitions:
   */
  int sdt                  = 0;
  int group_idx            = 0;
  int grouped_idx          = 0;
  int ungrouped_idx        = 0;
  int group_domain_rel_idx = num_existing_domain_groups;

  for (int sd = 0; sd < domain->num_domains; sd++) {
    dart_domain_locality_t * subdom        = &domain->domains[sd];
    dart_domain_locality_t * domain_copy;

    if (subdom->scope == DART_LOCALITY_SCOPE_GROUP) {
      domain_copy = &group_domains[group_idx];
      group_idx++;
    }
    else if (sdt < num_group_subdomain_tags &&
             strcmp(subdom->domain_tag, group_subdomain_tags_sorted[sdt])
             == 0) {
      domain_copy = &grouped_domains[grouped_idx];
      grouped_idx++;
      sdt++;
    } else {
      domain_copy = &ungrouped_domains[ungrouped_idx];
      ungrouped_idx++;
    }
    memcpy(domain_copy, subdom, sizeof(dart_domain_locality_t));
  }

  for (int sdt = 0; sdt < num_group_subdomain_tags; ++sdt) {
    free(group_subdomain_tags_sorted[sdt]);
  }
  free(group_subdomain_tags_sorted);

  /* Check that all subdomains have been found: */
  if (sdt != num_group_subdomain_tags) {
    return DART_ERR_NOTFOUND;
  }

  /* Append group domain to subdomains:
   */
  domain->domains =
    realloc(domain->domains,
            sizeof(dart_domain_locality_t) * num_subdomains_new);
  DART_ASSERT(domain->domains != NULL);

  domain->num_domains = num_subdomains_new;

  /* Initialize group domain and set it as the input domain's last child
   * node:
   */
  dart_domain_locality_t * group_domain =
    &domain->domains[group_domain_rel_idx];

  dart__base__locality__domain__init(group_domain);

  group_domain->parent         = domain;
  group_domain->relative_index = group_domain_rel_idx;
  group_domain->level          = domain->level + 1;
  group_domain->scope          = DART_LOCALITY_SCOPE_GROUP;
  group_domain->num_domains    = num_grouped;
  group_domain->domains        = malloc(sizeof(dart_domain_locality_t) *
                                        num_grouped);

  int tag_len = sprintf(group_domain->domain_tag, "%s", domain->domain_tag);
  sprintf(group_domain->domain_tag + tag_len, ".%d",
          group_domain->relative_index);
  DART_LOG_TRACE("dart__base__locality__group_subdomains: "
                 "group_domain.tag: %s relative index: %d grouped: %d "
                 "ungrouped: %d",
                 group_domain->domain_tag, group_domain->relative_index,
                 num_grouped, num_ungrouped);
  /*
   * Set grouped partition of subdomains as child nodes of group domain:
   */
  group_domain->num_units = 0;
  group_domain->num_nodes = 0;
  for (int gd = 0; gd < num_grouped; gd++) {
    memcpy(&group_domain->domains[gd], &grouped_domains[gd],
           sizeof(dart_domain_locality_t));

    int tag_len = sprintf(group_domain->domains[gd].domain_tag, "%s",
                          group_domain->domain_tag);
    sprintf(group_domain->domains[gd].domain_tag + tag_len, ".%d", gd);

    /* Pointers are invalidated by realloc, update parent pointers of
     * subdomains: */
    group_domain->domains[gd].parent         = group_domain;
    group_domain->domains[gd].relative_index = gd;
    group_domain->domains[gd].level          = group_domain->level + 1;

    group_domain->num_units += group_domain->domains[gd].num_units;
    group_domain->num_nodes += group_domain->domains[gd].num_nodes;
  }
  /*
   * Collect unit ids of group domain:
   */
  group_domain->unit_ids = malloc(sizeof(dart_unit_t) *
                                  group_domain->num_units);
  int group_domain_unit_idx = 0;
  for (int gd = 0; gd < num_grouped; gd++) {
    dart_domain_locality_t * group_subdomain = group_domain->domains + gd;
    memcpy(group_domain->unit_ids + group_domain_unit_idx,
           group_subdomain->unit_ids,
           sizeof(dart_unit_t) * group_subdomain->num_units);
    group_domain_unit_idx += group_subdomain->num_units;
  }

  for (int g = 0; g < num_existing_domain_groups; g++) {
    DART_LOG_TRACE(
      "dart__base__locality__group_subdomains: ==> domains[%d] g: %s",
      g, domain->domains[g].domain_tag);
    memcpy(&domain->domains[g], &group_domains[g],
           sizeof(dart_domain_locality_t));

    /* Pointers are invalidated by realloc, update parent pointers of
     * subdomains: */
    domain->domains[g].parent         = domain;
    domain->domains[g].relative_index = g;
  }
  DART_LOG_TRACE(
    "dart__base__locality__group_subdomains: ==> domains[%d] *: %s",
    group_domain->relative_index, group_domain->domain_tag);

  for (int sd = 0; sd < num_ungrouped; sd++) {
    int abs_sd = sd + num_existing_domain_groups + 1;
    DART_LOG_TRACE(
      "dart__base__locality__group_subdomains: ==> domains[%d] u: %s",
      abs_sd, domain->domains[abs_sd].domain_tag);
    memcpy(&domain->domains[abs_sd],
           &ungrouped_domains[sd],
           sizeof(dart_domain_locality_t));

    /* Pointers are invalidated by realloc, update parent pointers of
     * subdomains: */
    domain->domains[abs_sd].parent         = domain;
    domain->domains[abs_sd].relative_index = abs_sd;
  }

#ifdef DART_ENABLE_LOGGING
  int g_idx = 0;
  for (int sd = 0; sd < domain->num_domains; sd++) {
    dart_domain_locality_t * subdom = domain->domains + sd;
    DART_LOG_TRACE(
      "dart__base__locality__group_subdomains: --> domains[%d:%d]: "
      "tag: %s scope: %d subdomains: %d ADDR[%p]",
      sd, subdom->relative_index, subdom->domain_tag,
      subdom->scope, subdom->num_domains, subdom);
    if (subdom->scope == DART_LOCALITY_SCOPE_GROUP) {
      for (int gsd = 0; gsd < subdom->num_domains; gsd++) {
        dart_domain_locality_t * group_subdom = &(subdom->domains[gsd]);
        dart__unused(group_subdom);
        DART_LOG_TRACE(
          "dart__base__locality__group_subdomains: -->   groups[%d:%d]."
          "domains[%d]: tag: %s scope: %d subdomains: %d",
          g_idx, group_subdom->relative_index, gsd,
          group_subdom->domain_tag,
          group_subdom->scope, group_subdom->num_domains);
      }
      g_idx++;
    }
  }
#endif

  free(grouped_domains);
  free(ungrouped_domains);

  DART_LOG_TRACE("dart__base__locality__group_subdomains >");
  return DART_OK;
}

/* ====================================================================== *
 * Unit Locality                                                          *
 * ====================================================================== */

dart_ret_t dart__base__locality__unit(
  dart_team_t             team,
  dart_unit_t             unit,
  dart_unit_locality_t ** locality)
{
  DART_LOG_DEBUG("dart__base__locality__unit() team(%d) unit(%d)",
                 team, unit);
  *locality = NULL;

  dart_unit_locality_t * uloc;
  dart_ret_t ret = dart__base__unit_locality__at(
                     dart__base__locality__unit_mapping_[team], unit,
                     &uloc);
  if (ret != DART_OK) {
    DART_LOG_ERROR("dart_unit_locality: "
                   "dart__base__locality__unit(team:%d unit:%d) "
                   "failed (%d)", team, unit, ret);
    return ret;
  }
  *locality = uloc;

  DART_LOG_DEBUG("dart__base__locality__unit > team(%d) unit(%d)",
                 team, unit);
  return DART_OK;
}

/* ====================================================================== *
 * Private Function Definitions                                           *
 * ====================================================================== */

dart_ret_t dart__base__locality__scope_domains_rec(
  const dart_domain_locality_t   * domain,
  dart_locality_scope_t            scope,
  int                            * num_domains_out,
  char                         *** domain_tags_out)
{
  dart_ret_t ret;
  DART_LOG_TRACE("dart__base__locality__scope_domains() level %d",
                 domain->level);

  if (domain->scope == scope) {
    DART_LOG_TRACE("dart__base__locality__scope_domains domain %s matched",
                   domain->domain_tag);
    int     dom_idx           = *num_domains_out;
    *num_domains_out         += 1;
    char ** domain_tags_temp  = (char **)(
                                  realloc(*domain_tags_out,
                                            sizeof(char *) *
                                            (*num_domains_out)));
    if (domain_tags_temp != NULL) {
      int domain_tag_size         = strlen(domain->domain_tag) + 1;
      *domain_tags_out            = domain_tags_temp;
      (*domain_tags_out)[dom_idx] = malloc(sizeof(char) * domain_tag_size);
      strncpy((*domain_tags_out)[dom_idx], domain->domain_tag,
              DART_LOCALITY_DOMAIN_TAG_MAX_SIZE);
    } else {
      DART_LOG_ERROR("dart__base__locality__scope_domains ! "
                     "realloc failed");
      return DART_ERR_OTHER;
    }
  } else {
    for (int d = 0; d < domain->num_domains; ++d) {
      ret = dart__base__locality__scope_domains_rec(
              &domain->domains[d],
              scope,
              num_domains_out,
              domain_tags_out);
      if (ret != DART_OK) {
        return ret;
      }
    }
  }
  if (*num_domains_out <= 0) {
    DART_LOG_ERROR("dart__base__locality__scope_domains ! "
                   "no domains found");
    return DART_ERR_NOTFOUND;
  }
  DART_LOG_TRACE("dart__base__locality__scope_domains >");
  return DART_OK;
}

dart_locality_scope_t dart__base__locality__scope_parent(
  dart_locality_scope_t scope)
{
  switch (scope) {
    case DART_LOCALITY_SCOPE_GLOBAL: return DART_LOCALITY_SCOPE_NODE;
    case DART_LOCALITY_SCOPE_NODE:   return DART_LOCALITY_SCOPE_MODULE;
    case DART_LOCALITY_SCOPE_MODULE: return DART_LOCALITY_SCOPE_NUMA;
    case DART_LOCALITY_SCOPE_NUMA:   return DART_LOCALITY_SCOPE_CORE;
    default:                         return DART_LOCALITY_SCOPE_UNDEFINED;
  }
}

dart_locality_scope_t dart__base__locality__scope_child(
  dart_locality_scope_t scope)
{
  switch (scope) {
    case DART_LOCALITY_SCOPE_CORE:   return DART_LOCALITY_SCOPE_NUMA;
    case DART_LOCALITY_SCOPE_NUMA:   return DART_LOCALITY_SCOPE_MODULE;
    case DART_LOCALITY_SCOPE_MODULE: return DART_LOCALITY_SCOPE_NODE;
    case DART_LOCALITY_SCOPE_NODE:   return DART_LOCALITY_SCOPE_GLOBAL;
    default:                         return DART_LOCALITY_SCOPE_UNDEFINED;
  }
}
