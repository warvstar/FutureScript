"""Summarize an agent-eval results folder (run.py --out): pass rates, cost, and the errors agents ran into.

  python3 tools/agent-eval/report.py [build/agent-eval] [--kind feature|planted] [--by-task]
"""
import argparse, collections, json, os, re

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKED = ('type', 'no-field', 'no-method', 'arity', 'effect')


def mean(xs):
    xs = list(xs)
    return sum(xs) / len(xs) if xs else 0


def shape(msg):  # an error message with its specifics blanked, to group the same mistake
    msg = re.sub(r'"[^"]*"', '"…"', msg)
    msg = re.sub(r"'[^']*'", "'…'", msg)
    msg = re.sub(r'\([^)]*\)', '(…)', msg)
    return re.sub(r'\d+', 'N', msg)[:110]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out', nargs='?', default=os.path.join(HERE, '..', '..', 'build', 'agent-eval'))
    ap.add_argument('--kind')
    ap.add_argument('--by-task', action='store_true')
    a = ap.parse_args()
    R = json.load(open(os.path.join(a.out, 'results.json')))
    if a.kind:
        R = [r for r in R if r['kind'] == a.kind]
    groups = collections.defaultdict(list)
    for r in R:
        groups[(r['kind'], r['model'], r['compiler'])].append(r)

    print('| kind | model | compiler | runs | passed | turns | cost | secs | own errors: compile (checked) | runtime (checked) | failed expects | left in final code |')
    print('|---|---|---|---|---|---|---|---|---|---|---|---|')
    for (kind, model, comp), rs in sorted(groups.items()):
        ce = [e for r in rs for e in r['seen']['compile']]
        re_ = [e for r in rs for e in r['seen']['runtime']]
        lat = [e for r in rs for e in r['latent']]
        print(f"| {kind} | {model} | {comp} | {len(rs)} | {sum(r['passed'] for r in rs)} | {mean(r['turns'] for r in rs):.1f} | "
              f"${mean(r['cost'] for r in rs):.3f} | {mean(r['seconds'] for r in rs):.0f} | "
              f"{len(ce)} ({sum(c in CHECKED for c, _ in ce)}) | {len(re_)} ({sum(c in CHECKED for c, _ in re_)}) | "
              f"{sum(r['seen']['failed_expects'] for r in rs)} | {len(lat)} |")
    print(f"\ntotal cost ${sum(r['cost'] for r in R):.2f} over {len(R)} runs")

    # Which runs met at least one error a compile-time check reports, and where they met it.
    print('\nRuns that met an error of a checked kind (' + ', '.join(CHECKED) + '):')
    for (kind, model, comp), rs in sorted(groups.items()):
        hit_c = sum(any(c in CHECKED for c, _ in r['seen']['compile']) for r in rs)
        hit_r = sum(any(c in CHECKED for c, _ in r['seen']['runtime']) for r in rs)
        print(f'  {kind:8s} {model:7s} {comp:4s}: at compile time in {hit_c}/{len(rs)}, at run time in {hit_r}/{len(rs)}')

    seen = collections.Counter()
    for r in R:
        for where in ('compile', 'runtime'):
            for code, msg in r['seen'][where]:
                seen[(r['model'], r['compiler'], where, code, shape(msg))] += 1
    print('\nMost common errors agents ran into (model, compiler, when, code: message):')
    for (model, comp, where, code, msg), n in seen.most_common(25):
        print(f'  {n:3d}  {model} {comp} {where:7s} [{code}] {msg}')

    if a.by_task:
        print('\n| task | model | old passed | new passed | old turns | new turns |')
        print('|---|---|---|---|---|---|')
        by = collections.defaultdict(list)
        for r in R:
            by[(r['task'], r['model'], r['compiler'])].append(r)
        for task, model in sorted({(r['task'], r['model']) for r in R}):
            o, n = by[(task, model, 'old')], by[(task, model, 'new')]
            print(f"| {task} | {model} | {sum(r['passed'] for r in o)}/{len(o)} | {sum(r['passed'] for r in n)}/{len(n)} | "
                  f"{mean(r['turns'] for r in o):.1f} | {mean(r['turns'] for r in n):.1f} |")


if __name__ == '__main__':
    main()
