#
# Copyright (C) 2020 Collabora, Ltd.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice (including the next
# paragraph) shall be included in all copies or substantial portions of the
# Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

# Useful for autogeneration
COPYRIGHT = """/*
 * Copyright (C) 2020 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* Autogenerated file, do not edit */

"""

# Parse instruction set XML into a normalized form for processing

import xml.etree.ElementTree as ET
import copy
import itertools
from collections import OrderedDict

def parse_cond(cond, aliased = False):
    if cond.tag == 'reserved':
        return None
    
    if cond.attrib.get('alias', False) and not aliased:
        return ['alias', parse_cond(cond, True)]

    if 'left' in cond.attrib:
        return [cond.tag, cond.attrib['left'], cond.attrib['right']]
    else:
        return [cond.tag] + [parse_cond(x) for x in cond.findall('*')]

def parse_exact(obj):
    return [int(obj.attrib['mask'], 0), int(obj.attrib['exact'], 0)]

def parse_derived(obj):
    out = []

    for deriv in obj.findall('derived'):
        loc = [int(deriv.attrib['start']), int(deriv.attrib['size'])]
        count = 1 << loc[1]

        opts = [parse_cond(d) for d in deriv.findall('*')]
        default = [None] * count
        opts_fit = (opts + default)[0:count]

        out.append([loc, opts_fit])

    return out

def parse_modifiers(obj, include_pseudo):
    out = []

    for mod in obj.findall('mod'):
        if mod.attrib.get('pseudo', False) and not include_pseudo:
            continue

        name = mod.attrib['name']
        start = mod.attrib.get('start', None)
        size = int(mod.attrib['size'])

        if start is not None:
            start = int(start)

        opts = [x.text if x.tag == 'opt' else x.tag for x in mod.findall('*')]

        if len(opts) == 0:
            assert('opt' in mod.attrib)
            opts = ['none', mod.attrib['opt']]

        # Find suitable default
        default = mod.attrib.get('default', 'none' if 'none' in opts else None)

        # Pad out as reserved
        count = (1 << size)
        opts = (opts + (['reserved'] * count))[0:count]
        out.append([[name, start, size], default, opts])

    return out

def parse_copy(enc, existing):
    for node in enc.findall('copy'):
        name = node.get('name')
        for ex in existing:
            if ex[0][0] == name:
                ex[0][1] = node.get('start')

def parse_instruction(ins, include_pseudo):
    common = {
            'srcs': [],
            'modifiers': [],
            'immediates': [],
            'swaps': [],
            'derived': [],
            'staging': ins.attrib.get('staging', '').split('=')[0],
            'staging_count': ins.attrib.get('staging', '=0').split('=')[1],
            'dests': int(ins.attrib.get('dests', '1')),
            'unused': ins.attrib.get('unused', False),
            'pseudo': ins.attrib.get('pseudo', False),
            'message': ins.attrib.get('message', 'none'),
            'last': ins.attrib.get('last', False),
            'table': ins.attrib.get('table', False),
    }

    if 'exact' in ins.attrib:
        common['exact'] = parse_exact(ins)

    for src in ins.findall('src'):
        mask = int(src.attrib['mask'], 0) if ('mask' in src.attrib) else 0xFF
        common['srcs'].append([int(src.attrib['start'], 0), mask])

    for imm in ins.findall('immediate'):
        if imm.attrib.get('pseudo', False) and not include_pseudo:
            continue

        start = int(imm.attrib['start']) if 'start' in imm.attrib else None
        common['immediates'].append([imm.attrib['name'], start, int(imm.attrib['size'])])

    common['derived'] = parse_derived(ins)
    common['modifiers'] = parse_modifiers(ins, include_pseudo)

    for swap in ins.findall('swap'):
        lr = [int(swap.get('left')), int(swap.get('right'))]
        cond = parse_cond(swap.findall('*')[0])
        rewrites = {}

        for rw in swap.findall('rewrite'):
            mp = {}

            for m in rw.findall('map'):
                mp[m.attrib['from']] = m.attrib['to']

            rewrites[rw.attrib['name']] = mp

        common['swaps'].append([lr, cond, rewrites])

    encodings = ins.findall('encoding')
    variants = []

    if len(encodings) == 0:
        variants = [[None, common]]
    else:
        for enc in encodings:
            variant = copy.deepcopy(common)
            assert(len(variant['derived']) == 0)

            variant['exact'] = parse_exact(enc)
            variant['derived'] = parse_derived(enc)
            parse_copy(enc, variant['modifiers'])

            cond = parse_cond(enc.findall('*')[0])
            variants.append([cond, variant])

    return variants

