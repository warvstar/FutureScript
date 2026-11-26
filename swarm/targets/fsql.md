# fsql: an embedded SQL database, written in fut

Build a small but real SQL database as a fut library, with thorough tests.

## Scope (roughly in order)
1. Text helpers the rest needs (fut has few built in): character classes, number parsing via parseNum, padding, sorting (merge sort on lists with a compare fn).
2. Lexer: SQL tokens (keywords case-insensitive, identifiers, numbers, 'strings' with '' escapes, operators, punctuation), with positions for errors.
3. Parser to plain map ASTs: CREATE TABLE (column types INT, TEXT, REAL; PRIMARY KEY; NOT NULL), DROP TABLE, INSERT (multi-row VALUES, column lists), SELECT (columns, *, aliases, WHERE, ORDER BY ASC/DESC, LIMIT/OFFSET, DISTINCT), UPDATE, DELETE, expressions (arithmetic, comparisons, AND/OR/NOT, IS NULL, IN (...), LIKE, BETWEEN, parentheses), clear error messages.
4. Storage: a B-tree keyed by primary key (or a hidden rowid), with insert, delete, point lookup and in-order range scans; tested on thousands of keys, with `expect grows` checks that lookups grow like log n.
5. Executor: evaluate expressions with SQL NULL semantics; run every statement; return {columns, rows} or {changed}.
6. Joins (INNER, LEFT) and aggregates (COUNT, SUM, AVG, MIN, MAX) with GROUP BY and HAVING.
7. Transactions: BEGIN, COMMIT, ROLLBACK (fut's `atomic { }` blocks may help), and constraint errors (NOT NULL, duplicate primary key) that leave the table unchanged.
8. Secondary indexes (CREATE INDEX), used by WHERE col = value and range filters.
9. A small REPL-style `run(db, sql)` API and a pretty-printer for result tables.

## Public API (fixed: outside tests are written against it)
`src/fsql.fut` exports `pub fn runScript(sql: str): list`. It runs `;`-separated statements in order against a new, empty database and returns one result per statement:
- SELECT: `{columns: [names], rows: [[values]]}`. A column's name is its alias, else the column name as written (`t.a` gives `a`), else `?column?`. NULL is `nil`; INT, REAL and numeric results are numbers; TEXT is a string; booleans from comparisons are numbers 1 and 0 (or nil).
- any other statement: `{changed: n}`, the number of rows inserted, updated or deleted (0 for CREATE, DROP, BEGIN, COMMIT, ROLLBACK).
- a statement that fails (syntax, unknown table or column, constraint, type): `{error: "message"}`. It changes nothing; the statements after it still run.
Without ORDER BY, rows come in no particular order. Integer division truncates toward zero (7 / 2 is 3); dividing by zero gives NULL.

## Layout
- `src/*.fut` holds the modules. `src/fsql.fut` is the entry: it `pub import`s the public API.
- `tests/*_test.fut` hold the tests. Each imports what it tests with a relative path (`import "../src/lexer.fut"`).
- `all.fut` imports every module (`import "src/x.fut"`), so `fut map all.fut` shows the whole project. Keep it current.
