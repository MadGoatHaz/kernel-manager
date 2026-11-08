#! /usr/bin/env python3
"""// Copyright (C) 2022-2025 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
"""

from sys import argv
import os
import json
import hashlib

if len(argv) != 3:
    print('Wrong number of arguments! Please specify header directory.')
    exit(1)

HEADER_ROOT = argv[1]
SOURCE_ROOT = argv[2]


def sha256_file(path):
    """Return the SHA-256 hex digest of a file, or None if it does not exist."""
    try:
        with open(path, 'rb') as f:
            return hashlib.sha256(f.read()).hexdigest()
    except FileNotFoundError:
        return None


def validate_compile_options(compile_options):
    """Validate the structure of compile_options.json.

    Expected structure:
        {
            "<repo_name>": {
                "<option_key>": "<pkgbuild_var>"
                | {"var": "<pkgbuild_var>", "values": {"<ui_value>": "<pkgbuild_value>", ...}},
                ...
            },
            ...
        }

    Exits with a clear error message on any violation.
    """
    if not isinstance(compile_options, dict):
        print("ERROR: compile_options.json top level must be an object, got {}".format(
            type(compile_options).__name__))
        exit(1)

    if not compile_options:
        print("ERROR: compile_options.json is empty; at least one repo entry is required.")
        exit(1)

    for repo_name, repo_opts in compile_options.items():
        if not isinstance(repo_name, str) or not repo_name:
            print("ERROR: repo key must be a non-empty string, got {!r}".format(repo_name))
            exit(1)
        if not isinstance(repo_opts, dict):
            print("ERROR: repo '{}' value must be an object, got {}".format(
                repo_name, type(repo_opts).__name__))
            exit(1)
        if not repo_opts:
            print("ERROR: repo '{}' has no options".format(repo_name))
            exit(1)

        for opt_key, opt_val in repo_opts.items():
            if not isinstance(opt_key, str) or not opt_key:
                print("ERROR: repo '{}' option key must be a non-empty string, got {!r}".format(
                    repo_name, opt_key))
                exit(1)

            if isinstance(opt_val, str):
                # Direct mapping: option -> PKGBUILD var
                if not opt_val:
                    print("ERROR: repo '{}' option '{}' has an empty string value".format(
                        repo_name, opt_key))
                    exit(1)
            elif isinstance(opt_val, dict):
                # Translation form: {"var": ..., "values": {...}}
                if "var" not in opt_val:
                    print("ERROR: repo '{}' option '{}' dict is missing required 'var' key".format(
                        repo_name, opt_key))
                    exit(1)
                var = opt_val["var"]
                if not isinstance(var, str) or not var:
                    print("ERROR: repo '{}' option '{}' 'var' must be a non-empty string, got {!r}".format(
                        repo_name, opt_key, var))
                    exit(1)

                if "values" in opt_val:
                    values = opt_val["values"]
                    if not isinstance(values, dict):
                        print("ERROR: repo '{}' option '{}' 'values' must be an object, got {}".format(
                            repo_name, opt_key, type(values).__name__))
                        exit(1)
                    for ui_val, pkg_val in values.items():
                        if not isinstance(ui_val, str):
                            print("ERROR: repo '{}' option '{}' 'values' key must be a string, got {!r}".format(
                                repo_name, opt_key, ui_val))
                            exit(1)
                        if not isinstance(pkg_val, str):
                            print("ERROR: repo '{}' option '{}' 'values' entry for {!r} must be a string, got {!r}".format(
                                repo_name, opt_key, ui_val, pkg_val))
                            exit(1)

                # Reject unknown keys in the dict form
                allowed_keys = {"var", "values"}
                for k in opt_val:
                    if k not in allowed_keys:
                        print("ERROR: repo '{}' option '{}' has unknown key {!r} (allowed: {})".format(
                            repo_name, opt_key, k, sorted(allowed_keys)))
                        exit(1)
            else:
                print("ERROR: repo '{}' option '{}' value must be a string or object, got {}".format(
                    repo_name, opt_key, type(opt_val).__name__))
                exit(1)


def needs_run():
    """Determine if regeneration is needed based on content hashes.

    Compares SHA-256 hashes of the input JSON and this script against
    hashes stored in the generated header. This is more robust than mtime
    comparison (which breaks on git checkout, clock skew, etc.).
    """
    header_path = HEADER_ROOT + '/compile_options.hpp'
    json_path = SOURCE_ROOT + '/src/compile_options.json'

    # If the header doesn't exist, regenerate.
    if not os.path.exists(header_path):
        return True

    current_json_hash = sha256_file(json_path)
    current_script_hash = sha256_file(__file__)

    if current_json_hash is None:
        print("ERROR: input JSON not found: " + json_path)
        exit(1)

    # Read the stored hashes from the header (they are written as comments).
    stored_json_hash = None
    stored_script_hash = None
    with open(header_path, 'r', encoding='utf-8') as f:
        for line in f:
            if line.startswith('// mkoptions_hash: json='):
                stored_json_hash = line.split('json=', 1)[1].strip()
            elif line.startswith('// mkoptions_hash: script='):
                stored_script_hash = line.split('script=', 1)[1].strip()

    # If no stored hashes (old header format), regenerate.
    if stored_json_hash is None or stored_script_hash is None:
        return True

    # Regenerate if either hash differs.
    if current_json_hash != stored_json_hash or current_script_hash != stored_script_hash:
        return True

    return False


