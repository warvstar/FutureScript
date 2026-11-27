// A save normally changes a few characters in one file. Positions count UTF-16
// code units at both ends. null removes a file, including an obsolete import.
export const isSource = path => path.startsWith("/app/") && path.endsWith(".fut");

export function changes(before, after) {
  const patch = {};
  for (const path of new Set([...Object.keys(before), ...Object.keys(after)])) {
    if (!isSource(path) || before[path] === after[path]) continue;
    if (after[path] === undefined) { patch[path] = null; continue; }
    const a = before[path] || "", b = after[path];
    let left = 0, right = 0;
    const limit = Math.min(a.length, b.length);
    while (left < limit && a.charCodeAt(left) === b.charCodeAt(left)) left++;
    while (right < limit-left && a.charCodeAt(a.length-1-right) === b.charCodeAt(b.length-1-right)) right++;
    patch[path] = [left, a.length-left-right, b.slice(left, b.length-right)];
  }
  return patch;
}

export function apply(files, patch) {
  for (const [path, edit] of Object.entries(patch)) {
    if (!isSource(path)) throw Error("Invalid source path");
    if (edit === null) { delete files[path]; continue; }
    const [start, count, text] = edit;
    const old = files[path] || "";
    if (!Number.isInteger(start) || !Number.isInteger(count) || start < 0 || count < 0 ||
        start+count > old.length || typeof text !== "string") throw Error("Invalid source edit");
    files[path] = old.slice(0, start) + text + old.slice(start+count);
  }
}
