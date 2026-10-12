#!/usr/bin/env python3
"""check_opts2.py — строгая проверка применённости wildkernels-патчей.

Для каждого патча берём стопроцентно ДОБАВЛЯЕМЫЕ строки (в added/removed
парах — это правка, а не контекст) и ищем их в дереве. Если конкретные
добавляемые строки присутствуют — патч применён.
"""
import os
import re
import subprocess

ROOT = '/data/kleaf_ws/common'
PDIR = 'apex-patches/optimizations'


def added_lines(patch_path, min_len=12):
    """Строки, которые патч добавляет, и их файл."""
    out = []
    cur = None
    for raw in open(patch_path, encoding='utf-8', errors='replace'):
        l = raw.rstrip('\n')
        if l.startswith('diff --git'):
            m = re.match(r'diff --git a/(\S+) b/(\S+)', l)
            cur = m.group(2) if m else None
        elif l.startswith('+') and not l.startswith('+++'):
            body = l[1:].strip()
            # отсекаем контекстные/пустые и чисто комментарии-шапки
            if len(body) >= min_len and not body.startswith(('*', '//', '#')):
                out.append((cur, body))
    return out


def grep_hit(path, needle):
    f = os.path.join(ROOT, path)
    if not os.path.exists(f):
        return None
    try:
        r = subprocess.run(['grep', '-F', '-c', needle, f],
                           capture_output=True, text=True)
        return int(r.stdout.strip() or 0) > 0
    except Exception:
        return None


def main():
    rows = []
    for fn in sorted(os.listdir(PDIR)):
        if not fn.endswith('.patch'):
            continue
        path = os.path.join(PDIR, fn)
        adds = added_lines(path)
        if not adds:
            rows.append((fn, 'no-added-lines', [], ''))
            continue
        # берём до 4 репрезентативных добавленных строк из разных файлов
        seen, probes = set(), []
        for f, body in adds:
            if f and f not in seen and len(probes) < 4:
                seen.add(f)
                probes.append((f, body))
        found = 0
        total = 0
        unknown = 0
        for f, body in probes:
            total += 1
            hit = grep_hit(f, body)
            if hit is None:
                unknown += 1
            elif hit:
                found += 1
        if unknown == total:
            state = 'unknown'
        elif found == total - unknown and total > unknown:
            state = 'APPLIED'
        elif found:
            state = 'PARTIAL (%d/%d)' % (found, total - unknown)
        else:
            state = 'NOT-APPLIED'
        rows.append((fn, state, probes, ''))

    print('%-46s %-16s %s' % ('PATCH', 'STATE', 'checked'))
    print('-' * 96)
    for fn, state, probes, _ in rows:
        print('%-46s %-16s' % (fn[:46], state))
        for f, body in probes[:2]:
            print('%48s%s' % ('', '  .' + f + ':'))
            print('%48s%s' % ('', '    ' + body[:70]))
    print('-' * 96)
    n_ok = sum(1 for r in rows if r[1] == 'APPLIED')
    n_no = sum(1 for r in rows if r[1] == 'NOT-APPLIED')
    print('APPLIED %d, NOT-APPLIED %d, unknown/partial %d'
          % (n_ok, n_no, len(rows) - n_ok - n_no))


if __name__ == '__main__':
    main()
