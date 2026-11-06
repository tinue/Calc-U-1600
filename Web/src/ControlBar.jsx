// The control bar under the faceplate, as Qt6/app/ControlBar: the model
// picker, the memory-slot pickers with their Name & Save buttons, the
// model's own peripheral toggles, the CE-1600F disk group (picker, side,
// save, motor lamp) and the tape group (picker, new tape, counter). What is
// shown, enabled and listed comes from the engine (Shared/ControlState and
// the bays); the actions go back as WebController calls.

const MODEL_NAMES = ["PC-1500", "PC-1500A", "PC-1600"];
const EMPTY_LABEL = "–empty–";
const SEPARATOR_LABEL = "──────────";

// The standard "save" glyph: a diskette.
function SaveIcon() {
  return (
    <svg className="calcu-save-icon" viewBox="0 0 16 16" aria-hidden="true">
      <path d="M2.5 1.5h9l2.5 2.5v9.5a1 1 0 0 1-1 1h-10.5a1 1 0 0 1-1-1v-11a1 1 0 0 1 1-1z" fill="#3d6fb6" />
      <rect x="4" y="1.5" width="7" height="4.5" fill="#e9eef6" />
      <rect x="8.5" y="2.3" width="1.6" height="2.9" fill="#3d6fb6" />
      <rect x="3.5" y="8.5" width="9" height="6" rx="0.5" fill="#fff" />
    </svg>
  );
}

// A picker: "–empty–", then the groups, a separator between non-empty ones.
function Picker({ groups, selected, onChange, disabled, label, name, minWidth }) {
  const options = [<option key="empty" value="">{EMPTY_LABEL}</option>];
  let first = true;
  groups.forEach((group, g) => {
    if (!group || !group.length) return;
    if (!first) options.push(<option key={`sep${g}`} disabled value={`\u0000${g}`}>{SEPARATOR_LABEL}</option>);
    first = false;
    group.forEach((n) => options.push(<option key={`${g}:${n}`} value={n}>{n}</option>));
  });
  return (
    <select className="calcu-combo" value={selected || ""} disabled={disabled} aria-label={label} name={name}
      style={minWidth ? { minWidth } : undefined} onChange={(e) => onChange(e.target.value)}>
      {options}
    </select>
  );
}

function BarSeparator() {
  return <span className="calcu-bar-separator" aria-hidden="true" />;
}

function PeripheralToggle({ label, state, onToggle }) {
  if (!state || !state.onControlBar) return null;
  return (
    <button type="button" className={`calcu-toggle${state.attached ? " checked" : ""}`} aria-pressed={state.attached}
      disabled={!state.enabled} onClick={onToggle} title={`Attach/detach the ${label} (requires a power cycle)`}>
      {label}
    </button>
  );
}

function tapeCounter(lamps) {
  if (!lamps.tapeMode) return "–";
  const s = lamps.tapeSeconds;
  return `${lamps.tapeMode === "play" ? "▶" : "●"} ${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`;
}

function ControlBar({ engine, actions }) {
  const ui = useEngine(engine, (s) => s.ui);
  const lamps = useEngine(engine, (s) => s.lamps);
  const busy = useEngine(engine, (s) => s.busy);
  if (!ui) return <div className="calcu-controlbar" />;

  const slot = (n) => {
    const s = ui.slots[n - 1];
    if (!s) return null;
    return (
      <React.Fragment key={n}>
        <span className="calcu-bar-label">{n}:</span>
        <Picker groups={[s.templates, s.instances, s.roms]} selected={s.selected} disabled={busy}
          label={`Slot ${n}`} name={`slot${n}`} minWidth="11em"
          onChange={(name) => engine.call("selectModule", n, name)} />
        <button type="button" className="calcu-iconbutton" disabled={!s.canSave} title="Name & Save"
          aria-label={`Name & Save slot ${n}`} onClick={() => actions.nameAndSaveCard(n)}>
          <SaveIcon />
        </button>
      </React.Fragment>
    );
  };

  return (
    <div className="calcu-controlbar" role="toolbar">
      <select className="calcu-combo" value={ui.model} aria-label="Model" name="model" disabled={busy}
        onChange={(e) => engine.call("pickModel", Number(e.target.value))}>
        {MODEL_NAMES.map((n, i) => <option key={n} value={i}>{n}</option>)}
      </select>
      <BarSeparator />
      {slot(1)}
      {ui.slotCount >= 2 && <BarSeparator />}
      {ui.slotCount >= 2 && slot(2)}
      <span className="calcu-bar-stretch" />
      <BarSeparator />
      <PeripheralToggle label="CE-150" state={ui.ce150} onToggle={() => engine.call("togglePeripheral", 0)} />
      <PeripheralToggle label="CE-158" state={ui.ce158} onToggle={() => engine.call("togglePeripheral", 2)} />
      <PeripheralToggle label="CE-1600P" state={ui.ce1600p} onToggle={() => engine.call("togglePeripheral", 1)} />
      {ui.floppyShown && ui.floppy && (
        <React.Fragment>
          <span className="calcu-bar-label">Disk:</span>
          <Picker groups={[ui.floppy.templates, ui.floppy.instances]} selected={ui.floppy.selected}
            disabled={!ui.floppyEnabled} label="Disk" name="disk" minWidth="8em"
            onChange={(name) => engine.call("selectDisk", name)} />
          <button type="button" className="calcu-button calcu-compact" disabled={!ui.floppyEnabled}
            title="Eject and turn the disk over" aria-label="Disk side" onClick={() => engine.call("toggleDiskSide")}>
            {ui.floppy.side ? "B" : "A"}
          </button>
          <button type="button" className="calcu-iconbutton" disabled={!ui.floppy.canSave} title="Name & Save"
            aria-label="Name & Save disk" onClick={() => actions.nameAndSaveDisk()}>
            <SaveIcon />
          </button>
          <span className={`calcu-lamp${lamps.floppyMotor ? " on" : ""}`} data-lamp="floppy"
            title="Drive active -- wait for this to go dark before turning the disk over">●</span>
        </React.Fragment>
      )}
      {ui.tapeShown && ui.tape && (
        <React.Fragment>
          <BarSeparator />
          <span className="calcu-bar-label">Tape:</span>
          <Picker groups={[ui.tape.names]} selected={ui.tape.current} label="Tape" name="tape" minWidth="8em"
            onChange={(name) => actions.selectTape(name)} />
          <button type="button" className="calcu-iconbutton" title="A new tape to record onto (CSAVE)"
            aria-label="New tape" onClick={() => actions.recordTape()}>
            <SaveIcon />
          </button>
          <span className={`calcu-tape-counter${lamps.tapeMotor ? " on" : ""}`} data-lamp="tape"
            title={lamps.tapeMode === "play" ? `Playing ${lamps.tapeFile}` : lamps.tapeMode === "record" ? `Recording into ${lamps.tapeFile}` : "No tape"}>
            {tapeCounter(lamps)}
          </span>
        </React.Fragment>
      )}
    </div>
  );
}
