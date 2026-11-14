#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Generate the userspace kernel headers for Android (Soong) builds.

Produces the same tree as "make ARCH=<arch> headers_install" (usr/include),
including the uapi of the techpack vendor drivers, without running the kernel
build: every header goes through scripts/headers_install.sh as in
scripts/Makefile.headersinst, the asm-generic wrappers follow
scripts/Makefile.asm-generic, arm's unistd-{oabi,eabi}.h come from
scripts/syscallhdr.sh and linux/version.h from the top-level Makefile.

headers_install.sh renames struct sigaction to struct __kernel_sigaction and
__unused to __linux_unused, as bionic does for its kernel headers, so the
headers can be used next to bionic's <signal.h> and <sys/cdefs.h>.

  --bp              print gen_headers_<arch>.bp (the Soong genrule and its
                    output list); rerun when headers are added or removed
  --gen-dir DIR     write the headers; with --outs, fail unless the generated
                    set is exactly the declared Soong outputs
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Directories whose include/uapi the top-level Makefile exports (ext-mod-dirs).
TECHPACK_UAPI = [
    'techpack/audio/audio-kernel-elite',
    'techpack/camera',
    'techpack/cvp',
    'techpack/display',
    'techpack/kgsl/graphics-kernel',
    'techpack/video',
]


def kbuild_vars(path):
    """Collect 'var += a b' assignments of a simple Kbuild file."""
    out = {}
    if not os.path.exists(path):
        return out
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*([\w-]+)\s*\+?=\s*(.*)', line)
            if m:
                out.setdefault(m.group(1), []).extend(m.group(2).split())
    return out


