// The calculator face, as Qt6/app/FaceplateWidget + LcdWidget: the model's
// artwork scaled to `width` (aspect kept), the LCD painted over its
// rectangle, a translucent white flash on each pressed key (its button,
// calc.css), so a press doesn't redraw the artwork. One key per
// pointer, so two fingers can hold SHIFT and a key on a tablet; a key stays
// down until its pointer comes up, wherever that is. Holding the LCD runs
// the emulator flat out (turbo), as in the desktop app.
//
// Key rectangles and the LCD rectangle are Shared/KeyLayout's, in art
// pixels (the engine's faceplates). Transparent buttons over the keys,
// named by key, serve assistive technology and tests.

const FACE_ART = ["pc1500.png", "pc1500A.png", "pc1600.png"];
// Width / height of each artwork, known before the engine is (layout and
// prerender need it): Shared/KeyLayout's imageWidth / imageHeight.
const FACE_ASPECT = [4000 / 1760, 4000 / 1760, 4000 / 1777];
// How far (CSS px) outside a key's rectangle a touch still takes it.
const TOUCH_SLOP_PX = 24;

function LcdCanvas({ engine, model, rect }) {
  const ref = React.useRef(null);
  const lcd = useEngine(engine, (s) => s.lcd);

  React.useEffect(() => {
    const canvas = ref.current;
    if (!canvas) return undefined;
    const ctx = sizeCanvas(canvas, rect.w, rect.h);
    const draw = () => {
      const frame = engine && engine.frames.display;
      paintLcd(ctx, model, frame && frame.model === model ? frame : null, rect.w, rect.h, lcd);
    };
    draw();
    if (!engine) return undefined;
    engine.frames.onFrame = draw;
    return () => { if (engine.frames.onFrame === draw) engine.frames.onFrame = null; };
  }, [engine, model, lcd, rect.w, rect.h]);

  return (
    <canvas ref={ref} className="calcu-lcd" data-model={model}
      style={{ left: rect.x, top: rect.y, width: rect.w, height: rect.h }} aria-label="Display" role="img" />
  );
}

function Faceplate({ engine, model, width }) {
  const faceplates = useEngine(engine, (s) => s.faceplates);
  const layout = faceplates ? faceplates[model] : null;
  const height = width / FACE_ASPECT[model];
  const scale = layout ? width / layout.width : 0;
  const artRef = React.useRef(null);
  const [loaded, setLoaded] = React.useState(0);
  // pointerId -> key index (or "lcd" for the turbo hold)
  const pointers = React.useRef(new Map());
  const [pressed, setPressed] = React.useState([]);

  const scaled = (r) => ({ x: r.x * scale, y: r.y * scale, w: r.w * scale, h: r.h * scale });
  const keyRects = React.useMemo(() => (layout ? layout.keys.map(scaled) : []), [layout, scale]);
  const lcdRect = layout ? scaled(layout.lcd) : { x: 0, y: 0, w: 0, h: 0 };

  React.useEffect(() => {
    const ctx = sizeCanvas(artRef.current, width, height);
    ctx.clearRect(0, 0, width, height);
    const img = calcImage(FACE_ART[model], () => setLoaded((n) => n + 1));
    if (img) {
      ctx.imageSmoothingQuality = "high";
      ctx.drawImage(img, 0, 0, width, height);
    }
  }, [model, width, height, loaded]);

  // A model switch lets go of everything held.
  React.useEffect(() => () => releaseAll(), [model]);

  const syncPressed = () => setPressed([...pointers.current.values()].filter((v) => v !== "lcd"));

  // A mouse hits a key's rectangle exactly. A finger is reported as one
  // point, often a few pixels off and on a phone the keys are tiny (a
  // PC-1600 F-key is ~7 px tall), so a touch takes the nearest key within
  // TOUCH_SLOP_PX of its rectangle: the gaps between keys go to the closer
  // key instead of to nothing.
  const hit = (e) => {
    const box = artRef.current.getBoundingClientRect();
    const x = e.clientX - box.left, y = e.clientY - box.top;
    const distance = (r) => Math.hypot(Math.max(r.x - x, 0, x - (r.x + r.w)), Math.max(r.y - y, 0, y - (r.y + r.h)));
    if (distance(lcdRect) === 0) return "lcd";
    const slop = e.pointerType === "touch" ? TOUCH_SLOP_PX : 0;
    let best = -1, bestDistance = Infinity;
    keyRects.forEach((r, i) => {
      const d = distance(r);
      if (d <= slop && d < bestDistance) { best = i; bestDistance = d; }
    });
    return best;
  };

  const release = (pointerId) => {
    const held = pointers.current.get(pointerId);
    if (held === undefined) return;
    pointers.current.delete(pointerId);
    if (held === "lcd") {
      if (![...pointers.current.values()].includes("lcd")) engine.call("setTurbo", false);
    } else if (![...pointers.current.values()].includes(held)) {
      engine.call("faceKeyUp", layout.keys[held].name);
    }
    syncPressed();
  };

  function releaseAll() {
    for (const id of [...pointers.current.keys()]) release(id);
  }

  const onPointerDown = (e) => {
    if (!engine || !layout || e.button > 0) return;
    engine.unlockAudio();
    const target = hit(e);
    if (target === -1) return;
    e.preventDefault();
    e.currentTarget.setPointerCapture(e.pointerId);
    if (target === "lcd") {
      engine.call("setTurbo", true);
    } else if (![...pointers.current.values()].includes(target)) {
      engine.call("faceKeyDown", layout.keys[target].name);
    }
    pointers.current.set(e.pointerId, target);
    syncPressed();
  };
  const onPointerUp = (e) => release(e.pointerId);

  return (
    <div className="calcu-face" data-model={model} style={{ width, height }}
      onPointerDown={onPointerDown} onPointerUp={onPointerUp} onPointerCancel={onPointerUp}
      onContextMenu={(e) => e.preventDefault()}>
      <canvas ref={artRef} className="calcu-face-art" style={{ width, height }} aria-hidden="true" />
      {layout && <LcdCanvas engine={engine} model={model} rect={lcdRect} />}
      {layout && keyRects.map((r, i) => (
        <button key={layout.keys[i].name} type="button" tabIndex={-1} className="calcu-key"
          aria-label={layout.keys[i].name} aria-pressed={pressed.includes(i)}
          style={{ left: r.x, top: r.y, width: r.w, height: r.h }} />
      ))}
    </div>
  );
}
