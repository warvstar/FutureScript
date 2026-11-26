// The keyed table in React 19 (hooks), as js-framework-benchmark's react-hooks.
import { memo, useReducer } from "react";
import { createRoot } from "react-dom/client";
import { build, buttons } from "./data.js";

function reduce(s, a) {
  const { rows, selected } = s;
  switch (a.type) {
    case "run": return { rows: build(1000), selected: 0 };
    case "runlots": return { rows: build(10000), selected: 0 };
    case "add": return { rows: rows.concat(build(1000)), selected };
    case "update": {
      const next = rows.slice();
      for (let i = 0; i < next.length; i += 10) next[i] = { ...next[i], label: next[i].label + " !!!" };
      return { rows: next, selected };
    }
    case "clear": return { rows: [], selected: 0 };
    case "swaprows": {
      if (rows.length < 999) return s;
      const next = rows.slice();
      [next[1], next[998]] = [next[998], next[1]];
      return { rows: next, selected };
    }
    case "remove": return { rows: rows.filter((r) => r.id != a.id), selected };
    case "select": return { rows, selected: a.id };
  }
  return s;
}
const Row = memo(({ item, selected, dispatch }) => (
  <tr className={selected ? "danger" : ""}>
    <td className="col-md-1">{item.id}</td>
    <td className="col-md-4"><a className="lbl" onClick={() => dispatch({ type: "select", id: item.id })}>{item.label}</a></td>
    <td className="col-md-1"><a className="remove" onClick={() => dispatch({ type: "remove", id: item.id })}>x</a></td>
  </tr>
));
function Main() {
  const [{ rows, selected }, dispatch] = useReducer(reduce, { rows: [], selected: 0 });
  return (
    <>
      <div className="jumbotron">
        {buttons.map(([id, label]) => <button key={id} id={id} onClick={() => dispatch({ type: id })}>{label}</button>)}
      </div>
      <table className="table"><tbody id="tbody">
        {rows.map((r) => <Row key={r.id} item={r} selected={selected == r.id} dispatch={dispatch} />)}
      </tbody></table>
    </>
  );
}
createRoot(document.getElementById("main")).render(<Main />);
