#!/usr/bin/env python3
"""Generates fsql's hidden acceptance tests (accept/*_test.fut) from the cases below, with the expected results taken
from SQLite, whose semantics match the fsql spec on everything these cases touch. Run: python3 swarm/targets/fsql/gen.py
The swarm never sees these files; swarm.fut runs them against main to score a run."""
import os, sqlite3

HERE = os.path.dirname(os.path.abspath(__file__))

T = "CREATE TABLE t (id INT PRIMARY KEY, name TEXT NOT NULL, age INT, score REAL);"
TD = T + " INSERT INTO t VALUES (1, 'ann', 30, 1.5), (2, 'bob', 25, 2.5), (3, 'cy', NULL, 4.0), (4, 'dee', 41, NULL);"
J = ("CREATE TABLE u (id INT PRIMARY KEY, name TEXT); CREATE TABLE o (id INT PRIMARY KEY, uid INT, amt INT);"
     " INSERT INTO u VALUES (1, 'ann'), (2, 'bob'), (3, 'cy');"
     " INSERT INTO o VALUES (10, 1, 5), (11, 1, 7), (12, 2, 3), (13, 9, 1);")
BIG = "CREATE TABLE k (id INT PRIMARY KEY, v INT); INSERT INTO k VALUES " + ", ".join(
    f"({(i * 7919) % 1000}, {i})" for i in range(1000)) + ";"

