"""Run coding agents on fut tasks against two compilers, grade them with hidden tests, and count what they ran into.

  python3 tools/agent-eval/run.py --old ../fut-old --new . [--models sonnet,haiku] [--reps 3] [--jobs 6] [TASK ...]

--old/--new are fut checkouts with a built build/fut (each run sees that checkout's docs/spec.md as SPEC.md and its
std). TASK is a task id or a glob (default: every task). Each task is a project (projects/<P>) plus, in tasks/<id>/:
task.txt (the prompt), hidden.fut (tests appended to main.fut to grade), meta.json, and optionally bug.patch (a
planted bug, applied before the agent starts) and ref.patch (a correct solution, used by validate.py).

A run passes when every test passes with hidden.fut appended. Results go to --out (default build/agent-eval):
one folder per run (work/, stream.jsonl, result.json) and results.json; runs already there are reused, so an
interrupted run resumes. report.py summarizes them.
"""
import argparse, fnmatch, json, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
CLAUDE_MD = """This directory is a fut project. fut is a programming language; its full reference is SPEC.md (read it before writing fut).
The `fut` command is on PATH (`fut test main.fut` runs the tests).
"""
TOOLS = ['Bash(fut *)', 'Bash(ls *)', 'Bash(cat *)', 'Read', 'Edit', 'Write', 'Glob', 'Grep']
CHECKED = ('type', 'no-field', 'no-method', 'arity', 'effect')  # codes some compile-time check reports
COMPILE_ERR = re.compile(r'^\S+:\d+:\d+: error: (.*?)(?: \[([a-z-]+)\])?$', re.M)
RUNTIME_ERR = re.compile(r'runtime error: (.*?)(?: \[([a-z-]+)\])?$', re.M)


def compiler(root):
    root = os.path.abspath(root)
    if not os.access(f'{root}/build/fut', os.X_OK):
        sys.exit(f'{root}/build/fut not found: build it (python3 build.py) first')
    return root


def make_project(task, dest):  # the task's starting tree: its project, plus its planted bug if any
    meta = json.load(open(f'{HERE}/tasks/{task}/meta.json'))
    shutil.copytree(f"{HERE}/projects/{meta['project']}", dest, ignore=shutil.ignore_patterns('.fut-cache'))
    apply(f'{HERE}/tasks/{task}/bug.patch', dest)
    return meta


def apply(patch, dest):
    if os.path.exists(patch) and os.path.getsize(patch):
        subprocess.run(['patch', '-s', '-p1', '-d', dest, '-i', os.path.abspath(patch)], check=True)


def fut_env(root):
    return dict(os.environ, FUT_STD=f'{root}/std')


def grade(work, root, task):  # (passed, summary): every test, hidden ones included, passes
    with tempfile.TemporaryDirectory() as d:
        shutil.copytree(work, d, dirs_exist_ok=True, ignore=shutil.ignore_patterns('.fut-cache', 'SPEC.md', 'CLAUDE.md'))
        with open(f'{d}/main.fut', 'a') as f:
            f.write('\n' + open(f'{HERE}/tasks/{task}/hidden.fut').read())
        out = subprocess.run([f'{root}/build/fut', 'test', 'main.fut'], cwd=d, env=fut_env(root), capture_output=True, text=True, timeout=300)
        text = out.stdout + out.stderr
    summary = ([l for l in text.splitlines() if re.match(r'^\d+ tests?: ', l)] or [''])[-1]
    if summary:  # every test passed, and nothing else failed (a stale .api: `fut api` wasn't run)
        stale = next((l for l in text.splitlines() if l.startswith('API ')), '')
        return bool(re.match(r'^\d+ tests?: \d+ passed \(', summary)) and out.returncode == 0, summary + (f' | {stale}' if stale else '')
    first = next((l for l in text.splitlines() if 'error' in l), text.strip()[:200])
    return False, first


def latent(work, root):  # the compile errors the given (new) compiler finds in the final code: [(code, message)]
    out = subprocess.run([f'{root}/build/fut', 'check', 'main.fut'], cwd=work, env=fut_env(root), capture_output=True, text=True, timeout=300)
    return [(m.group(2) or '', m.group(1)) for m in COMPILE_ERR.finditer(out.stdout + out.stderr)]


def fut_outputs(events):  # (command, output) of each fut command the agent ran, in order
    cmds, outs = {}, []
    for e in events:
        m = e.get('message')
        if not isinstance(m, dict) or not isinstance(m.get('content'), list):
            continue
        for c in m['content']:
            if not isinstance(c, dict):
                continue
            if e.get('type') == 'assistant' and c.get('type') == 'tool_use' and c.get('name') == 'Bash' and 'fut' in c['input'].get('command', ''):
                cmds[c['id']] = c['input']['command']
            elif e.get('type') == 'user' and c.get('type') == 'tool_result' and c.get('tool_use_id') in cmds:
                t = c['content'] if isinstance(c['content'], str) else ' '.join(x.get('text', '') for x in c['content'] if isinstance(x, dict))
                outs.append((cmds[c['tool_use_id']], t))
    return outs


def errors_seen(outs):  # every distinct error the agent saw: {'compile': [(code, msg)], 'runtime': [...], 'failed_expects': n}
    seen = {'compile': [], 'runtime': [], 'failed_expects': 0}
    keys = set()
    for _, t in outs:
        for kind, rx in (('compile', COMPILE_ERR), ('runtime', RUNTIME_ERR)):
            for m in rx.finditer(t):
                k = (kind, m.group(1).strip())
                if k not in keys:
                    keys.add(k)
                    seen[kind].append((m.group(2) or '', m.group(1).strip()))
        seen['failed_expects'] += len(re.findall(r'^\s*FAIL\s', t, re.M))
    return seen


