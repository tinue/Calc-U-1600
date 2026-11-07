// The in-page menu bar: File / Edit / Machine / Help with the desktop
// app's items (Qt6/app/MainWindow::buildMenuBar()), minus what a browser
// can't do (Mount Directory, Quit) and plus what it needs instead (Import,
// Saved Media). Which items are shown and enabled comes from the engine's
// control state (Shared/ControlState). A click opens a menu; moving across
// the bar then switches menus, as on the desktop.
//
// An item: {label, onClick, shortcut, checked, disabled, submenu: [...]},
// or {separator: true}, or {heading: "..."}; falsy entries are skipped.

function MenuList({ items, onDone, className }) {
  const [open, setOpen] = React.useState(-1);
  return (
    <div className={`calcu-menu ${className || ""}`} role="menu">
      {items.filter(Boolean).map((item, i) => {
        if (item.separator) return <div key={i} className="calcu-menu-separator" role="separator" />;
        if (item.heading) return <div key={i} className="calcu-menu-heading">{item.heading}</div>;
        const hasSub = !!item.submenu;
        return (
          <div key={i} className={`calcu-menu-item${item.disabled ? " disabled" : ""}${open === i ? " open" : ""}`}
            role={item.checked === undefined ? "menuitem" : "menuitemcheckbox"}
            aria-checked={item.checked === undefined ? undefined : !!item.checked}
            aria-disabled={!!item.disabled}
            onPointerEnter={() => setOpen(hasSub ? i : -1)}
            onClick={(e) => {
              e.stopPropagation();
              if (item.disabled) return;
              if (hasSub) { setOpen(i); return; }
              onDone();
              if (item.onClick) item.onClick();
            }}>
            <span className="calcu-menu-check">{item.checked ? "✓" : ""}</span>
            <span className="calcu-menu-label">{item.label}</span>
            <span className="calcu-menu-shortcut">{hasSub ? "▸" : item.shortcut || ""}</span>
            {hasSub && open === i && !item.disabled && <MenuList items={item.submenu} onDone={onDone} className="calcu-submenu" />}
          </div>
        );
      })}
    </div>
  );
}

// While `active`: a pointer down outside `ref`'s element or Escape calls
// `close` (an open menu, the menu bar's or a pill's).
function useDismiss(ref, active, close) {
  React.useEffect(() => {
    if (!active) return undefined;
    const outside = (e) => { if (!ref.current || !ref.current.contains(e.target)) close(); };
    const escape = (e) => { if (e.key === "Escape") close(); };
    document.addEventListener("pointerdown", outside, true);
    document.addEventListener("keydown", escape, true);
    return () => {
      document.removeEventListener("pointerdown", outside, true);
      document.removeEventListener("keydown", escape, true);
    };
  }, [active]);
}

function MenuBar({ menus }) {
  const [open, setOpen] = React.useState(null);
  const barRef = React.useRef(null);
  useDismiss(barRef, open !== null, () => setOpen(null));

  return (
    <div className="calcu-menubar" ref={barRef} role="menubar">
      {menus.map((m) => (
        <div key={m.title} className={`calcu-menubar-item${open === m.title ? " open" : ""}`}>
          <button type="button" tabIndex={-1} role="menuitem" aria-haspopup="true" aria-expanded={open === m.title}
            onClick={() => setOpen(open === m.title ? null : m.title)}
            onPointerEnter={() => { if (open !== null) setOpen(m.title); }}>
            {m.title}
          </button>
          {open === m.title && <MenuList items={m.items} onDone={() => setOpen(null)} />}
        </div>
      ))}
    </div>
  );
}
