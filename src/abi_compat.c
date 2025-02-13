/*
 * Copyright (C) 2021 Canonical, Ltd.
 * Author: Simon Chopin <simon.chopin@canonical.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * The whole point of this file is to export the former ABI as simple wrappers
 * around the newer API. Most functions should thus be relatively short, the meat
 * of things being in the newer API implementation.
 */

#include "netplan.h"
#include "types.h"
#include "util-internal.h"
#include "parse-nm.h"
#include "parse-globals.h"
#include "names.h"
#include "networkd.h"
#include "nm.h"
#include "openvswitch.h"
#include "util.h"

#include <unistd.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <errno.h>

/* These arrays are not useful per-say, but allow us to export the various
 * struct offsets of the netplan_state members to the linker, which can use
 * them in a linker script to create symbols pointing to the internal data
 * members of the global_state global object.
 */

/* The +8 is to prevent the compiler removing the array if the array is empty,
 * i.e. the data member is the first in the struct definition.
 */
__attribute__((used)) __attribute__((section("netdefs_offset")))
char _netdefs_off[8+offsetof(struct netplan_state, netdefs)] = {};

__attribute__((used)) __attribute__((section("netdefs_ordered_offset")))
char _netdefs_ordered_off[8+offsetof(struct netplan_state, netdefs_ordered)] = {};

__attribute__((used)) __attribute__((section("ovs_settings_offset")))
char _ovs_settings_global_off[8+offsetof(struct netplan_state, ovs_settings)] = {};

__attribute__((used)) __attribute__((section("global_backend_offset")))
char _global_backend_off[8+offsetof(struct netplan_state, backend)] = {};

NETPLAN_ABI
NetplanState global_state = {};

// LCOV_EXCL_START
NetplanBackend
netplan_get_global_backend()
{
    return netplan_state_get_backend(&global_state);
}
// LCOV_EXCL_STOP

/**
 * Clear NetplanNetDefinition hashtable
 */
guint
netplan_clear_netdefs()
{
    guint n = netplan_state_get_netdefs_size(&global_state);
    netplan_state_reset(&global_state);
    netplan_parser_reset(&global_parser);
    return n;
}

// LCOV_EXCL_START
NETPLAN_INTERNAL void
write_network_file(const NetplanNetDefinition* def, const char* rootdir, const char* path)
{
    GError* error = NULL;
    if (!netplan_netdef_write_network_file(&global_state, def, rootdir, path, NULL, &error))
    {
        g_fprintf(stderr, "%s", error->message);
        exit(1);
    }
}

/**
 * Generate networkd configuration in @rootdir/run/systemd/network/ from the
 * parsed #netdefs.
 * @rootdir: If not %NULL, generate configuration in this root directory
 *           (useful for testing).
 * Returns: TRUE if @def applies to networkd, FALSE otherwise.
 */
gboolean
write_networkd_conf(const NetplanNetDefinition* def, const char* rootdir)
{
    GError* error = NULL;
    gboolean has_been_written;
    if (!netplan_netdef_write_networkd(&global_state, def, rootdir, &has_been_written, &error))
    {
        g_fprintf(stderr, "%s", error->message);
        exit(1);
    }
    return has_been_written;
}

NETPLAN_INTERNAL void
cleanup_networkd_conf(const char* rootdir)
{
    netplan_networkd_cleanup(rootdir);
}

// There only for compatibility purposes, the proper implementation is now directly
// in the `generate` binary.
NETPLAN_ABI void
enable_networkd(const char* generator_dir)
{
    g_autofree char* link = g_build_path(G_DIR_SEPARATOR_S, generator_dir, "multi-user.target.wants", "systemd-networkd.service", NULL);
    g_debug("We created networkd configuration, adding %s enablement symlink", link);
    safe_mkdir_p_dir(link);
    if (symlink("../systemd-networkd.service", link) < 0 && errno != EEXIST) {
        g_fprintf(stderr, "failed to create enablement symlink: %m\n");
        exit(1);
    }

    g_autofree char* link2 = g_build_path(G_DIR_SEPARATOR_S, generator_dir, "network-online.target.wants", "systemd-networkd-wait-online.service", NULL);
    safe_mkdir_p_dir(link2);
    if (symlink("/lib/systemd/system/systemd-networkd-wait-online.service", link2) < 0 && errno != EEXIST) {
        g_fprintf(stderr, "failed to create enablement symlink: %m\n");
        exit(1);
    }
}

NETPLAN_INTERNAL void
write_nm_conf(NetplanNetDefinition* def, const char* rootdir)
{
    GError* error = NULL;
    if (!netplan_netdef_write_nm(&global_state, def, rootdir, NULL, &error)) {
        g_fprintf(stderr, "%s", error->message);
        exit(1);
    }
}