def run_one(a, task, comp, model, rep):
    root = a.compilers[comp]
    d = f'{a.out}/{task}-{comp}-{model}-{rep}'
    if os.path.exists(f'{d}/result.json'):
        return json.load(open(f'{d}/result.json'))
    w = f'{d}/work'
    stream = f'{d}/stream.jsonl'
    if not (os.path.exists(stream) and '"result"' in open(stream).read()):
        if os.path.exists(w):
            shutil.rmtree(w)
        os.makedirs(f'{d}/bin', exist_ok=True)
        make_project(task, w)
        shutil.copy(f'{root}/docs/spec.md', f'{w}/SPEC.md')
        open(f'{w}/CLAUDE.md', 'w').write(CLAUDE_MD)
        if not os.path.lexists(f'{d}/bin/fut'):
            os.symlink(f'{root}/build/fut', f'{d}/bin/fut')
        env = dict(fut_env(root), PATH=f'{d}/bin:' + os.environ['PATH'])
        prompt = open(f'{HERE}/tasks/{task}/task.txt').read().strip()
        cmd = ['claude', '-p', prompt, '--model', model, '--output-format', 'stream-json', '--verbose',
               '--setting-sources', 'project', '--strict-mcp-config', '--no-session-persistence',
               '--max-budget-usd', str(a.budget), '--allowedTools', *TOOLS]
        try:
            p = subprocess.run(cmd, cwd=w, env=env, capture_output=True, text=True, timeout=a.timeout)
            out, err = p.stdout, p.stderr
        except subprocess.TimeoutExpired as e:
            out = e.stdout.decode() if isinstance(e.stdout, bytes) else (e.stdout or '')
            err = 'timeout'
        open(stream, 'w').write(out)
        open(f'{d}/stderr.txt', 'w').write(err)
    events = []
    for l in open(stream).read().splitlines():
        try:
            events.append(json.loads(l))
        except ValueError:
            pass
    final = next((e for e in reversed(events) if e.get('type') == 'result'), {})
    outs = fut_outputs(events)
    passed, summary = grade(w, root, task)
    meta = json.load(open(f'{HERE}/tasks/{task}/meta.json'))
    u = final.get('usage') or {}
    r = {'task': task, 'compiler': comp, 'model': model, 'rep': rep, 'kind': meta.get('kind', 'planted'),
         'bug': meta.get('bug'), 'path': meta.get('path'), 'project': meta['project'],
         'passed': passed, 'grade': summary,
         'turns': final.get('num_turns') or 0, 'cost': final.get('total_cost_usd') or 0,
         'seconds': round((final.get('duration_ms') or 0) / 1000, 1),
         'out_tokens': u.get('output_tokens') or 0,
         'fut_runs': len(outs), 'seen': errors_seen(outs),
         'first_fut': errors_seen(outs[:1]),
         'latent': latent(w, a.compilers['new']),
         'error': (final.get('subtype') if final.get('is_error') else None) if final else 'no result'}
    json.dump(r, open(f'{d}/result.json', 'w'), indent=1)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--old', required=True, help='fut checkout without the change under test (built)')
    ap.add_argument('--new', required=True, help='fut checkout with it (built)')
    ap.add_argument('--models', default='sonnet')
    ap.add_argument('--reps', type=int, default=1)
    ap.add_argument('--sides', default='old,new', help='which compilers to run (new alone: compare with an earlier run)')
    ap.add_argument('--jobs', type=int, default=6)
    ap.add_argument('--budget', type=float, default=3, help='max dollars per run')
    ap.add_argument('--timeout', type=int, default=1800, help='max seconds per run')
    ap.add_argument('--out', default=os.path.join(HERE, '..', '..', 'build', 'agent-eval'))
    ap.add_argument('tasks', nargs='*')
    a = ap.parse_args()
    a.compilers = {'old': compiler(a.old), 'new': compiler(a.new)}
    a.out = os.path.abspath(a.out)
    all_tasks = sorted(t for t in os.listdir(f'{HERE}/tasks') if os.path.isdir(f'{HERE}/tasks/{t}'))
    tasks = [t for t in all_tasks if not a.tasks or any(fnmatch.fnmatch(t, p) for p in a.tasks)]
    if not tasks:
        sys.exit('no task matches')
    jobs = [(t, c, m, r) for r in range(a.reps) for m in a.models.split(',') for t in tasks for c in a.sides.split(',')]

    def safe(j):
        try:
            return run_one(a, *j)
        except Exception as e:
            print('run failed', j, repr(e), flush=True)

    with ThreadPoolExecutor(a.jobs) as ex:
        results = [r for r in ex.map(safe, jobs) if r]
    os.makedirs(a.out, exist_ok=True)
    every = {}
    if os.path.exists(f'{a.out}/results.json'):
        every = {(r['task'], r['compiler'], r['model'], r['rep']): r for r in json.load(open(f'{a.out}/results.json'))}
    every.update({(r['task'], r['compiler'], r['model'], r['rep']): r for r in results})
    json.dump(sorted(every.values(), key=lambda r: (r['task'], r['model'], r['compiler'], r['rep'])), open(f'{a.out}/results.json', 'w'), indent=1)
    print(f'{len(results)} runs; python3 {os.path.relpath(HERE)}/report.py {os.path.relpath(a.out)}')


if __name__ == '__main__':
    main()