def map_type(values_count):
    return 'frozen::unordered_map<frozen::string, std::string_view, {}>'.format(values_count)


def main():
    if not needs_run():
        print("Output files up-to-date.")
        return

    header_file = HEADER_ROOT + '/compile_options.hpp'

    # Load and validate the JSON input.
    json_path = SOURCE_ROOT + '/src/compile_options.json'
    try:
        with open(json_path, 'r') as f:
            compile_options = json.load(f)
    except json.JSONDecodeError as e:
        print("ERROR: failed to parse compile_options.json: {} (line {}, col {})".format(
            e.msg, e.lineno, e.colno))
        exit(1)

    validate_compile_options(compile_options)

    # Compute content hashes for the staleness check.
    json_hash = sha256_file(json_path)
    script_hash = sha256_file(__file__)

    with open(header_file, 'w', encoding='utf-8', newline='\u000A') as header:
        header.write(globals()['__doc__'])
        header.write("// mkoptions_hash: json={}\n".format(json_hash))
        header.write("// mkoptions_hash: script={}\n".format(script_hash))
        header.write("""#pragma once

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#endif

#include <frozen/unordered_map.h>
#include <frozen/string.h>

#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <string_view>
#include <optional>
#include <variant>

namespace detail {

""")

        # Per-repo option maps. A string value maps option -> PKGBUILD var
        # directly (as before); an object value {"var", "values"} maps
        # option -> obj["var"] and feeds each obj["values"] pair into the
        # global flat value_xforms translation map keyed
        # "<repo>|<option>|<ui_value>".
        value_xforms = []
        distinct_map_sizes = []
        for map_name in compile_options.keys():
            map_obj = compile_options[map_name]
            values_count = len(map_obj)
            if values_count not in distinct_map_sizes:
                distinct_map_sizes.append(values_count)
            header.write('constexpr {} {} '.format(map_type(values_count), map_name))
            header.write('{\n')
            for key in map_obj:
                value = map_obj[key]
                if isinstance(value, dict):
                    header.write('    {{"{0}", "{1}"}},\n'.format(key, value['var']))
                    for src, dst in value.get('values', {}).items():
                        value_xforms.append((map_name + '|' + key + '|' + src, dst))
                else:
                    header.write('    {{"{0}", "{1}"}},\n'.format(key, value))
            header.write('\n};\n')

        # Global flat UI-value translation table: "<repo>|<option>|<ui_value>"
        # -> PKGBUILD value. An absent key means no translation (pass-through).
        header.write('// "<repo>|<option>|<ui_value>" -> PKGBUILD value (absent key = pass-through)\n')
        header.write('constexpr {} value_xforms {{\n'.format(map_type(len(value_xforms))))
        for key, dst in value_xforms:
            header.write('    {{"{0}", "{1}"}},\n'.format(key, dst))
        header.write('\n};\n')

        # One variant alternative per distinct per-repo map type: frozen maps
        # of differing sizes are distinct types, so repos sharing a size share
        # the alternative (std::variant alternatives must be distinct types);
        # the if-chain below still points at the specific per-repo map.
        header.write('\n')
        if distinct_map_sizes:
            alternatives = ', '.join('const {} *'.format(map_type(size)) for size in distinct_map_sizes)
            header.write('using option_maps_variant = std::variant<{}>;\n\n'.format(alternatives))
            header.write('inline std::optional<option_maps_variant> resolve_option_map(std::string_view repo)\n{\n')
            for map_name in compile_options.keys():
                index = distinct_map_sizes.index(len(compile_options[map_name]))
                header.write('    if (repo == "{0}") return std::optional<option_maps_variant>{{std::in_place_t{{}}, std::in_place_index<{1}>, &{0}}};\n'.format(map_name, index))
            header.write('    return std::nullopt;\n')
            header.write('}\n')
        else:
            # No per-repo maps in the JSON: nothing to resolve.
            header.write('using option_maps_variant = std::monostate;\n\n')
            header.write('inline std::optional<option_maps_variant> resolve_option_map(std::string_view repo)\n{\n')
            header.write('    (void) repo;\n')
            header.write('    return std::nullopt;\n')
            header.write('}\n')

        header.write("\n} // namespace detail\n")

    print("Generated {} (json={}, script={})".format(header_file, json_hash[:12], script_hash[:12]))


main()
