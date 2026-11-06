// The dialogs, as the desktop app's: message boxes, name prompts, "Load
// which one?", the machine-code start address and its result, Settings,
// About, and the web's own Saved Media manager. Each is shown through
// useDialogs()'s ask(), which resolves with the answer (null: cancelled),
// so the flows in CalcU1600.jsx read like MainWindow's.

function Modal({ title, children, buttons, onCancel, className, name }) {
  const ref = React.useRef(null);
  React.useEffect(() => {
    const el = ref.current;
    const focusable = el && el.querySelector("input, select, button.default, button");
    if (focusable) focusable.focus();
  }, []);
  return (
    <div className="calcu-modal-backdrop" onPointerDown={(e) => e.stopPropagation()}>
      <div className={`calcu-modal ${className || ""}`} role="dialog" aria-modal="true" aria-label={title}
        data-dialog={name} ref={ref}
        onKeyDown={(e) => {
          e.stopPropagation();
          if (e.key === "Escape" && onCancel) onCancel();
        }}>
        <div className="calcu-modal-title">{title}</div>
        <div className="calcu-modal-body">{children}</div>
        {buttons && <div className="calcu-modal-buttons">{buttons}</div>}
      </div>
    </div>
  );
}

function NoticeDialog({ title, text, done }) {
  return (
    <Modal title={title} name="notice" onCancel={() => done(true)}
      buttons={<button type="button" className="calcu-button default" onClick={() => done(true)}>OK</button>}>
      <p className="calcu-modal-text">{text}</p>
    </Modal>
  );
}

function ConfirmDialog({ title, text, done }) {
  return (
    <Modal title={title} name="confirm" onCancel={() => done(null)}
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-button" onClick={() => done(null)}>No</button>
          <button type="button" className="calcu-button default" onClick={() => done(true)}>Yes</button>
        </React.Fragment>
      }>
      <p className="calcu-modal-text">{text}</p>
    </Modal>
  );
}

function PromptDialog({ title, label, done }) {
  const [value, setValue] = React.useState("");
  const ok = () => done(value.trim());
  return (
    <Modal title={title} name="prompt" onCancel={() => done(null)}
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-button" onClick={() => done(null)}>Cancel</button>
          <button type="button" className="calcu-button default" onClick={ok}>OK</button>
        </React.Fragment>
      }>
      <label className="calcu-form-row">
        <span>{label}</span>
        <input type="text" value={value} onChange={(e) => setValue(e.target.value)}
          onKeyDown={(e) => { if (e.key === "Enter") ok(); }} />
      </label>
    </Modal>
  );
}

function ChoiceDialog({ title, text, options, done }) {
  const [index, setIndex] = React.useState(0);
  return (
    <Modal title={title} name="choice" onCancel={() => done(null)}
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-button" onClick={() => done(null)}>Cancel</button>
          <button type="button" className="calcu-button default" onClick={() => done(index)}>OK</button>
        </React.Fragment>
      }>
      <p className="calcu-modal-text">{text}</p>
      <select className="calcu-combo calcu-wide" value={index} onChange={(e) => setIndex(Number(e.target.value))}>
        {options.map((o, i) => <option key={i} value={i}>{o}</option>)}
      </select>
    </Modal>
  );
}

// Load Machine Code's start address (Qt6/app/MachineCodeLoadDialog): each
// keystroke is checked by Shared's MachineCodeLoad::setAddress().
function AddressDialog({ engine, plan, done }) {
  const [text, setText] = React.useState(plan.defaultAddress || "");
  const [check, setCheck] = React.useState({ valid: false, hint: "" });
  React.useEffect(() => {
    let live = true;
    engine.request("checkAddress", text).then((c) => { if (live) setCheck(c); });
    return () => { live = false; };
  }, [text]);
  const ok = () => { if (check.valid) done(true); };
  return (
    <Modal title="Load Machine Code" name="machinecode" onCancel={() => done(null)} className="calcu-address"
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-button" onClick={() => done(null)}>Cancel</button>
          <button type="button" className="calcu-button default" disabled={!check.valid} onClick={ok}>Load</button>
        </React.Fragment>
      }>
      <div className="calcu-form">
        <label className="calcu-form-row">
          <span>Start address:</span>
          <input type="text" value={text} placeholder={plan.placeholder} onChange={(e) => setText(e.target.value)}
            onKeyDown={(e) => { if (e.key === "Enter") ok(); }} />
        </label>
        <div className="calcu-form-row"><span>Length:</span><span>{plan.length} bytes</span></div>
      </div>
      {plan.cpuNote && <p className="calcu-modal-text">{plan.cpuNote}</p>}
      {check.hint && <p className="calcu-modal-text calcu-hint">{check.hint}</p>}
    </Modal>
  );
}

