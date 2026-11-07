// Canvas painters: the LCD (Qt6/app/LcdWidget.cpp's algorithm, with the
// constants and colours of Shared/LcdLayout.hpp from the engine) and the
// plotter paper (strokes from Shared/PlotterPaper.hpp, already in pane
// pixels). Drawing only -- what is lit and where the strokes go comes from
// the worker.

const MONO_FONT = '"SF Mono", Menlo, Consolas, "DejaVu Sans Mono", "Liberation Mono", monospace';

const rgb = (c) => `rgb(${c[0]},${c[1]},${c[2]})`;

// A canvas of `width` × `height` CSS pixels at the screen's resolution;
// its 2D context, scaled to CSS pixels.
function sizeCanvas(canvas, width, height) {
  const ratio = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(width * ratio)), h = Math.max(1, Math.round(height * ratio));
  if (canvas.width !== w) canvas.width = w;
  if (canvas.height !== h) canvas.height = h;
  const ctx = canvas.getContext("2d");
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  return ctx;
}

// Images beside calc.js, loaded once; `onLoad` asks for a repaint.
const imageCache = new Map();
function calcImage(name, onLoad) {
  let entry = imageCache.get(name);
  if (!entry) {
    const img = new Image();
    entry = { img, loaded: false, waiters: [] };
    img.onload = () => { entry.loaded = true; entry.waiters.splice(0).forEach((f) => f()); };
    img.src = `${CALC_BASE}img/${name}`;
    imageCache.set(name, entry);
  }
  if (entry.loaded) return entry.img;
  if (onLoad) entry.waiters.push(onLoad);
  return null;
}

function paintDotMatrix(ctx, frame, x, y, w, h, ink) {
  if (!frame || frame.cols <= 0 || frame.rows <= 0 || !frame.poweredOn) return;
  const dotSpacing = w / frame.cols;
  const rowSpacing = h / frame.rows;
  const dot = Math.min(dotSpacing, rowSpacing) * 0.9;
  ctx.fillStyle = ink;
  const dots = frame.dots;
  for (let row = 0; row < frame.rows; row++) {
    for (let col = 0; col < frame.cols; col++) {
      if (!dots[row * frame.cols + col]) continue;
      const cx = x + (col + 0.5) * dotSpacing;
      const cy = y + (row + 0.5) * rowSpacing;
      ctx.fillRect(cx - dot / 2, cy - dot / 2, dot, dot);
    }
  }
}

function paintLcdPC1500(ctx, frame, width, height, L) {
  const background = rgb(L.p15Background), ink = rgb(L.ink);
  ctx.fillStyle = background;
  ctx.fillRect(0, 0, width, height);
  if (!frame || !frame.poweredOn) return;
  // 4 px horizontal padding on the whole panel.
  const left = 4, totalWidth = width - 8, totalHeight = height;
  const effectiveHeight = totalHeight * L.p15ContentScale;
  const barHeight = effectiveHeight * L.p15IndicatorBarFraction / (1 + L.p15IndicatorBarFraction);
  const canvasHeight = effectiveHeight - barHeight;
  const fontSize = effectiveHeight * L.p15IndicatorFontHeightFraction;
  const topInset = effectiveHeight * L.p15IndicatorTopInsetFraction;
  const matrixTopGap = effectiveHeight * L.p15MatrixTopGapFraction;

  ctx.font = `600 ${Math.max(1, Math.floor(fontSize))}px ${MONO_FONT}`;
  ctx.textBaseline = "top";
  ctx.fillStyle = ink;
  const y = topInset;
  for (const ind of frame.indicators) {
    const x = left + ind.x * totalWidth;
    if (ind.style === "dot") {
      const d = fontSize * 0.4;
      ctx.beginPath();
      ctx.ellipse(x + d / 2, y + d / 2, d / 2, d / 2, 0, 0, Math.PI * 2);
      ctx.fill();
      continue;
    }
    ctx.fillText(ind.text, x, y);
  }
  paintDotMatrix(ctx, frame, left, barHeight + matrixTopGap, totalWidth, canvasHeight, ink);
}

