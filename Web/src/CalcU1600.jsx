// CalcU1600 -- the web client, and the one global calc.js exposes
// (docs/developer/Web-Client.md has the props). The desktop window in a
// box: the menu bar, the faceplate scaled to the width, the control bar,
// and the docked panels in the space below (inspector, plotter paper,
// CE-158 printer). It owns the engine (src/engine.js), the physical
// keyboard while focused, drag-and-drop, and the dialog-led flows of
// Qt6/app/MainWindow (file opens, Name & Save, tapes) -- the decisions in
// them are Shared's, behind WebController.

const MENU_HEIGHT = 26;
const BAR_HEIGHT = 40;
const PANEL_MIN = 220;          // the least room below the faceplate worth a panels' row
const INSPECTOR_MIN_WIDTH = 700; // narrower: the inspector only when no other panel is shown
const SHIFT_TAP_MS = 400;
const MAX_DROP_BYTES = 1 << 20;
const IS_MAC = typeof navigator !== "undefined" && /Mac|iPhone|iPad|iPod/.test(navigator.platform || navigator.userAgent);

// <input type=file> once; resolves with the picked Files ([] if cancelled).
function pickFiles(accept, multiple) {
  return new Promise((resolve) => {
    const input = document.createElement("input");
    input.type = "file";
    input.accept = accept;
    input.multiple = !!multiple;
    input.style.display = "none";
    let settled = false;
    const finish = (files) => {
      if (settled) return;
      settled = true;
      input.remove();
      resolve(files);
    };
    input.addEventListener("change", () => finish([...input.files]));
    input.addEventListener("cancel", () => finish([]));
    document.body.appendChild(input);
    input.click();
  });
}

// A grey image (Copy Screen) as a PNG Blob.
function grayPng(image) {
  const canvas = document.createElement("canvas");
  canvas.width = image.width;
  canvas.height = image.height;
  const ctx = canvas.getContext("2d");
  const data = ctx.createImageData(image.width, image.height);
  for (let i = 0; i < image.pixels.length; i++) {
    const v = image.pixels[i];
    data.data[4 * i] = v;
    data.data[4 * i + 1] = v;
    data.data[4 * i + 2] = v;
    data.data[4 * i + 3] = 255;
  }
  ctx.putImageData(data, 0, 0);
  return canvasPng(canvas);
}

