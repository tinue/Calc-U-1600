// The docked panels under the control bar, as in the desktop app: the
// INSPECTOR (Qt6/app/DebugPanel, without TRACE/LOG), the plotter paper
// (PlotterPaperWidget) and the CE-158 printer output (Ce158PrinterWidget).
// Same chrome on all three: a header strip, the output, a button bar with
// pill buttons, light or dark with the system.

function Panel({ title, className, children, buttons, panel, empty }) {
  return (
    <section className={`calcu-panel ${className || ""}`} data-panel={panel} data-empty={empty ? "true" : "false"}>
      <header className="calcu-panel-header">{title}</header>
      <div className="calcu-panel-body">{children}</div>
      <div className="calcu-panel-buttons">{buttons}</div>
    </section>
  );
}

// A pill button with a drop-down menu; `entries()` resolves to its
// MenuList items ({label, disabled, onClick}).
function PillMenu({ label, entries }) {
  const [open, setOpen] = React.useState(false);
  const [items, setItems] = React.useState([]);
  const ref = React.useRef(null);
  useDismiss(ref, open, () => setOpen(false));
  return (
    <span className="calcu-pillmenu" ref={ref}>
      <button type="button" className="calcu-pill" aria-haspopup="true" aria-expanded={open}
        onClick={async () => {
          if (open) { setOpen(false); return; }
          setItems(await entries());
          setOpen(true);
        }}>
        {label} <span className="calcu-pill-arrow">▾</span>
      </button>
      {open && <MenuList items={items} onDone={() => setOpen(false)} className="calcu-pillmenu-list" />}
    </span>
  );
}

const INSPECTOR_MAX_ENTRIES = 200;

function Inspector({ engine }) {
  const [entries, setEntries] = React.useState([]);
  const outRef = React.useRef(null);
  const add = (chunks) => setEntries((e) => e.concat(chunks).slice(-INSPECTOR_MAX_ENTRIES));

  React.useEffect(() => {
    const el = outRef.current;
    if (el) el.scrollTop = el.scrollHeight;
  }, [entries]);

  const showView = async (view) => {
    const text = await engine.request("inspectorView", view);
    if (text) add([text]);
  };
  const menu = (dumps) => async () => {
    const list = await engine.request("inspectorMenu", dumps);
    return list.map((e) => ({ label: e.title, disabled: !e.enabled, onClick: () => showView(e.view) }));
  };

  return (
    <Panel title="INSPECTOR" className="calcu-inspector" panel="inspector"
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-pill" onClick={() => showView(0)}>Pointers</button>
          <PillMenu label="Memory" entries={menu(false)} />
          <PillMenu label="Dump" entries={menu(true)} />
          <span className="calcu-bar-stretch" />
          <button type="button" className="calcu-pill" title="The module in each slot, as initial-content: YAML for a card file"
            onClick={async () => add(await engine.request("cardYaml"))}>Card YAML</button>
          <button type="button" className="calcu-pill calcu-pill-icon" title="Clear" aria-label="Clear"
            onClick={() => setEntries([])}>
            <svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3 4h10l-1 10H4L3 4zm2-2h6v1.5H5z" fill="currentColor" opacity="0.6" /></svg>
          </button>
        </React.Fragment>
      }>
      <pre className="calcu-inspector-output" ref={outRef}>{entries.join("\n")}</pre>
    </Panel>
  );
}

const FOLLOW_THRESHOLD = 64;

function PlotterPaper({ engine }) {
  const paper = useEngine(engine, (s) => s.paper);
  const lcd = useEngine(engine, (s) => s.lcd);
  const kind = useEngine(engine, (s) => (s.ui ? s.ui.paper : ""));
  const scrollRef = React.useRef(null);
  const canvasRef = React.useRef(null);
  const [view, setView] = React.useState({ w: 0, h: 0 });
  const paneWidth = view.w;

  // The pane width the worker lays the strokes out for.
  React.useEffect(() => {
    const el = scrollRef.current;
    if (!el) return undefined;
    const measure = () => setView({ w: el.clientWidth, h: el.clientHeight });
    const ro = new ResizeObserver(measure);
    ro.observe(el);
    measure();
    return () => ro.disconnect();
  }, []);
  React.useEffect(() => { if (engine && paneWidth > 0) engine.call("setPaperWidth", paneWidth); }, [engine, paneWidth, kind]);

  const height = Math.max(paper ? paper.contentHeight : 0, view.h);

  React.useLayoutEffect(() => {
    const el = scrollRef.current, canvas = canvasRef.current;
    if (!el || !canvas || paneWidth <= 0) return;
    const nearBottom = el.scrollHeight - el.clientHeight - el.scrollTop <= FOLLOW_THRESHOLD;
    const ctx = sizeCanvas(canvas, paneWidth, height);
    paintPaper(ctx, paneWidth, height, paper && !paper.empty ? paper.strokes : null, lcd);
    if (nearBottom) el.scrollTop = el.scrollHeight;
  }, [paper, lcd, paneWidth, height]);

  const empty = !paper || paper.empty;

  // The paper at its physical size (1200 DPI at most), as a PNG.
  const exportPng = async () => {
    const ex = await engine.request("paperExport");
    if (!ex) return null;
    const canvas = document.createElement("canvas");
    canvas.width = ex.width;
    canvas.height = ex.height;
    const ctx = canvas.getContext("2d");
    ctx.scale(ex.scale, ex.scale);
    paintPaper(ctx, ex.paneWidth, ex.contentHeight, ex.strokes, lcd);
    return canvasPng(canvas);
  };
  const copy = () => copyImageOrDownload(`${kind || "plot"}.png`, { "image/png": exportPng() });

  return (
    <Panel title={kind} className="calcu-paper" panel="paper" empty={empty}
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-pill" disabled={empty} title="Copy the paper to the clipboard as an image."
            onClick={copy}>Copy</button>
          <button type="button" className="calcu-pill" disabled={empty} title="Copy the paper, then tear it off (clears the plot)."
            onClick={async () => { await copy(); engine.call("clearPaper"); }}>Cut</button>
        </React.Fragment>
      }>
      <div className="calcu-paper-scroll" ref={scrollRef}>
        <canvas ref={canvasRef} className="calcu-paper-canvas" style={{ width: paneWidth, height }} />
      </div>
    </Panel>
  );
}

function Ce158Printer({ engine }) {
  const printer = useEngine(engine, (s) => s.printer);
  const outRef = React.useRef(null);
  React.useLayoutEffect(() => {
    const el = outRef.current;
    if (el && el.scrollHeight - el.clientHeight - el.scrollTop <= 40) el.scrollTop = el.scrollHeight;
  }, [printer.text]);
  const empty = printer.chunks.length === 0;
  return (
    <Panel title="CE-158" className="calcu-printer" panel="printer" empty={empty}
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-pill" disabled={empty} title="Save the printed bytes, exactly as sent, to a file."
            onClick={() => downloadBytes("ce158-printout.txt", new Blob(printer.chunks))}>Save…</button>
          <button type="button" className="calcu-pill" disabled={empty} onClick={() => engine.clearPrinter()}>Clear</button>
        </React.Fragment>
      }>
      <div className="calcu-printer-serial">RS-232C: not available in the browser</div>
      <pre className="calcu-printer-output" ref={outRef}>{printer.text}</pre>
    </Panel>
  );
}