function paintLcdPC1600(ctx, frame, width, height, L) {
  const background = rgb(L.p16Background), ink = rgb(L.ink);
  ctx.fillStyle = background;
  ctx.fillRect(0, 0, width, height);
  if (!frame || !frame.poweredOn) return;
  const fontSize = height * L.p16LabelFontHeightFraction;
  const rowHeight = height * L.p16RowHeightFraction;
  const rowTop = height * L.p16IndicatorRowTop;
  ctx.textBaseline = "middle";
  for (const ind of frame.indicators) {
    const x = ind.x * width;
    const size = Math.max(1, Math.floor(fontSize * (ind.scale || 1)));
    ctx.font = `500 ${size}px ${MONO_FONT}`;
    const cy = rowTop + rowHeight / 2;
    if (ind.style === "inverted") {
      // A filled rounded box in ink, the text in the background colour.
      const w = ctx.measureText(ind.text).width;
      const h = size;
      ctx.fillStyle = ink;
      ctx.beginPath();
      if (ctx.roundRect) ctx.roundRect(x - 2, cy - h / 2 - 2, w + 4, h + 4, 2);
      else ctx.rect(x - 2, cy - h / 2 - 2, w + 4, h + 4);
      ctx.fill();
      ctx.fillStyle = background;
    } else {
      ctx.fillStyle = ink;
    }
    ctx.fillText(ind.text, x, cy);
  }
  paintDotMatrix(ctx, frame, L.p16MatrixLeft * width, L.p16MatrixTop * height,
    (L.p16MatrixRight - L.p16MatrixLeft) * width, (L.p16MatrixBottom - L.p16MatrixTop) * height, ink);
}

// The LCD for `model` (0 PC-1500, 1 PC-1500A, 2 PC-1600); `frame` the
// worker's display message (null: blank panel).
function paintLcd(ctx, model, frame, width, height, L) {
  if (!L) return;
  if (model === 2) paintLcdPC1600(ctx, frame, width, height, L);
  else paintLcdPC1500(ctx, frame, width, height, L);
}

// The paper: white, then each pen's polylines (1 pt, round caps and joins).
function paintPaper(ctx, width, height, strokes, L) {
  ctx.fillStyle = "#fff";
  ctx.fillRect(0, 0, width, height);
  if (!strokes || !L) return;
  ctx.lineWidth = L.penWidth;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  strokes.forEach((pen, i) => {
    const { coords, starts } = pen;
    if (starts.length < 2) return;
    ctx.strokeStyle = rgb(L.penColors[i]);
    ctx.beginPath();
    for (let s = 0; s + 1 < starts.length; s++) {
      const from = starts[s], to = starts[s + 1];
      if (to <= from) continue;
      ctx.moveTo(coords[2 * from], coords[2 * from + 1]);
      for (let k = from + 1; k < to; k++) ctx.lineTo(coords[2 * k], coords[2 * k + 1]);
      if (to - from === 1) ctx.lineTo(coords[2 * from] + 0.01, coords[2 * from + 1]);
    }
    ctx.stroke();
  });
}

// A PNG Blob of a canvas.
function canvasPng(canvas) {
  return new Promise((resolve) => canvas.toBlob(resolve, "image/png"));
}

// Puts `items` ({mime: Promise of a Blob or string}) on the clipboard.
// Called straight from the click (Safari allows a write only inside the
// gesture), with the data still on its way from the worker -- ClipboardItem
// takes promises. Resolves false if the browser refuses.
async function writeClipboard(items) {
  const asBlob = (mime) => Promise.resolve(items[mime]).then((v) => (typeof v === "string" ? new Blob([v], { type: mime }) : v));
  try {
    if (navigator.clipboard && window.ClipboardItem) {
      const entries = {};
      for (const mime of Object.keys(items)) entries[mime] = asBlob(mime);
      await navigator.clipboard.write([new ClipboardItem(entries)]);
      return true;
    }
    if (navigator.clipboard && items["text/plain"]) {
      await navigator.clipboard.writeText(await items["text/plain"]);
      return true;
    }
  } catch (e) { /* refused, or a type this browser doesn't take */ }
  return false;
}

// Copy Screen / the paper's Copy: `items` on the clipboard, else (the
// browser refuses) its "image/png" downloaded as `fileName`.
async function copyImageOrDownload(fileName, items) {
  if (await writeClipboard(items)) return;
  const png = await items["image/png"];
  if (png) downloadBytes(fileName, png, "image/png");
}
