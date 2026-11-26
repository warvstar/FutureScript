// Labels for the JavaScript apps: the same generator as apps/fut.fut, so every app shows the same rows.
const adjectives = ["pretty", "large", "big", "small", "tall", "short", "long", "handsome", "plain", "quaint", "clean",
  "elegant", "easy", "angry", "crazy", "helpful", "mushy", "odd", "unsightly", "adorable", "important", "inexpensive",
  "cheap", "expensive", "fancy"];
const colours = ["red", "yellow", "blue", "green", "pink", "brown", "purple", "brown", "white", "black", "orange"];
const nouns = ["table", "chair", "house", "bbq", "desk", "car", "pony", "cookie", "sandwich", "burger", "pizza", "mouse",
  "keyboard"];
let seed = 1, nextId = 1;
const pick = (l) => ((seed = (seed * 16807) % 2147483647), l[seed % l.length]);
export function build(n) {
  const out = new Array(n);
  for (let i = 0; i < n; i++) out[i] = { id: nextId++, label: `${pick(adjectives)} ${pick(colours)} ${pick(nouns)}` };
  return out;
}
export const buttons = [["run", "Create 1,000 rows"], ["runlots", "Create 10,000 rows"], ["add", "Append 1,000 rows"],
  ["update", "Update every 10th row"], ["clear", "Clear"], ["swaprows", "Swap rows"]];