function CalcU1600(props) {
  const { width, height, keyboard = true, workerUrl, model: modelProp, preset: presetProp } = props;
  const [engine, setEngine] = React.useState(null);
  const [presets, setPresets] = React.useState([]);
  const dialogs = useDialogs();
  const rootRef = React.useRef(null);
  const heldKeys = React.useRef(new Map());   // KeyboardEvent.code -> {base: Promise of the key to release}
  const shiftTap = React.useRef(null);        // time the bare Shift went down
  const pendingDrop = React.useRef(null);

  React.useEffect(() => {
    const e = createEngine({ workerUrl, model: modelProp, preset: presetProp });
    setEngine(e);
    // Test hook (docs/developer/Web-Client.md): the root element's
    // calcuEngine has request(method, ...args) -- e.g. request("lcdText").
    if (rootRef.current) rootRef.current.calcuEngine = e;
    return () => e.destroy();
  }, []);

  const ready = useEngine(engine, (s) => s.ready);
  // File ▸ Examples: examples/bundled.yaml's `examples`, from the engine.
  React.useEffect(() => {
    if (ready) engine.request("presetExamples").then(setPresets).catch(() => {});
  }, [ready]);
  const ui = useEngine(engine, (s) => s.ui);
  const busy = useEngine(engine, (s) => s.busy);
  const notices = useEngine(engine, (s) => s.notices);
  const fatal = useEngine(engine, (s) => s.fatal);
  const persistent = useEngine(engine, (s) => s.persistent);
  const model = ui ? ui.model : (modelProp === undefined ? 1 : modelProp);

  // The keyboard reaches the calculator without a click first: the client
  // takes the focus once it runs and after each dialog, unless the host
  // page has put it elsewhere.
  const running = ready && !!ui;
  React.useEffect(() => {
    const el = rootRef.current;
    if (!keyboard || !running || dialogs.open || !el) return;
    const active = document.activeElement;
    if (!active || active === document.body) el.focus({ preventScroll: true });
  }, [keyboard, running, dialogs.open]);

  // After a tap or click anywhere but a text field or picker, the keyboard
  // goes back to the calculator. The faceplate never takes the focus (its
  // pointerdown is prevented), and a control-bar button that kept it would
  // be pressed again by ENTER or SPACE. A dialog keeps its own focus, and
  // so does a host page element outside the client.
  const reclaimFocus = () => {
    const el = rootRef.current;
    if (!keyboard || !el) return;
    setTimeout(() => {
      if (el.querySelector(".calcu-modal")) return;
      const active = document.activeElement;
      if (active && active !== document.body && !el.contains(active)) return;
      if (active && active !== el && /^(INPUT|SELECT|TEXTAREA)$/.test(active.tagName)) return;
      el.focus({ preventScroll: true });
    }, 0);
  };

  // Notices from the engine, one at a time.
  React.useEffect(() => {
    if (!engine || !notices.length || dialogs.open) return;
    const { title, text } = notices[0];
    engine.dismissNotice();
    dialogs.ask("notice", { title, text });
  }, [engine, notices, dialogs.open]);

  // ── Flows (MainWindow's) ────────────────────────────────────────────
  const tell = (title, text) => dialogs.ask("notice", { title, text });

  const loadPresetFiles = async (files) => {
    if (!files.length) return;
    // The preset and the files it names, side by side in /uploads.
    const presetFile = files.find((f) => /\.pc1500a?$|\.pc1600$/i.test(f.name)) || files[0];
    let path = "";
    for (const f of files) {
      const p = await engine.request("upload", f.name, await fileBytes(f));
      if (f === presetFile) path = p;
    }
    engine.call("loadPreset", path);
  };

  // Load BASIC Program / Load Machine Code on a File.
  const openProgram = async (kind, file) => {
    const title = kind === "basic" ? "Load BASIC Program" : "Load Machine Code";
    const { names } = await engine.request("openProgram", file.name, await fileBytes(file));
    let index = 0;
    if (names.length) {
      index = await dialogs.ask("choice", { title, text: `${file.name} holds ${names.length} files. Load which one?`, options: names });
      if (index === null) return;
    }
    if (kind === "basic") {
      const err = await engine.request("loadBasic", index);
      if (err) tell(title, err);
      return;
    }
    const plan = await engine.request("planMachineCode", index);
    if (plan.error) { tell(title, plan.error); return; }
    if (plan.needsAddress && !(await dialogs.ask("address", { engine, plan }))) return;
    const result = await engine.request("loadMachineCode");
    if (result.error) { tell(title, result.error); return; }
    await dialogs.ask("loaded", { result });
    // Type the proposed CALL, ready to run: the user presses ENTER.
    engine.call("pasteText", result.callCommand);
  };

  const openDropped = async (file) => {
    if (busy) { pendingDrop.current = file; return; }
    if (file.size > MAX_DROP_BYTES) return;
    const target = await engine.request("dropTarget", file.name, await fileBytes(file));
    if (target === "preset") loadPresetFiles([file]);
    else if (target === "basic" || target === "code") openProgram(target, file);
  };
  React.useEffect(() => {
    if (!busy && pendingDrop.current) {
      const f = pendingDrop.current;
      pendingDrop.current = null;
      openDropped(f);
    }
  }, [busy]);

  const importFiles = async () => {
    const files = await pickFiles(".yaml,.wav", true);
    const errors = [];
    for (const f of files) {
      const err = await engine.request("importMedia", f.name, await fileBytes(f));
      if (err) errors.push(err);
    }
    return errors.join("\n");
  };

  const actions = {
    nameAndSaveCard: async (slot) => {
      const name = await dialogs.ask("prompt", { title: "Name & Save", label: "Instance name:" });
      if (name === null) return;
      const err = await engine.request("nameAndSaveCard", slot, name);
      if (err) tell("Name & Save", err);
    },
    nameAndSaveDisk: async () => {
      const name = await dialogs.ask("prompt", { title: "Name & Save", label: "Disk name:" });
      if (name === null) return;
      const err = await engine.request("nameAndSaveDisk", name);
      if (err) tell("Name & Save", err);
    },
    selectTape: async (name) => {
      const err = await engine.request("selectTape", name);
      if (err) tell("Tape", err);
    },
    recordTape: async () => {
      const name = await dialogs.ask("prompt", { title: "Save Tape", label: "Tape name:" });
      if (name === null) return;
      if (await engine.request("tapeExists", name) &&
          !(await dialogs.ask("confirm", { title: "Save Tape", text: `A tape named "${name}" exists. Replace it?` }))) return;
      const err = await engine.request("recordTape", name);
      if (err) tell("Save Tape", err);
    },
  };

  const copyScreen = () => {
    const image = engine.request("screenImage");
    copyImageOrDownload("screen.png", { "image/png": image.then(grayPng), "text/plain": image.then((i) => i.text) });
  };
  const pasteText = async () => {
    try {
      const text = await navigator.clipboard.readText();
      if (text) engine.call("pasteText", text);
    } catch (e) {
      tell("Paste Text", `The browser didn't give the clipboard to the page. Press ${IS_MAC ? "⌘" : "Ctrl+"}V on the calculator instead.`);
    }
  };

  // ── Menus ───────────────────────────────────────────────────────────
  const presetMenu = () => {
    const items = [];
    let group = null;
    for (const p of presets) {
      if (p.group !== group) {
        group = p.group;
        items.push({ heading: group });
      }
      items.push({ label: p.description || p.name, onClick: () => engine.call("loadPreset", p.ref) });
    }
    items.push({ separator: true });
    items.push({ label: "Upload Preset…", onClick: async () => loadPresetFiles(await pickFiles(".pc1500,.pc1500a,.pc1600,.bas,.bbin,.bin,.wav,.yaml", true)) });
    return items;
  };
  const mod = IS_MAC ? "⌘" : "Ctrl+";
  const s = ui || {};
  const ok = !!ui && !busy;
  const radio = (label, checked, onClick) => ({ label, checked, onClick, disabled: !ok });
  const peripheral = (label, st, p) =>
    st && st.offered ? { label, checked: st.attached, disabled: !ok || !st.enabled, onClick: () => engine.call("togglePeripheral", p) } : null;
  const menus = [
    {
      title: "File",
      items: [
        { label: "Load Preset", submenu: presetMenu(), disabled: !ok },
        { label: "Load BASIC Program…", disabled: !ok, onClick: async () => { const [f] = await pickFiles(".bas,.bbin,.wav"); if (f) openProgram("basic", f); } },
        { label: "Load Machine Code…", disabled: !ok, onClick: async () => { const [f] = await pickFiles(".bin,.wav"); if (f) openProgram("code", f); } },
        { separator: true },
        { label: "Import Card, Disk or Tape…", disabled: !ok, onClick: async () => { const err = await importFiles(); if (err) tell("Import", err); } },
        { label: "Saved Media…", disabled: !ok, onClick: () => dialogs.ask("media", { engine, importFiles }) },
      ],
    },
    {
      title: "Edit",
      items: [
        { label: "Copy Screen", shortcut: `${mod}C`, disabled: !ui, onClick: copyScreen },
        { label: "Paste Text", shortcut: `${mod}V`, disabled: !ui, onClick: pasteText },
        { separator: true },
        { label: "Settings…", shortcut: `${mod},`, disabled: !ui, onClick: () => dialogs.ask("settings", { engine }) },
      ],
    },
    {
      title: "Machine",
      items: [
        { label: "Model", disabled: !ok, submenu: MODEL_NAMES.map((n, i) => radio(n, s.model === i, () => engine.call("pickModel", i))) },
        s.pc1500RomMenu && { label: "ROM Revision", disabled: !ok, submenu: ["A01", "A03", "A04"].map((n, i) => radio(n, s.pc1500Rom === i, () => engine.call("selectPC1500Rom", i))) },
        s.pc1600RomMenu && { label: "ROM Version", disabled: !ok, submenu: ["New", "Old"].map((n, i) => radio(n, s.pc1600Rom === i, () => engine.call("selectPC1600Rom", i))) },
        s.ce1600pRomMenu && { label: "CE-1600P ROM", disabled: !ok, submenu: ["New", "Old"].map((n, i) => radio(n, s.ce1600pRom === i, () => engine.call("selectCE1600PRom", i))) },
        { label: "Peripherals", disabled: !ok, submenu: [peripheral("CE-150", s.ce150, 0), peripheral("CE-158", s.ce158, 2), peripheral("CE-1600P", s.ce1600p, 1)] },
        { separator: true },
        { label: "Reset", shortcut: `${mod}R`, disabled: !ok, onClick: () => engine.call("reset", false) },
        { label: "Reset All", shortcut: `${IS_MAC ? "⇧⌘" : "Ctrl+Shift+"}R`, disabled: !ok, onClick: () => engine.call("reset", true) },
      ],
    },
    { title: "Help", items: [{ label: "About Calc-U-1600…", onClick: () => dialogs.ask("about", {}) }] },
  ];

  // ── Keyboard (MainWindow::keyPressEvent / keyReleaseEvent) ──────────
  const releaseHeldKeys = () => {
    for (const { base } of heldKeys.current.values()) base.then((b) => { if (b) engine.call("hostKeyUp", b); });
    heldKeys.current.clear();
    shiftTap.current = null;
  };

  const onKeyDown = (e) => {
    if (!keyboard || !engine || !ui || dialogs.open) return;
    engine.unlockAudio();
    const command = IS_MAC ? e.metaKey : e.ctrlKey && !e.altKey;
    if (command) {
      shiftTap.current = null;
      const k = e.key.toLowerCase();
      if (k === "c" && !e.shiftKey) { e.preventDefault(); copyScreen(); }
      else if (k === "r") { e.preventDefault(); if (!busy) engine.call("reset", e.shiftKey); }
      else if (k === ",") { e.preventDefault(); dialogs.ask("settings", { engine }); }
      // ⌘V / Ctrl+V: the browser's paste event (onPaste) brings the text.
      return;
    }
    if (e.repeat) { e.preventDefault(); return; }
    if (e.key === "Shift") { shiftTap.current = performance.now(); return; }
    shiftTap.current = null;
    if (e.key === "Dead" || e.isComposing) return;
    // Function keys past F6 and the like stay the browser's.
    if (/^F([7-9]|1\d)$/.test(e.key)) return;
    e.preventDefault();
    if (heldKeys.current.has(e.code)) return;
    const base = engine.request("hostKeyDown", e.code, e.key, e.shiftKey, e.ctrlKey, e.altKey, e.metaKey, IS_MAC)
      .catch(() => "");
    heldKeys.current.set(e.code, { base });
  };

  const onKeyUp = (e) => {
    if (!engine) return;
    if (e.key === "Shift") {
      const tapped = shiftTap.current !== null && performance.now() - shiftTap.current <= SHIFT_TAP_MS;
      shiftTap.current = null;
      if (tapped) engine.call("hostShiftTap");
    }
    const held = heldKeys.current.get(e.code);
    if (!held) return;
    heldKeys.current.delete(e.code);
    // The engine keeps a quick tap down long enough to be scanned
    // (MachineSession::releaseKey()).
    held.base.then((b) => { if (b) engine.call("hostKeyUp", b); });
  };

  // ── Layout ──────────────────────────────────────────────────────────
  const [measured, setMeasured] = React.useState({ w: width || 0, h: height || 0 });
  React.useEffect(() => {
    if (width && height) { setMeasured({ w: width, h: height }); return undefined; }
    const el = rootRef.current && rootRef.current.parentElement;
    if (!el) return undefined;
    const ro = new ResizeObserver(() => setMeasured({ w: width || el.clientWidth, h: height || el.clientHeight }));
    ro.observe(el);
    return () => ro.disconnect();
  }, [width, height]);
  const W = measured.w || 900, H = measured.h || 600;
  // The calculator comes first: the faceplate is as large as the window
  // allows. Only when it reaches both edges and leaves PANEL_MIN or more
  // below it do the panels get that space (portrait, tall windows) -- a
  // phone in landscape shows the calculator alone.
  const aspect = FACE_ASPECT[model];
  const faceRoom = H - MENU_HEIGHT - BAR_HEIGHT;
  const faceWidth = Math.max(200, Math.min(W, faceRoom * aspect));
  const panelRoom = faceRoom - W / aspect >= PANEL_MIN;
  const showPaper = panelRoom && !!(ui && ui.paper);
  const showPrinter = panelRoom && !!(ui && ui.printerShown);
  const showInspector = panelRoom && (W >= INSPECTOR_MIN_WIDTH || !(showPaper || showPrinter));
  const anyPanel = showInspector || showPaper || showPrinter;

  return (
    <div className="calcu-client" ref={rootRef} tabIndex={0} style={{ width: W, height: H }}
      data-ready={ready && !!ui ? "true" : "false"}
      onKeyDown={onKeyDown} onKeyUp={onKeyUp} onBlur={releaseHeldKeys}
      onPointerDown={() => { shiftTap.current = null; if (engine) engine.unlockAudio(); }}
      onPointerUp={reclaimFocus}
      onChange={(e) => {
        if (e.target.tagName === "SELECT" && !e.target.closest(".calcu-modal")) { e.target.blur(); reclaimFocus(); }
      }}
      onPaste={(e) => {
        const text = e.clipboardData && e.clipboardData.getData("text/plain");
        if (text && engine) { e.preventDefault(); engine.call("pasteText", text); }
      }}
      onDragOver={(e) => { if (e.dataTransfer.types.includes("Files")) e.preventDefault(); }}
      onDrop={(e) => {
        e.preventDefault();
        const files = [...e.dataTransfer.files];
        if (files.length === 1 && engine) openDropped(files[0]);
      }}>
      <MenuBar menus={menus} />
      {!persistent && (
        <div className="calcu-banner" role="status">
          Calc-U-1600 is open in another tab: cards, disks and tapes saved here are not kept.
        </div>
      )}
      <div className="calcu-face-row">
        <Faceplate engine={engine} model={model} width={faceWidth} />
      </div>
      <ControlBar engine={engine} actions={actions} />
      {anyPanel && (
        <div className="calcu-panels">
          {showInspector && <Inspector engine={engine} />}
          {showPaper && <PlotterPaper engine={engine} />}
          {showPrinter && <Ce158Printer engine={engine} />}
        </div>
      )}
      <BusyPopup engine={engine} />
      {dialogs.element}
      {fatal && (
        <div className="calcu-modal-backdrop">
          <div className="calcu-modal" role="alertdialog">
            <div className="calcu-modal-title">Calc-U-1600 stopped</div>
            <div className="calcu-modal-body"><p className="calcu-modal-text">{fatal}</p></div>
          </div>
        </div>
      )}
    </div>
  );
}

CalcU1600.version = "__CALC_VERSION__";
if (typeof window !== "undefined") window.CalcU1600 = CalcU1600;