function LoadedDialog({ result, done }) {
  return (
    <Modal title="Machine Code Loaded" name="loaded" onCancel={() => done(true)}
      buttons={<button type="button" className="calcu-button default" onClick={() => done(true)}>OK</button>}>
      <div className="calcu-modal-text calcu-selectable">
        <p>{result.summary}.</p>
        <p>
          Keep BASIC from overwriting it:<br />
          {result.newCommand && <React.Fragment><b><tt>{result.newCommand}</tt></b><br /></React.Fragment>}
          <small>{result.newNote}</small>
        </p>
        <p>
          Start it:<br />
          <b><tt>{result.callCommand}</tt></b><br />
          <small>{result.callNote}</small>
        </p>
      </div>
    </Modal>
  );
}

const STARTUP_CHOICES = [
  ["last", "Last used"], ["PC1500", "PC-1500"], ["PC1500A", "PC-1500A"], ["PC1600", "PC-1600"],
];
const MODEL_KEYS = ["PC1500", "PC1500A", "PC1600"];

// Settings (Qt6/app/SettingsDialog), the parts a browser has: the startup
// device and each model's default preset (from the bundled presets). The
// save folder is this browser's storage; serial ports, tracing and the
// debugger aren't there.
function SettingsDialog({ engine, presets, done }) {
  const [state, setState] = React.useState(null);
  const persistent = useEngine(engine, (s) => s.persistent);
  React.useEffect(() => { engine.request("settingsState").then(setState); }, []);
  if (!state) return null;
  const forModel = (key) => presets.filter((p) => p.model === key);
  return (
    <Modal title="Settings" name="settings" onCancel={() => done(true)} className="calcu-settings"
      buttons={<button type="button" className="calcu-button default" onClick={() => done(true)}>Close</button>}>
      <h3 className="calcu-settings-section">General</h3>
      <label className="calcu-form-row">
        <span>Startup device:</span>
        <select className="calcu-combo" value={state.startupModel}
          onChange={(e) => {
            engine.call("setStartupModel", e.target.value);
            setState({ ...state, startupModel: e.target.value });
          }}>
          {STARTUP_CHOICES.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
      </label>
      <h3 className="calcu-settings-section">Default presets</h3>
      {MODEL_KEYS.map((key, model) => (
        <label key={key} className="calcu-form-row">
          <span>{MODEL_NAMES[model]}:</span>
          <select className="calcu-combo" value={state.defaultPresets[key] || ""}
            onChange={(e) => {
              engine.call("setDefaultPreset", model, e.target.value);
              setState({ ...state, defaultPresets: { ...state.defaultPresets, [key]: e.target.value } });
            }}>
            <option value="">None</option>
            {forModel(key).map((p) => <option key={p.file} value={`/presets/${p.file}`}>{p.title}</option>)}
          </select>
        </label>
      ))}
      <h3 className="calcu-settings-section">Storage</h3>
      <p className="calcu-modal-text">
        Saved cards, disks and tapes are kept in this browser's storage for this site
        (File ▸ Saved Media… lists them).
        {!persistent && " This tab can't save: Calc-U-1600 is open in another tab."}
      </p>
    </Modal>
  );
}

function AboutDialog({ done }) {
  return (
    <Modal title="About Calc-U-1600" name="about" onCancel={() => done(true)}
      buttons={<button type="button" className="calcu-button default" onClick={() => done(true)}>OK</button>}>
      <div className="calcu-about">
        <img src={`${CALC_BASE}img/icon.png`} alt="" width="64" height="64" />
        <div>
          <p><b>Calc-U-1600</b> __CALC_VERSION__ (web)</p>
          <p>An emulator of the Sharp PC-1500, PC-1500A and PC-1600 pocket computers and their peripherals.</p>
          <p className="calcu-small">Free software under the GPL. The Sharp ROMs are © Sharp Corporation.</p>
        </div>
      </div>
    </Modal>
  );
}

const MEDIA_GROUPS = [["card", "Memory cards"], ["disk", "Floppy disks"], ["tape", "Tapes"]];

// File ▸ Saved Media…: what the browser keeps for the save folder (Shared
// MediaLibrary), each item downloadable and deletable, plus Import.
function MediaDialog({ engine, importFiles, done }) {
  const [items, setItems] = React.useState(null);
  const [error, setError] = React.useState("");
  const refresh = () => engine.request("mediaItems").then(setItems);
  React.useEffect(() => { refresh(); }, []);
  const download = async (item) => {
    const bytes = await engine.request("exportMedia", item.path);
    if (bytes) downloadBytes(item.fileName, bytes);
  };
  const remove = async (item) => {
    const err = await engine.request("removeMedia", item.path);
    setError(err);
    refresh();
  };
  return (
    <Modal title="Saved Media" name="media" onCancel={() => done(true)} className="calcu-media"
      buttons={
        <React.Fragment>
          <button type="button" className="calcu-button" onClick={async () => { setError(await importFiles()); refresh(); }}>Import…</button>
          <span className="calcu-bar-stretch" />
          <button type="button" className="calcu-button default" onClick={() => done(true)}>Close</button>
        </React.Fragment>
      }>
      {items && items.length === 0 && <p className="calcu-modal-text">Nothing saved yet. Name & Save a card or disk, or record a tape.</p>}
      {items && MEDIA_GROUPS.map(([type, heading]) => {
        const group = items.filter((i) => i.type === type);
        if (!group.length) return null;
        return (
          <div key={type} className="calcu-media-group">
            <h3 className="calcu-settings-section">{heading}</h3>
            {group.map((item) => (
              <div key={item.path} className="calcu-media-row" data-media={item.name}>
                <span className="calcu-media-name">{item.name}{item.isTemplate ? " (template)" : ""}</span>
                {item.inUse && <span className="calcu-media-inuse">in use</span>}
                <button type="button" className="calcu-button" onClick={() => download(item)}>Download</button>
                <button type="button" className="calcu-button" disabled={item.inUse} onClick={() => remove(item)}>Delete</button>
              </div>
            ))}
          </div>
        );
      })}
      {error && <p className="calcu-modal-text calcu-hint">{error}</p>}
    </Modal>
  );
}

// "Loading…" once an operation has run for half a second (as the desktop
// app's SyncOperations popup).
function BusyPopup({ engine }) {
  const busy = useEngine(engine, (s) => s.busy);
  const [shown, setShown] = React.useState(false);
  React.useEffect(() => {
    if (!busy) { setShown(false); return undefined; }
    const t = setTimeout(() => setShown(true), 500);
    return () => clearTimeout(t);
  }, [busy]);
  if (!shown) return null;
  return <div className="calcu-busy" role="status"><span className="calcu-spinner" />Loading…</div>;
}

const DIALOGS = {
  notice: NoticeDialog, confirm: ConfirmDialog, prompt: PromptDialog, choice: ChoiceDialog,
  address: AddressDialog, loaded: LoadedDialog, settings: SettingsDialog, about: AboutDialog, media: MediaDialog,
};

// ask(kind, props) shows one dialog and resolves with its answer.
function useDialogs() {
  const [current, setCurrent] = React.useState(null);
  const queue = React.useRef([]);
  const next = () => setCurrent(queue.current.length ? queue.current.shift() : null);
  const ask = React.useCallback((kind, props) => new Promise((resolve) => {
    const entry = { kind, props, resolve };
    setCurrent((c) => {
      if (c) { queue.current.push(entry); return c; }
      return entry;
    });
  }), []);
  let element = null;
  if (current) {
    const Dialog = DIALOGS[current.kind];
    element = <Dialog {...current.props} done={(value) => { current.resolve(value); next(); }} />;
  }
  return { ask, element, open: !!current };
}