NETPLAN_INTERNAL void
write_nm_conf_finish(const char* rootdir)
{
    /* Original implementation had no error possible!! */
    g_assert(netplan_state_finish_nm_write(&global_state, rootdir, NULL));
}

NETPLAN_INTERNAL void
cleanup_nm_conf(const char* rootdir)
{
    netplan_nm_cleanup(rootdir);
}

NETPLAN_INTERNAL void
write_ovs_conf(const NetplanNetDefinition* def, const char* rootdir)
{
    GError* error = NULL;
    if (!netplan_netdef_write_ovs(&global_state, def, rootdir, NULL, &error)) {
        g_fprintf(stderr, "%s", error->message);
        exit(1);
    }
}

NETPLAN_INTERNAL void
write_ovs_conf_finish(const char* rootdir)
{
    /* Original implementation had no error possible!! */
    g_assert(netplan_state_finish_ovs_write(&global_state, rootdir, NULL));
}

NETPLAN_INTERNAL void
cleanup_ovs_conf(const char* rootdir)
{
    netplan_ovs_cleanup(rootdir);
}
// LCOV_EXCL_STOP

gboolean
netplan_parse_yaml(const char* filename, GError** error)
{
    return netplan_parser_load_yaml(&global_parser, filename, error);
}

/**
 * Post-processing after parsing all config files
 */
GHashTable *
netplan_finish_parse(GError** error)
{
    if (netplan_state_import_parser_results(&global_state, &global_parser, error))
        return global_state.netdefs;
    return NULL;
}

/**
 * Generate the Netplan YAML configuration for the selected netdef
 * @def: NetplanNetDefinition (as pointer), the data to be serialized
 * @rootdir: If not %NULL, generate configuration in this root directory
 *           (useful for testing).
 */
void
write_netplan_conf(const NetplanNetDefinition* def, const char* rootdir)
{
    netplan_netdef_write_yaml(&global_state, def, rootdir, NULL);
}

gboolean
netplan_state_write_yaml(const NetplanState* np_state, const char* file_hint, const char* rootdir, GError** error);

/**
 * Generate the Netplan YAML configuration for all currently parsed netdefs
 * @file_hint: Name hint for the generated output YAML file
 * @rootdir: If not %NULL, generate configuration in this root directory
 *           (useful for testing).
 */
NETPLAN_ABI void
write_netplan_conf_full(const char* file_hint, const char* rootdir)
{
    netplan_finish_parse(NULL);
    netplan_state_write_yaml(&global_state, file_hint, rootdir, NULL);
}

NETPLAN_PUBLIC gboolean
netplan_parse_keyfile(const char* filename, GError** error)
{
    return netplan_parser_load_keyfile(&global_parser, filename, error);
}

// LCOV_EXCL_START
void process_input_file(const char *f)
{
    GError* error = NULL;

    g_debug("Processing input file %s..", f);
    if (!netplan_parser_load_yaml(&global_parser, f, &error)) {
        g_fprintf(stderr, "%s\n", error->message);
        exit(1);
    }
}

gboolean
process_yaml_hierarchy(const char* rootdir)
{
    GError* error = NULL;
    if (!netplan_parser_load_yaml_hierarchy(&global_parser, rootdir, &error)) {
        g_fprintf(stderr, "%s\n", error->message);
        exit(1);
    }
    return TRUE;
}
// LCOV_EXCL_STOP

/**
 * Helper function for testing only
 */
NETPLAN_INTERNAL void
_write_netplan_conf(const char* netdef_id, const char* rootdir)
{
    GHashTable* ht = NULL;
    const NetplanNetDefinition* def = NULL;
    ht = netplan_finish_parse(NULL);
    def = g_hash_table_lookup(ht, netdef_id);
    write_netplan_conf(def, rootdir);
}

/**
 * Get the filename from which the given netdef has been parsed.
 * @rootdir: ID of the netdef to be looked up
 * @rootdir: parse files from this root directory
 */
gchar*
netplan_get_filename_by_id(const char* netdef_id, const char* rootdir)
{
    NetplanParser* npp = netplan_parser_new();
    NetplanState* np_state = netplan_state_new();
    char *filename = NULL;
    GError* error = NULL;

    if (!netplan_parser_load_yaml_hierarchy(npp, rootdir, &error) ||
            !netplan_state_import_parser_results(np_state, npp, &error)) {
        g_fprintf(stderr, "%s\n", error->message);
        return NULL;
    }
    netplan_parser_clear(&npp);

    netplan_state_get_netdef(np_state, netdef_id);
    filename = g_strdup(netplan_netdef_get_filename(netplan_state_get_netdef(np_state, netdef_id)));
    netplan_state_clear(&np_state);
    return filename;
}