CASES = {
    "basics": [
        ("create and select empty", "CREATE TABLE t (a INT); SELECT a FROM t;"),
        ("insert counts rows", T + " INSERT INTO t VALUES (1, 'a', 2, 0.5), (2, 'b', 3, 1.5);"),
        ("select star", TD + " SELECT * FROM t ORDER BY id;"),
        ("select columns", TD + " SELECT name, id FROM t ORDER BY id;"),
        ("insert with column list fills NULL", T + " INSERT INTO t (id, name) VALUES (7, 'x'); SELECT id, name, age, score FROM t;"),
        ("keywords are case-insensitive", "create table t (a int); insert into t values (1); SeLeCt a FrOm t;"),
        ("quoted string escape", "CREATE TABLE t (s TEXT); INSERT INTO t VALUES ('it''s'); SELECT s FROM t;"),
        ("update counts and changes", TD + " UPDATE t SET age = age + 1 WHERE age > 26; SELECT id, age FROM t ORDER BY id;"),
        ("delete counts and removes", TD + " DELETE FROM t WHERE id >= 3; SELECT id FROM t ORDER BY id;"),
        ("delete all", TD + " DELETE FROM t; SELECT id FROM t;"),
        ("drop table", TD + " DROP TABLE t; SELECT id FROM t;"),
        ("unknown table is an error", "SELECT a FROM nope;"),
        ("unknown column is an error", TD + " SELECT nope FROM t;"),
        ("syntax error", "SELEC 1;"),
        ("statements after an error still run", "CREATE TABLE t (a INT); SELEC; INSERT INTO t VALUES (5); SELECT a FROM t;"),
        ("create existing table is an error", "CREATE TABLE t (a INT); CREATE TABLE t (b INT);"),
        ("real values", "CREATE TABLE t (x REAL); INSERT INTO t VALUES (1.25), (2.5); SELECT x FROM t ORDER BY x;"),
    ],
    "expressions": [
        ("arithmetic and alias", "SELECT 1 + 2 * 3 AS a, (1 + 2) * 3 AS b, 10 - 4 - 3 AS c;"),
        ("integer division truncates", "SELECT 7 / 2 AS a, -7 / 2 AS b, 7 % 3 AS c;"),
        ("division by zero is NULL", "SELECT 1 / 0 AS a;"),
        ("comparisons give 1 and 0", "SELECT 1 < 2 AS a, 2 < 1 AS b, 'a' = 'a' AS c, 3 <> 3 AS d;"),
        ("NULL arithmetic", "SELECT 1 + NULL AS a, NULL = NULL AS b, NULL IS NULL AS c, 1 IS NOT NULL AS d;"),
        ("three-valued AND OR", "SELECT NULL AND 0 AS a, NULL OR 1 AS b, NULL AND 1 AS c, NOT NULL AS d;"),
        ("where filters NULL comparisons", TD + " SELECT id FROM t WHERE age > 20 ORDER BY id;"),
        ("is null in where", TD + " SELECT id FROM t WHERE age IS NULL OR score IS NULL ORDER BY id;"),
        ("in list", TD + " SELECT id FROM t WHERE name IN ('bob', 'dee', 'zed') ORDER BY id;"),
        ("not in list", TD + " SELECT id FROM t WHERE id NOT IN (1, 2) ORDER BY id;"),
        ("between", TD + " SELECT id FROM t WHERE score BETWEEN 1.5 AND 2.5 ORDER BY id;"),
        ("like", "CREATE TABLE w (s TEXT); INSERT INTO w VALUES ('apple'), ('apply'), ('maple'), ('ape');"
                 " SELECT s FROM w WHERE s LIKE 'app%' ORDER BY s; SELECT s FROM w WHERE s LIKE '_pe' ORDER BY s;"),
        ("and or not precedence", TD + " SELECT id FROM t WHERE NOT id = 1 AND age > 20 OR name = 'ann' ORDER BY id;"),
        ("expressions over columns", TD + " SELECT id, age * 2 + 1 AS x FROM t WHERE id <= 2 ORDER BY id;"),
        ("qualified column name", TD + " SELECT t.name FROM t WHERE t.id = 2;"),
        ("text comparison", TD + " SELECT name FROM t WHERE name >= 'bob' ORDER BY name;"),
    ],
    "select": [
        ("order by desc", TD + " SELECT id FROM t ORDER BY id DESC;"),
        ("order by text then id", TD + " INSERT INTO t VALUES (5, 'ann', 1, 1.0); SELECT id FROM t ORDER BY name, id DESC;"),
        ("order by expression alias", TD + " SELECT id, score * 2 AS d FROM t WHERE score IS NOT NULL ORDER BY d DESC;"),
        ("limit", TD + " SELECT id FROM t ORDER BY id LIMIT 2;"),
        ("limit offset", TD + " SELECT id FROM t ORDER BY id LIMIT 2 OFFSET 1;"),
        ("distinct", "CREATE TABLE d (x INT); INSERT INTO d VALUES (1), (2), (1), (3), (2); SELECT DISTINCT x FROM d ORDER BY x;"),
        ("select without from", "SELECT 42 AS answer, 'hi' AS greeting;"),
        ("where on primary key", TD + " SELECT name FROM t WHERE id = 3;"),
        ("primary key range", TD + " SELECT id FROM t WHERE id > 1 AND id < 4 ORDER BY id;"),
    ],
    "joins_aggregates": [
        ("inner join", J + " SELECT u.name, o.amt FROM u JOIN o ON o.uid = u.id ORDER BY o.id;"),
        ("inner join keyword", J + " SELECT o.id FROM o INNER JOIN u ON u.id = o.uid ORDER BY o.id;"),
        ("left join keeps unmatched", J + " SELECT u.name, o.amt FROM u LEFT JOIN o ON o.uid = u.id ORDER BY u.id, o.id;"),
        ("count star", J + " SELECT COUNT(*) AS n FROM o;"),
        ("count skips NULL", TD + " SELECT COUNT(age) AS n, COUNT(*) AS m FROM t;"),
        ("sum min max", J + " SELECT SUM(amt) AS s, MIN(amt) AS lo, MAX(amt) AS hi FROM o;"),
        ("avg", "CREATE TABLE a (x INT); INSERT INTO a VALUES (1), (2), (3), (4); SELECT AVG(x) AS m FROM a;"),
        ("aggregates of empty table", "CREATE TABLE a (x INT); SELECT COUNT(*) AS n, SUM(x) AS s FROM a;"),
        ("group by", J + " SELECT uid, SUM(amt) AS s, COUNT(*) AS n FROM o GROUP BY uid ORDER BY uid;"),
        ("group by having", J + " SELECT uid, SUM(amt) AS s FROM o GROUP BY uid HAVING SUM(amt) > 3 ORDER BY uid;"),
        ("join then group", J + " SELECT u.name, COUNT(o.id) AS n FROM u LEFT JOIN o ON o.uid = u.id GROUP BY u.name ORDER BY u.name;"),
        ("where before group", J + " SELECT uid, COUNT(*) AS n FROM o WHERE amt > 3 GROUP BY uid ORDER BY uid;"),
    ],
    "transactions_constraints": [
        ("duplicate primary key is an error", TD + " INSERT INTO t VALUES (2, 'x', 1, 1.0); SELECT name FROM t WHERE id = 2;"),
        ("failed multi-row insert changes nothing", TD + " INSERT INTO t VALUES (9, 'x', 1, 1.0), (1, 'y', 1, 1.0); SELECT COUNT(*) AS n FROM t;"),
        ("not null is enforced", TD + " INSERT INTO t (id, age) VALUES (8, 3); SELECT COUNT(*) AS n FROM t;"),
        ("update to NULL in NOT NULL column fails", TD + " UPDATE t SET name = NULL WHERE id = 1; SELECT name FROM t WHERE id = 1;"),
        ("update making a duplicate key fails", TD + " UPDATE t SET id = 2 WHERE id = 1; SELECT id FROM t ORDER BY id;"),
        ("rollback undoes", TD + " BEGIN; INSERT INTO t VALUES (9, 'z', 1, 1.0); DELETE FROM t WHERE id = 1; ROLLBACK; SELECT id FROM t ORDER BY id;"),
        ("commit keeps", TD + " BEGIN; INSERT INTO t VALUES (9, 'z', 1, 1.0); COMMIT; SELECT id FROM t ORDER BY id;"),
        ("rollback undoes create", "BEGIN; CREATE TABLE x (a INT); ROLLBACK; SELECT a FROM x;"),
        ("an error inside a transaction keeps earlier work", TD + " BEGIN; INSERT INTO t VALUES (9, 'z', 1, 1.0); INSERT INTO t VALUES (9, 'z', 1, 1.0); COMMIT; SELECT COUNT(*) AS n FROM t;"),
    ],
    "storage_indexes": [
        ("a thousand keys", BIG + " SELECT COUNT(*) AS n, MIN(id) AS lo, MAX(id) AS hi FROM k;"),
        ("point lookups among many", BIG + " SELECT v FROM k WHERE id = 0; SELECT v FROM k WHERE id = 999; SELECT v FROM k WHERE id = 500;"),
        ("range scan among many", BIG + " SELECT id FROM k WHERE id >= 495 AND id < 505 ORDER BY id;"),
        ("delete half then scan", BIG + " DELETE FROM k WHERE id % 2 = 0; SELECT COUNT(*) AS n FROM k; SELECT id FROM k WHERE id < 10 ORDER BY id;"),
        ("create index", TD + " CREATE INDEX t_age ON t (age); SELECT id FROM t WHERE age = 25;"),
        ("index range", BIG + " CREATE INDEX k_v ON k (v); SELECT id FROM k WHERE v BETWEEN 10 AND 13 ORDER BY v;"),
        ("index follows updates", TD + " CREATE INDEX t_age ON t (age); UPDATE t SET age = 99 WHERE id = 1;"
                                       " SELECT id FROM t WHERE age = 99; SELECT id FROM t WHERE age = 30;"),
        ("index follows deletes", TD + " CREATE INDEX t_name ON t (name); DELETE FROM t WHERE name = 'bob'; SELECT id FROM t WHERE name = 'bob';"),
        ("drop index", TD + " CREATE INDEX t_age ON t (age); DROP INDEX t_age; SELECT id FROM t WHERE age = 41;"),
    ],
}