def parse_instructions(xml, include_unused = False, include_pseudo = False):
    final = {}
    instructions = ET.parse(xml).getroot().findall('ins')

    for ins in instructions:
        parsed = parse_instruction(ins, include_pseudo)

        # Some instructions are for useful disassembly only and can be stripped
        # out of the compiler, particularly useful for release builds
        if parsed[0][1]["unused"] and not include_unused:
            continue

        # On the other hand, some instructions are only for the IR, not disassembly
        if parsed[0][1]["pseudo"] and not include_pseudo:
            continue

        final[ins.attrib['name']] = parsed

    return final

# Expand out an opcode name to something C-escaped

def opname_to_c(name):
    return name.lower().replace('*', 'fma_').replace('+', 'add_').replace('.', '_')

# Expand out distinct states to distrinct instructions, with a placeholder
# condition for instructions with a single state

def expand_states(instructions):
    out = {}

    for ins in instructions:
        c = instructions[ins]

        for ((test, desc), i) in zip(c, range(len(c))):
            # Construct a name for the state
            name = ins + (('.' + str(i)) if len(c) > 1 else '')

            out[name] = (ins, test if test is not None else [], desc)

    return out

# Drop keys used for packing to simplify IR representation, so we can check for
# equivalence easier

def simplify_to_ir(ins):
    return {
            'staging': ins['staging'],
            'srcs': len(ins['srcs']),
            'dests': ins['dests'],
            'modifiers': [[m[0][0], m[2]] for m in ins['modifiers']],
            'immediates': [m[0] for m in ins['immediates']]
        }


def combine_ir_variants(instructions, key):
    seen = [op for op in instructions.keys() if op[1:] == key]
    variant_objs = [[simplify_to_ir(Q[1]) for Q in instructions[x]] for x in seen]
    variants = sum(variant_objs, [])

    # Accumulate modifiers across variants
    modifiers = {}

    for s in variants[0:]:
        # Check consistency
        assert(s['srcs'] == variants[0]['srcs'])
        assert(s['dests'] == variants[0]['dests'])
        assert(s['immediates'] == variants[0]['immediates'])
        assert(s['staging'] == variants[0]['staging'])

        for name, opts in s['modifiers']:
            if name not in modifiers:
                modifiers[name] = copy.deepcopy(opts)
            else:
                modifiers[name] += opts

    # Great, we've checked srcs/immediates are consistent and we've summed over
    # modifiers
    return {
            'srcs': variants[0]['srcs'],
            'dests': variants[0]['dests'],
            'staging': variants[0]['staging'],
            'immediates': sorted(variants[0]['immediates']),
            'modifiers': modifiers,
            'v': len(variants),
            'ir': variants
        }

# Partition instructions to mnemonics, considering units and variants
# equivalent.

def partition_mnemonics(instructions):
    key_func = lambda x: x[1:]
    sorted_instrs = sorted(instructions.keys(), key = key_func)
    partitions = itertools.groupby(sorted_instrs, key_func)
    return { k: combine_ir_variants(instructions, k) for k, v in partitions }

# Generate modifier lists, by accumulating all the possible modifiers, and
# deduplicating thus assigning canonical enum values. We don't try _too_ hard
# to be clever, but by preserving as much of the original orderings as
# possible, later instruction encoding is simplified a bit.  Probably a micro
# optimization but we have to pick _some_ ordering, might as well choose the
# most convenient.
#
# THIS MUST BE DETERMINISTIC

def order_modifiers(ir_instructions):
    out = {}

    # modifier name -> (list of option strings)
    modifier_lists = {}

    for ins in sorted(ir_instructions):
        modifiers = ir_instructions[ins]["modifiers"]

        for name in modifiers:
            name_ = name[0:-1] if name[-1] in "0123" else name

            if name_ not in modifier_lists:
                modifier_lists[name_] = copy.deepcopy(modifiers[name])
            else:
                modifier_lists[name_] += modifiers[name]

    for mod in modifier_lists:
        lst = list(OrderedDict.fromkeys(modifier_lists[mod]))

        # Ensure none is false for booleans so the builder makes sense
        if len(lst) == 2 and lst[1] == "none":
            lst.reverse()
        elif mod == "table":
            # We really need a zero sentinel to materialize DTSEL
            assert(lst[2] == "none")
            lst[2] = lst[0]
            lst[0] = "none"

        out[mod] = lst

    return out

# Count sources for a simplified (IR) instruction, including a source for a
# staging register if necessary
def src_count(op):
    staging = 1 if (op["staging"] in ["r", "rw"]) else 0
    return op["srcs"] + staging
