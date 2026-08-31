#! /usr/bin/env python3
"""// Copyright (C) 2022-2025 Vladislav Nepogodin
//
// This file is part of kernel-manager.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
"""

from sys import argv
import os
import json

if len(argv) != 3:
    print('Wrong number of arguments! Please specify header directory.')
    exit(1)

HEADER_ROOT = argv[1]
SOURCE_ROOT = argv[2]
SCRIPT_MTIME = os.stat(__file__).st_mtime

def needs_run():
    try:
        for filename in ['compile_options.hpp']:
            st = os.stat(HEADER_ROOT + '/' + filename)
            if st.st_mtime < SCRIPT_MTIME:
                return True
    except FileNotFoundError:
        return True
    # The per-repo option maps live in compile_options.json: a JSON newer
    # than the generated header means the header is stale, so regenerate.
    try:
        json_mtime = os.stat(SOURCE_ROOT + '/src/compile_options.json').st_mtime
        header_mtime = os.stat(HEADER_ROOT + '/compile_options.hpp').st_mtime
        if header_mtime < json_mtime:
            return True
    except FileNotFoundError:
        return True
    return False

def map_type(values_count):
    return 'frozen::unordered_map<frozen::string, std::string_view, {}>'.format(values_count)

def main():
    if needs_run():
        header_file = HEADER_ROOT + '/compile_options.hpp'
        header = open(header_file, 'w', encoding='utf-8', newline='\u000A')

        compile_options = {}
        with open(SOURCE_ROOT + '/src/compile_options.json', 'r') as f:
            compile_options = json.load(f)

        header.write(globals()['__doc__'])
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
        header.close()
    else:
        print("Output files up-to-date.")

main()