def split(sql):  # ;-separated statements; the cases have no ';' inside strings
    return [s.strip() for s in sql.split(";") if s.strip()]


def run(sql):
    db = sqlite3.connect(":memory:", isolation_level=None)
    out = []
    for st in split(sql):
        try:
            cur = db.execute(st)
            if st.upper().startswith("SELECT"):
                out.append({"columns": [d[0] for d in cur.description], "rows": [list(r) for r in cur.fetchall()]})
            elif st.split()[0].upper() in ("INSERT", "UPDATE", "DELETE"):
                out.append({"changed": cur.rowcount})
            else:
                out.append({"changed": 0})
        except sqlite3.Error as e:
            out.append({"error": str(e)})
    return out


def lit(v):
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "1" if v else "0"
    if isinstance(v, (int, float)):
        return repr(v)
    if isinstance(v, str):
        return '"' + v.replace("\\", "\\\\").replace('"', '\\"').replace("{", "\\{").replace("\n", "\\n") + '"'
    if isinstance(v, list):
        return "[" + ", ".join(lit(x) for x in v) + "]"
    raise TypeError(v)


def main():
    os.makedirs(os.path.join(HERE, "accept"), exist_ok=True)
    total = 0
    for group, cases in CASES.items():
        src = ['// generated by gen.py from SQLite; do not edit', 'import "../src/fsql.fut"', ""]
        for name, sql in cases:
            res = run(sql)
            stmts = split(sql)
            body = [f"  var r = runScript({lit(sql)})", f"  expect r.len() == {len(res)}"]
            for i, (st, x) in enumerate(zip(stmts, res)):
                if "error" in x:
                    body.append(f"  expect r[{i}].error != nil")
                elif "columns" in x:
                    body.append(f"  expect r[{i}].error == nil")
                    body.append(f"  expect r[{i}].columns == {lit(x['columns'])}")
                    body.append(f"  expect r[{i}].rows == {lit(x['rows'])}")
                elif st.split()[0].upper() in ("INSERT", "UPDATE", "DELETE"):
                    body.append(f"  expect r[{i}].changed == {x['changed']}")
                else:
                    body.append(f"  expect r[{i}].error == nil")
            src.append(f"test {lit(name)} {{\n" + "\n".join(body) + "\n}\n")
            total += 1
        with open(os.path.join(HERE, "accept", group + "_test.fut"), "w") as f:
            f.write("\n".join(src))
    print(f"{total} acceptance tests in {len(CASES)} files")


main()
