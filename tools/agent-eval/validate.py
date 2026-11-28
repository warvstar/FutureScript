"""Check that agent-eval tasks are sound: see AUTHORING.md.

  python3 tools/agent-eval/validate.py --old ../fut-old --new . [TASK ...]
"""
import argparse, fnmatch, json, os, subprocess, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from run import HERE, apply, compiler, fut_env, grade, make_project  # noqa: E402


def own_tests(tree, root):  # (ok, summary) for the tree's own tests
    out = subprocess.run([f'{root}/build/fut', 'test', 'main.fut'], cwd=tree, env=fut_env(root), capture_output=True, text=True, timeout=300)
    text = out.stdout + out.stderr
    lines = [l for l in text.splitlines() if l.strip()]
    return out.returncode == 0, (lines or [''])[-1][:150]


def check(task, compilers):
    problems = []
    meta = json.load(open(f'{HERE}/tasks/{task}/meta.json'))
    for name, root in compilers.items():
        with tempfile.TemporaryDirectory() as d:
            start = f'{d}/start'
            make_project(task, start)
            if meta.get('kind', 'planted') == 'planted' and (meta.get('contract') or name == 'new' and meta.get('bug') != 'logic'):
                pass  # the new compiler may reject a planted bug at compile time, a contract's property test may catch it: that is the point
            else:
                ok, s = own_tests(start, root)
                if not ok:
                    problems.append(f'{name}: the starting tree\'s tests fail: {s}')
            passed, s = grade(start, root, task)
            if passed:
                problems.append(f'{name}: the starting tree passes the hidden tests')
            solved = f'{d}/solved'
            make_project(task, solved)
            if not os.path.exists(f'{HERE}/tasks/{task}/ref.patch'):
                problems.append('no ref.patch')
                continue
            if os.path.exists(f'{HERE}/tasks/{task}/bug.patch'):  # ref.patch is from the clean project
                subprocess.run(['rm', '-rf', solved], check=True)
                os.makedirs(solved)
                subprocess.run(['cp', '-R', f"{HERE}/projects/{meta['project']}/.", solved], check=True)
            try:
                apply(f'{HERE}/tasks/{task}/ref.patch', solved)
            except subprocess.CalledProcessError:
                problems.append('ref.patch does not apply')
                continue
            passed, s = grade(solved, root, task)
            if not passed:
                problems.append(f'{name}: the solved tree fails: {s}')
            if os.path.exists(f'{HERE}/tasks/{task}/bug.patch'):
                partial = f'{d}/partial'
                subprocess.run(['cp', '-R', solved, partial], check=True)
                apply(f'{HERE}/tasks/{task}/bug.patch', partial)
                passed, s = grade(partial, root, task)
                if passed:
                    problems.append(f'{name}: the feature without the fix passes')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--old', required=True)
    ap.add_argument('--new', required=True)
    ap.add_argument('tasks', nargs='*')
    a = ap.parse_args()
    compilers = {'old': compiler(a.old), 'new': compiler(a.new)}
    all_tasks = sorted(t for t in os.listdir(f'{HERE}/tasks') if os.path.isdir(f'{HERE}/tasks/{t}'))
    bad = 0
    for t in [t for t in all_tasks if not a.tasks or any(fnmatch.fnmatch(t, p) for p in a.tasks)]:
        problems = check(t, compilers)
        bad += bool(problems)
        print(f"{'ok  ' if not problems else 'FAIL'} {t}" + ''.join(f'\n       {p}' for p in problems), flush=True)
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