def no_export_headers(src, arch, generated):
    """Evaluate include/uapi/Kbuild (wildcard conditions on the arch uapi)."""
    path = os.path.join(src, 'include/uapi/Kbuild')
    result, stack = [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            m = re.match(r'ifeq \(\$\(wildcard (\S+)\),\)', line)
            if m:
                p = m.group(1).replace('$(SRCARCH)', arch)
                if p.startswith('$(objtree)/'):
                    gen = p[len('$(objtree)/arch/%s/include/generated/uapi/' % arch):]
                    exists = gen in generated
                else:
                    exists = os.path.exists(os.path.join(src, p.replace('$(srctree)/', '')))
                stack.append(not exists)
            elif line == 'endif':
                stack.pop()
            elif line.startswith('no-export-headers') and all(stack):
                result.extend(line.split('+=', 1)[1].split())
    return set(result)


def subdir_headers(root):
    """Headers below the subdirectories of root, as Makefile.headersinst."""
    found = []
    if not os.path.isdir(root):
        return found
    for sub in sorted(os.listdir(root)):
        d = os.path.join(root, sub)
        if not os.path.isdir(d):
            continue
        for dirpath, _, files in os.walk(d, followlinks=True):
            for name in files:
                if name.endswith('.h'):
                    found.append(os.path.relpath(os.path.join(dirpath, name), root))
    return found


def plan(src, arch):
    """Return {output path: (kind, source)} in install order."""
    out = {}
    asm_uapi = os.path.join(src, 'arch', arch, 'include/uapi/asm')
    akb = kbuild_vars(os.path.join(asm_uapi, 'Kbuild'))
    mandatory = kbuild_vars(os.path.join(src, 'include/uapi/asm-generic/Kbuild')).get('mandatory-y', [])
    generated_y = akb.get('generated-y', [])
    generic = list(akb.get('generic-y', []))
    generic += [h for h in mandatory if h not in generated_y
                and not os.path.exists(os.path.join(asm_uapi, h))]
    generated = {'asm/' + h for h in generic + generated_y}

    noexp = no_export_headers(src, arch, generated)
    for h in subdir_headers(os.path.join(src, 'include/uapi')):
        if h not in noexp:
            out[h] = ('install', os.path.join('include/uapi', h))
    out['linux/version.h'] = ('version', None)
    for h in subdir_headers(os.path.join(src, 'arch', arch, 'include/uapi')):
        out[h] = ('install', os.path.join('arch', arch, 'include/uapi', h))
    for h in sorted(set(generic)):
        if 'asm/' + h not in out:
            out['asm/' + h] = ('wrap', h)
    for h in generated_y:
        if 'asm/' + h not in out:
            out['asm/' + h] = ('syscall', re.match(r'unistd-(\w+)\.h', h).group(1))
    for d in TECHPACK_UAPI:
        for h in subdir_headers(os.path.join(src, d, 'include/uapi')):
            out[h] = ('install', os.path.join(d, 'include/uapi', h))
    return out


def version_h(src):
    v = {}
    with open(os.path.join(src, 'Makefile')) as f:
        for line in f:
            m = re.match(r'(VERSION|PATCHLEVEL|SUBLEVEL) = (\d+)', line)
            if m:
                v[m.group(1)] = int(m.group(2))
            if len(v) == 3:
                break
    code = v['VERSION'] * 65536 + v['PATCHLEVEL'] * 256 + min(v['SUBLEVEL'], 255)
    return ('#define LINUX_VERSION_CODE %d\n'
            '#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + ((c) > 255 ? 255 : (c)))\n'
            '#define LINUX_VERSION_MAJOR %d\n'
            '#define LINUX_VERSION_PATCHLEVEL %d\n'
            '#define LINUX_VERSION_SUBLEVEL %d\n'
            % (code, v['VERSION'], v['PATCHLEVEL'], v['SUBLEVEL']))


def generate(src, arch, gen_dir, unifdef, outs):
    src = os.path.abspath(src)
    entries = plan(src, arch)
    if outs is not None:
        declared = {os.path.relpath(o, gen_dir) for o in outs}
        if declared != set(entries):
            extra = sorted(set(entries) - declared)
            missing = sorted(declared - set(entries))
            sys.exit('gen_uapi_headers: header set changed for %s (new: %s, gone: %s); '
                     'regenerate gen_headers_%s.bp with --bp' % (arch, extra, missing, arch))

    work = tempfile.mkdtemp()
    try:
        # headers_install.sh runs scripts/unifdef relative to its cwd.
        os.makedirs(os.path.join(work, 'scripts'))
        os.symlink(os.path.abspath(unifdef), os.path.join(work, 'scripts/unifdef'))
        hi = os.path.join(src, 'scripts/headers_install.sh')
        tbl = os.path.join(src, 'arch', arch, 'tools/syscall.tbl')
        for rel, (kind, arg) in entries.items():
            dst = os.path.abspath(os.path.join(gen_dir, rel))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if kind == 'version':
                with open(dst, 'w') as f:
                    f.write(version_h(src))
                continue
            # syscallhdr.sh derives the include guard from the file name.
            infile = (os.path.join(src, arg) if kind == 'install'
                      else os.path.join(work, os.path.basename(rel)))
            if kind == 'wrap':
                with open(infile, 'w') as f:
                    f.write('#include <asm-generic/%s>\n' % arg)
            elif kind == 'syscall':
                subprocess.check_call(['sh', os.path.join(src, 'scripts/syscallhdr.sh'),
                                       '--abis', 'common,' + arg,
                                       '--offset', '__NR_SYSCALL_BASE', tbl, infile],
                                      cwd=work)
            subprocess.check_call(['sh', hi, infile, dst], cwd=work)
    finally:
        shutil.rmtree(work)


BP = """// Generated by scripts/gen_uapi_headers.py --arch {arch} --bp; do not edit.
// Rerun it when uapi headers are added or removed.

genrule {{
    name: "timelm_uapi_headers_{arch}",
    tools: ["timelm_unifdef"],
    tool_files: ["scripts/gen_uapi_headers.py"],
    srcs: [
{srcs}    ],
    cmd: "python3 $(location scripts/gen_uapi_headers.py) --arch {arch} " +
        "--gen-dir $(genDir) --unifdef $(location timelm_unifdef) " +
        "--in $(in) --outs $(out)",
    out: [
{outs}    ],
}}
"""


def bp(src, arch):
    # The tree goes in srcs (tool_files land elsewhere in the sandbox).
    srcs = ['Makefile', 'scripts/headers_install.sh', 'scripts/syscallhdr.sh',
            'include/uapi/Kbuild', 'include/uapi/asm-generic/Kbuild',
            'include/uapi/**/*.h', 'arch/%s/include/uapi/**/*' % arch]
    if arch == 'arm':
        srcs.append('arch/arm/tools/syscall.tbl')
    srcs += ['%s/include/uapi/**/*.h' % d for d in TECHPACK_UAPI]
    return BP.format(arch=arch,
                     srcs=''.join('        "%s",\n' % x for x in srcs),
                     outs=''.join('        "%s",\n' % x for x in sorted(plan(src, arch))))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', required=True, choices=['arm', 'arm64'])
    ap.add_argument('--src', help='kernel source tree (default: this tree)')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--bp', action='store_true')
    ap.add_argument('--gen-dir')
    ap.add_argument('--unifdef')
    ap.add_argument('--in', dest='inputs', nargs='*',
                    help='the genrule inputs; the tree is where include/uapi/Kbuild is')
    ap.add_argument('--outs', nargs='*')
    a = ap.parse_args()
    if a.src is None and a.inputs:
        kb = [i for i in a.inputs if i.endswith('/include/uapi/Kbuild')
              or i == 'include/uapi/Kbuild']
        a.src = kb[0][:-len('include/uapi/Kbuild')] or '.'
    if a.src is None:
        a.src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    if a.list:
        for rel in sorted(plan(os.path.abspath(a.src), a.arch)):
            print(rel)
        return
    if a.bp:
        sys.stdout.write(bp(os.path.abspath(a.src), a.arch))
        return
    if not a.gen_dir or not a.unifdef:
        ap.error('--gen-dir and --unifdef are required')
    generate(a.src, a.arch, a.gen_dir, a.unifdef, a.outs)


if __name__ == '__main__':
    main()
