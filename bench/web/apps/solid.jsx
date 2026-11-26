// The keyed table in Solid, as js-framework-benchmark's solid (a signal per label; createSelector for the selection).
import { createSelector, createSignal, For, batch } from "solid-js";
import { render } from "solid-js/web";
import { build, buttons } from "./data.js";

const make = (n) => build(n).map(({ id, label }) => { const [get, set] = createSignal(label); return { id, label: get, setLabel: set }; });
function Main() {
  const [rows, setRows] = createSignal([]), [selected, setSelected] = createSignal(0);
  const isSelected = createSelector(selected);
  const ops = {
    run: () => setRows(make(1000)),
    runlots: () => setRows(make(10000)),
    add: () => setRows(rows().concat(make(1000))),
    update: () => batch(() => { const l = rows(); for (let i = 0; i < l.length; i += 10) l[i].setLabel((v) => v + " !!!"); }),
    clear: () => setRows([]),
    swaprows: () => {
      const l = rows().slice();
      if (l.length < 999) return;
      [l[1], l[998]] = [l[998], l[1]];
      setRows(l);
    },
  };
  return (
    <>
      <div class="jumbotron">{buttons.map(([id, label]) => <button id={id} onClick={ops[id]}>{label}</button>)}</div>
      <table class="table"><tbody id="tbody">
        <For each={rows()}>{(r) => (
          <tr class={isSelected(r.id) ? "danger" : ""}>
            <td class="col-md-1">{r.id}</td>
            <td class="col-md-4"><a class="lbl" onClick={() => setSelected(r.id)}>{r.label()}</a></td>
            <td class="col-md-1"><a class="remove" onClick={() => setRows(rows().filter((x) => x.id != r.id))}>x</a></td>
          </tr>
        )}</For>
      </tbody></table>
    </>
  );
}
render(Main, document.getElementById("main"));
