(function () {
  const NS = "http://www.w3.org/2000/svg";
  const C = {
    ink: "#1e1b4b", muted: "#64748b", grid: "#e2e8f0",
    movF: "#dbeafe", movS: "#2563eb", hiF: "#fce7f3", hiS: "#db2777",
    tealF: "#ccfbf1", tealS: "#0f766e", amberF: "#fef3c7", amberS: "#d97706",
    arrow: "#4f46e5", red: "#dc2626",
  };
  const NET_COLORS = ["#2563eb", "#0f766e", "#d97706", "#7c3aed"];

  function el(name, attrs, parent) {
    const node = document.createElementNS(NS, name);
    for (const [k, v] of Object.entries(attrs || {})) node.setAttribute(k, v);
    if (parent) parent.appendChild(node);
    return node;
  }
  function label(parent, x, y, s, size, fill, anchor, weight) {
    const t = el("text", {x, y, "font-size": size || 12, fill: fill || C.ink,
      "text-anchor": anchor || "start", "font-weight": weight || "normal"}, parent);
    t.textContent = s;
    return t;
  }
  function fmt(v) {
    return (Math.round(v * 100) / 100).toString().replace("-", "−");
  }
  function svgPoint(svg, evt) {
    const p = svg.createSVGPoint();
    p.x = evt.clientX;
    p.y = evt.clientY;
    return p.matrixTransform(svg.getScreenCTM().inverse());
  }
  function card(host, w, h, title) {
    host.innerHTML = "";
    const svg = el("svg", {viewBox: `0 0 ${w} ${h}`, width: "100%", role: "img",
      "aria-label": title, "font-family": "Inter, 'Segoe UI', Helvetica, Arial, sans-serif"}, host);
    el("rect", {x: 0, y: 0, width: w, height: h, fill: "#ffffff"}, svg);
    return svg;
  }

  // ------------------------------------------------------------------ Abacus PlaceRow

  function placeRow(cells, xmin, xmax) {
    const order = cells.map((c, i) => i).sort((a, b) => cells[a].gx - cells[b].gx || a - b);
    const clusters = [];
    for (const i of order) {
      const cell = cells[i];
      let c = clusters[clusters.length - 1];
      if (!c || c.x + c.w <= cell.gx) {
        c = {cells: [], e: 0, q: 0, w: 0, x: cell.gx};
        clusters.push(c);
        c.cells.push(i); c.e += 1; c.q += cell.gx - c.w; c.w += cell.w;
        c.x = Math.min(Math.max(c.x, xmin), xmax - c.w);
        collapse(clusters, xmin, xmax);
        continue;
      }
      c.cells.push(i); c.e += 1; c.q += cell.gx - c.w; c.w += cell.w;
      collapse(clusters, xmin, xmax);
    }
    const x = new Array(cells.length);
    for (const c of clusters) {
      let pos = c.x;
      for (const i of c.cells) { x[i] = pos; pos += cells[i].w; }
    }
    return {x, clusters};
  }
  function collapse(clusters, xmin, xmax) {
    let c = clusters[clusters.length - 1];
    for (;;) {
      c.x = c.q / c.e;
      if (c.x < xmin) c.x = xmin;
      if (c.x > xmax - c.w) c.x = xmax - c.w;
      const k = clusters.indexOf(c);
      const prev = k > 0 ? clusters[k - 1] : null;
      if (!prev || prev.x + prev.w <= c.x) return;
      prev.cells = prev.cells.concat(c.cells);
      prev.q += c.q - c.e * prev.w;
      prev.e += c.e;
      prev.w += c.w;
      clusters.splice(k, 1);
      c = prev;
    }
  }
  function tetris(cells, xmin, xmax) {
    const order = cells.map((c, i) => i).sort((a, b) => cells[a].gx - cells[b].gx || a - b);
    const x = new Array(cells.length);
    let end = xmin;
    for (const i of order) {
      x[i] = Math.min(Math.max(cells[i].gx, end), xmax - cells[i].w);
      end = x[i] + cells[i].w;
    }
    return x;
  }

  function placeRowWidget(host) {
    const W = 760, H = 330, x0 = 30, sx = 28, lo = 0, hi = 25;
    const X = (v) => x0 + (v - lo) * sx;
    const cells = [
      {gx: 2, w: 3}, {gx: 3.5, w: 2}, {gx: 5, w: 4}, {gx: 12, w: 2}, {gx: 12.5, w: 3}, {gx: 14, w: 2},
    ];
    let drag = null;
    function draw() {
      const svg = card(host, W, H, "Interactive PlaceRow");
      label(svg, 20, 28, "Drag the pink cells", 16, C.ink, "start", "bold");
      const lanes = [["global placement (drag)", 48], ["Abacus: PlaceRow", 140], ["Tetris: placed cells never move", 232]];
      for (const [name, y] of lanes) {
        label(svg, 20, y - 7, name, 13, C.muted, "start", "bold");
        el("rect", {x: X(lo), y, width: (hi - lo) * sx, height: 50, rx: 3, fill: "#f8fafc", stroke: "#cbd5e1"}, svg);
      }
      const ab = placeRow(cells, lo, hi);
      const te = tetris(cells, lo, hi);
      const palette = [[C.tealF, C.tealS], [C.amberF, C.amberS]];
      ab.clusters.forEach((c, k) => {
        const [f, s] = palette[k % 2];
        el("rect", {x: X(c.x) - 3, y: 138, width: c.w * sx + 6, height: 54, rx: 6, fill: f, stroke: s,
          "stroke-width": 1.5, opacity: 0.7}, svg);
      });
      let moveA = 0, moveT = 0;
      cells.forEach((c, i) => {
        const yy = 52 + (i % 2) * 20;
        const g = el("rect", {x: X(c.gx), y: yy, width: c.w * sx, height: 22, rx: 4, fill: C.hiF, stroke: C.hiS,
          "stroke-width": 1.5, style: "cursor: grab; touch-action: none"}, svg);
        label(svg, X(c.gx) + c.w * sx / 2, yy + 16, String(i + 1), 14, C.hiS, "middle", "bold").style.pointerEvents = "none";
        g.addEventListener("pointerdown", (e) => {
          drag = {i, off: svgPoint(svg, e).x - X(c.gx)};
          e.preventDefault();
        });
        for (const [xs, y, which] of [[ab.x, 144, "a"], [te[i], 236, "t"]]) {
          const fx = which === "a" ? xs[i] : xs;
          el("line", {x1: X(c.gx) + c.w * sx / 2, y1: yy + 22, x2: X(fx) + c.w * sx / 2, y2: y, stroke: "#c7d2fe",
            "stroke-width": 1}, svg);
          el("rect", {x: X(fx), y, width: c.w * sx, height: 40, rx: 4, fill: C.movF, stroke: C.movS,
            "stroke-width": 1.5}, svg);
          label(svg, X(fx) + c.w * sx / 2, y + 26, String(i + 1), 15, C.ink, "middle", "bold");
        }
        moveA += Math.abs(ab.x[i] - c.gx);
        moveT += Math.abs(te[i] - c.gx);
      });
      label(svg, W - 20, 132, `${ab.clusters.length} cluster(s) · movement ${fmt(moveA)}`, 14, C.tealS, "end", "bold");
      label(svg, W - 20, 224, `movement ${fmt(moveT)}`, 14, C.red, "end", "bold");
      for (let v = lo; v <= hi; v += 5) label(svg, X(v), 314, String(v), 12, C.muted, "middle");
      svg.addEventListener("pointermove", (e) => {
        if (!drag) return;
        const p = svgPoint(svg, e);
        const c = cells[drag.i];
        c.gx = Math.round(Math.min(Math.max((p.x - drag.off - x0) / sx + lo, lo), hi - c.w) * 4) / 4;
        draw();
      });
      svg.addEventListener("pointerup", () => { drag = null; });
      svg.addEventListener("pointerleave", () => { drag = null; });
    }
    draw();
  }

  // ------------------------------------------------------------------ FastDP optimal region

  function optimalRegionWidget(host) {
    const W = 760, H = 390, x0 = 40, sx = 50, lo = 0, hi = 12;
    const X = (v) => x0 + (v - lo) * sx;
    let nets = [[2, 5], [4, 9], [6, 7]];
    let cellX = 3;
    let dragging = false;
    const extra = (x, [a, b]) => Math.max(0, a - x) + Math.max(0, x - b);
    function draw() {
      const svg = card(host, W, H, "Interactive optimal region");
      label(svg, 20, 28, "Drag cell i along x", 16, C.ink, "start", "bold");
      const bounds = nets.flat().sort((a, b) => a - b);
      const k = nets.length;
      const m0 = bounds[k - 1], m1 = bounds[k];
      el("rect", {x: X(m0), y: 40, width: Math.max((m1 - m0) * sx, 2), height: 290, fill: "rgba(15,118,110,0.15)"}, svg);
      label(svg, X((m0 + m1) / 2), 362, `optimal region [${fmt(m0)}, ${fmt(m1)}]`, 14, C.tealS, "middle", "bold");
      nets.forEach(([a, b], n) => {
        const y = 52 + n * 26;
        const color = NET_COLORS[n % NET_COLORS.length];
        el("rect", {x: X(a), y, width: (b - a) * sx, height: 14, rx: 3, fill: "none", stroke: color,
          "stroke-width": 2, "stroke-dasharray": "6 3"}, svg);
        const grow = extra(cellX, [a, b]);
        if (grow > 0) {
          const from = cellX < a ? cellX : b, to = cellX < a ? a : cellX;
          el("rect", {x: X(from), y: y + 3, width: (to - from) * sx, height: 8, fill: C.red, opacity: 0.6}, svg);
        }
        label(svg, X(hi) + 10, y + 12, `net ${n + 1}: +${fmt(grow)}`, 14, color, "start", "bold");
      });
      const base = 320, sy = 13;
      const total = (x) => nets.reduce((s, net) => s + extra(x, net), 0);
      const pts = [];
      for (let x = lo; x <= hi + 1e-9; x += 0.05) pts.push(`${X(x).toFixed(1)},${(base - total(x) * sy).toFixed(1)}`);
      el("line", {x1: X(lo), y1: base, x2: X(hi), y2: base, stroke: "#94a3b8"}, svg);
      el("polyline", {points: pts.join(" "), fill: "none", stroke: C.hiS, "stroke-width": 3}, svg);
      for (let v = lo; v <= hi; v += 1) label(svg, X(v), base + 18, String(v), 12, C.muted, "middle");
      const t = total(cellX);
      const slope = bounds.filter((b) => b < cellX).length - k;
      el("circle", {cx: X(cellX), cy: base - t * sy, r: 7, fill: C.hiS}, svg);
      el("line", {x1: X(cellX), y1: 40, x2: X(cellX), y2: base, stroke: C.hiS, "stroke-dasharray": "4 3"}, svg);
      const handle = el("rect", {x: X(cellX) - 18, y: 136, width: 36, height: 26, rx: 5, fill: C.hiF, stroke: C.hiS,
        "stroke-width": 2, style: "cursor: grab; touch-action: none"}, svg);
      label(svg, X(cellX), 154, "i", 14, C.hiS, "middle", "bold").style.pointerEvents = "none";
      label(svg, X(hi) + 10, 190, `x = ${fmt(cellX)}`, 15, C.ink, "start", "bold");
      label(svg, X(hi) + 10, 212, `extra ${fmt(t)}`, 15, C.hiS, "start", "bold");
      label(svg, X(hi) + 10, 234, `slope ${slope > 0 ? "+" : ""}${slope}`, 15, C.arrow, "start", "bold");
      label(svg, X(hi) + 10, 254, `${bounds.filter((b) => b < cellX).length} bounds left`, 12, C.muted);
      label(svg, X(hi) + 10, 270, `− ${k} nets`, 12, C.muted);
      handle.addEventListener("pointerdown", (e) => { dragging = true; e.preventDefault(); });
      svg.addEventListener("pointermove", (e) => {
        if (!dragging) return;
        const p = svgPoint(svg, e);
        cellX = Math.round(Math.min(Math.max((p.x - x0) / sx + lo, lo), hi) * 4) / 4;
        draw();
      });
      svg.addEventListener("pointerup", () => { dragging = false; });
      svg.addEventListener("pointerleave", () => { dragging = false; });
      button(host, "New random nets", () => {
        const count = 2 + Math.floor(Math.random() * 3);
        nets = Array.from({length: count}, () => {
          const a = Math.round(Math.random() * 9 * 2) / 2;
          return [a, Math.min(12, a + 0.5 + Math.round(Math.random() * 5 * 2) / 2)];
        });
        draw();
      }, true);
    }
    draw();
  }

  // ------------------------------------------------------------------ Layout helpers

  function controlRow(host) {
    const row = document.createElement("div");
    row.className = "kt-widget-controls";
    host.appendChild(row);
    return row;
  }
  function lastRow(host) {
    const rows = [...host.children].filter((c) => c.className === "kt-widget-controls");
    return rows.length ? rows[rows.length - 1] : controlRow(host);
  }
  function button(host, text, onClick, primary) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = primary ? "kt-btn kt-btn-primary" : "kt-btn";
    btn.textContent = text;
    btn.addEventListener("click", onClick);
    lastRow(host).appendChild(btn);
    return btn;
  }
  function choice(host, options, current, onPick) {
    const group = document.createElement("div");
    group.className = "kt-segmented";
    const buttons = options.map(([key, text]) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.textContent = text;
      btn.addEventListener("click", () => { select(key); onPick(key); });
      group.appendChild(btn);
      return [key, btn];
    });
    function select(key) {
      for (const [k, btn] of buttons) btn.className = k === key ? "kt-btn kt-btn-primary" : "kt-btn";
    }
    select(current);
    lastRow(host).appendChild(group);
    return select;
  }
  function caption(host, text) {
    const span = document.createElement("span");
    span.className = "kt-caption";
    span.textContent = text;
    lastRow(host).appendChild(span);
  }
  function spacer(host) {
    const span = document.createElement("span");
    span.className = "kt-spacer";
    lastRow(host).appendChild(span);
  }
  function slider(host, text, min, max, step, value, onInput) {
    const wrap = document.createElement("label");
    wrap.className = "kt-slider";
    const name = document.createElement("span");
    name.textContent = text;
    const input = document.createElement("input");
    Object.assign(input, {type: "range", min: String(min), max: String(max), step: String(step), value: String(value)});
    const shown = document.createElement("output");
    input.addEventListener("input", () => onInput(Number(input.value), shown));
    wrap.append(name, input, shown);
    lastRow(host).appendChild(wrap);
    return {input, shown};
  }
  function heading(scene, title, hint) {
    label(scene, 24, 32, title, 16, C.ink, "start", "bold");
    if (hint) label(scene, 24, 52, hint, 12, C.muted);
  }
  function stat(scene, x, y, name, value, color) {
    label(scene, x, y, name, 11, C.muted);
    label(scene, x, y + 22, value, 18, color || C.ink, "start", "bold");
    return y + 54;
  }
  function arrow(parent, x1, y, length, color, maxLength) {
    if (Math.abs(length) < 0.5) return;
    const shown = Math.sign(length) * Math.min(Math.abs(length), maxLength || 1e9);
    const tip = x1 + shown;
    const dir = Math.sign(shown);
    el("line", {x1, y1: y, x2: tip - dir * 6, y2: y, stroke: color, "stroke-width": 3, "stroke-linecap": "round"}, parent);
    el("polygon", {points: `${tip},${y} ${tip - dir * 9},${y - 5} ${tip - dir * 9},${y + 5}`, fill: color}, parent);
  }
  function pin(parent, x, y, size) {
    const s = size || 24;
    return el("rect", {x: x - s / 2, y: y - s / 2, width: s, height: s, rx: 4, fill: "#e5e7eb", stroke: "#374151",
      "stroke-width": 2}, parent);
  }
  function solveDense(a, b) {
    const n = b.length;
    const m = a.map((row, i) => [...row, b[i]]);
    for (let c = 0; c < n; c++) {
      let p = c;
      for (let r = c + 1; r < n; r++) if (Math.abs(m[r][c]) > Math.abs(m[p][c])) p = r;
      [m[c], m[p]] = [m[p], m[c]];
      for (let r = c + 1; r < n; r++) {
        const f = m[r][c] / m[c][c];
        for (let k = c; k <= n; k++) m[r][k] -= f * m[c][k];
      }
    }
    const x = new Array(n).fill(0);
    for (let r = n - 1; r >= 0; r--) {
      let s = m[r][n];
      for (let k = r + 1; k < n; k++) s -= m[r][k] * x[k];
      x[r] = s / m[r][r];
    }
    return x;
  }
  function random(seed) {
    let a = seed >>> 0;
    return () => {
      a = (a + 0x6d2b79f5) >>> 0;
      let t = a;
      t = Math.imul(t ^ (t >>> 15), t | 1);
      t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }
  function mix(a, b, t) {
    const pa = [1, 3, 5].map((k) => parseInt(a.slice(k, k + 2), 16));
    const pb = [1, 3, 5].map((k) => parseInt(b.slice(k, k + 2), 16));
    return `rgb(${pa.map((v, k) => Math.round(v + (pb[k] - v) * t)).join(",")})`;
  }
  function plane(px, py, size, lo, hi) {
    return {
      x: (v) => px + ((v - lo) / (hi - lo)) * size,
      y: (v) => py + ((hi - v) / (hi - lo)) * size,
      fromX: (p) => lo + ((p - px) / size) * (hi - lo),
      fromY: (p) => hi - ((p - py) / size) * (hi - lo),
      contains: (p) => p.x >= px && p.x <= px + size && p.y >= py && p.y <= py + size,
      px, py, size, lo, hi,
    };
  }
  function contours(parent, f, map) {
    const n = 72, step = (map.hi - map.lo) / n;
    const grid = [];
    let vmax = 0, vmin = 0;
    for (let j = 0; j <= n; j++) {
      const row = [];
      for (let i = 0; i <= n; i++) {
        const v = f(map.lo + i * step, map.lo + j * step);
        row.push(v);
        vmax = Math.max(vmax, v);
        vmin = Math.min(vmin, v);
      }
      grid.push(row);
    }
    el("rect", {x: map.px, y: map.py, width: map.size, height: map.size, fill: "#fbfcff", stroke: "#cbd5e1"}, parent);
    const levels = [];
    for (let k = 1; k <= 8; k++) {
      if (vmax > 1e-9) levels.push([vmax * (k / 9) ** 2, mix("#c7d2fe", "#4338ca", k / 8)]);
      if (vmin < -1e-9) levels.push([vmin * (k / 9) ** 2, mix("#fed7aa", "#c2410c", k / 8)]);
    }
    if (vmin < -1e-9 && vmax > 1e-9) levels.push([0, "#94a3b8"]);
    const at = (gi, gj) => `${map.x(map.lo + gi * step).toFixed(1)},${map.y(map.lo + gj * step).toFixed(1)}`;
    for (const [level, color] of levels) {
      let d = "";
      for (let j = 0; j < n; j++) {
        for (let i = 0; i < n; i++) {
          const corners = [[i, j], [i + 1, j], [i + 1, j + 1], [i, j + 1]];
          const hits = [];
          for (let e = 0; e < 4; e++) {
            const [x1, y1] = corners[e], [x2, y2] = corners[(e + 1) % 4];
            const a = grid[y1][x1], b = grid[y2][x2];
            if ((a < level) !== (b < level)) {
              const t = (level - a) / (b - a);
              hits.push([x1 + t * (x2 - x1), y1 + t * (y2 - y1)]);
            }
          }
          for (let k = 0; k + 1 < hits.length; k += 2) d += `M${at(...hits[k])}L${at(...hits[k + 1])}`;
        }
      }
      if (d) el("path", {d, fill: "none", stroke: color, "stroke-width": 1.4, "stroke-linecap": "round"}, parent);
    }
  }
  function eigen2(a, b, c) {
    const mean = (a + c) / 2, d = Math.sqrt(((a - c) / 2) ** 2 + b * b);
    return [mean + d, mean - d];
  }
  function draggable(svg, onMove) {
    let held = null;
    svg.addEventListener("pointermove", (evt) => {
      if (held !== null) onMove(held, svgPoint(svg, evt));
    });
    const release = () => { held = null; };
    svg.addEventListener("pointerup", release);
    svg.addEventListener("pointerleave", release);
    return (node, key) => {
      node.style.cursor = "grab";
      node.style.touchAction = "none";
      node.addEventListener("pointerdown", (evt) => { held = key; evt.preventDefault(); });
    };
  }

  // ------------------------------------------------------------------ One spring pair (§3.1)

  function springWidget(host) {
    const W = 760, H = 380, lo = 0, hi = 6;
    const X = (v) => 60 + v * 72;
    const choices = [[1, 2], [1, 1], [3, 1], [1, 5]];
    let weights = choices[0];
    let cellX = 1.5;
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive spring");
    const scene = el("g", {}, svg);
    const grab = draggable(svg, (key, p) => {
      cellX = Math.round(Math.min(Math.max((p.x - 60) / 72, lo), hi) * 4) / 4;
      draw();
    });
    function draw() {
      scene.replaceChildren();
      const [wl, wr] = weights;
      const cost = (x) => wl * x * x + wr * (hi - x) ** 2;
      const best = (wr * hi) / (wl + wr);
      const pullLeft = wl * cellX, pullRight = wr * (hi - cellX), net = pullRight - pullLeft;
      heading(scene, "Where do the two springs balance?", "Drag the cell. The arrows are the two pulls; the curve is the energy Φ.");
      const rowY = 110;
      el("line", {x1: X(lo), y1: rowY, x2: X(cellX), y2: rowY, stroke: C.arrow, "stroke-width": 1 + wl * 1.2}, scene);
      el("line", {x1: X(cellX), y1: rowY, x2: X(hi), y2: rowY, stroke: C.tealS, "stroke-width": 1 + wr * 1.2}, scene);
      if (cellX > 0.6) label(scene, (X(lo) + X(cellX)) / 2, rowY - 12, `w = ${wl}`, 12, C.arrow, "middle", "bold");
      if (cellX < hi - 0.6) label(scene, (X(cellX) + X(hi)) / 2, rowY - 12, `w = ${wr}`, 12, C.tealS, "middle", "bold");
      pin(scene, X(lo), rowY);
      pin(scene, X(hi), rowY);
      arrow(scene, X(cellX), rowY + 34, -pullLeft * 10, C.arrow, 180);
      arrow(scene, X(cellX), rowY + 34, pullRight * 10, C.tealS, 180);
      const base = 330, top = 190;
      const sy = (base - top) / Math.max(cost(lo), cost(hi));
      const pts = [];
      for (let x = lo; x <= hi + 1e-9; x += 0.05) pts.push(`${X(x).toFixed(1)},${(base - cost(x) * sy).toFixed(1)}`);
      el("line", {x1: X(lo), y1: base, x2: X(hi), y2: base, stroke: "#cbd5e1"}, scene);
      el("line", {x1: X(best), y1: top, x2: X(best), y2: base, stroke: C.tealS, "stroke-dasharray": "3 4"}, scene);
      el("polyline", {points: pts.join(" "), fill: "none", stroke: C.hiS, "stroke-width": 2.5}, scene);
      for (let v = lo; v <= hi; v += 1) label(scene, X(v), base + 18, String(v), 11, C.muted, "middle");
      label(scene, X(lo) - 10, top + 4, "Φ", 13, C.hiS, "end", "bold");
      el("circle", {cx: X(cellX), cy: base - cost(cellX) * sy, r: 6, fill: C.hiS}, scene);
      const handle = el("circle", {cx: X(cellX), cy: rowY, r: 15, fill: C.movF, stroke: C.movS, "stroke-width": 2.5}, scene);
      grab(handle, 0);
      label(scene, X(cellX), rowY + 5, "x", 13, C.movS, "middle", "bold").style.pointerEvents = "none";
      const tx = 560;
      let y = 84;
      y = stat(scene, tx, y, "position x", fmt(cellX));
      y = stat(scene, tx, y, `left pull ${wl} · x`, fmt(pullLeft), C.arrow);
      y = stat(scene, tx, y, `right pull ${wr} · (6 − x)`, fmt(pullRight), C.tealS);
      stat(scene, tx, y, "net force", Math.abs(net) < 1e-9 ? "0, at rest" : `${fmt(Math.abs(net))} ${net > 0 ? "→" : "←"}`,
        Math.abs(net) < 1e-9 ? C.tealS : C.red);
    }
    caption(host, "stiffness left · right");
    choice(host, choices.map((c, k) => [k, `${c[0]} · ${c[1]}`]), 0, (k) => { weights = choices[k]; draw(); });
    draw();
  }

  // ------------------------------------------------------------------ Spring network (Eq. 4)

  function springNetworkWidget(host) {
    const W = 760, H = 430, lo = 0.5, hi = 4, x0 = 50, sx = 640 / (hi - lo);
    const X = (v) => x0 + (v - lo) * sx;
    const mainY = 170, upperY = 100;
    const paper = () => ({
      nodes: [
        {name: "f₁", x: 1.0, fixed: true, y: mainY},
        {name: "x₁", x: 31 / 21, y: mainY},
        {name: "x₂", x: 41 / 21, y: upperY},
        {name: "x₃", x: 17 / 7, y: mainY},
        {name: "x₄", x: 22 / 7, y: mainY},
        {name: "f₂", x: 3.5, fixed: true, y: mainY},
      ],
      edges: [[0, 1, 3], [1, 2, 1], [1, 3, 1], [2, 3, 1], [3, 4, 2], [4, 5, 4]],
    });
    let {nodes, edges} = paper();
    let held = null;
    let running = false;
    const movable = () => nodes.map((n, i) => i).filter((i) => !nodes[i].fixed);
    const neighbours = (i) => edges.filter((e) => e[0] === i || e[1] === i).map(([a, b, w]) => ({j: a === i ? b : a, w}));
    const average = (i) => {
      const nb = neighbours(i);
      return nb.reduce((s, {j, w}) => s + w * nodes[j].x, 0) / nb.reduce((s, {w}) => s + w, 0);
    };
    const force = (i) => neighbours(i).reduce((s, {j, w}) => s + w * (nodes[j].x - nodes[i].x), 0);
    function exact() {
      const ids = movable();
      const index = new Map(ids.map((id, k) => [id, k]));
      const q = ids.map(() => ids.map(() => 0));
      const b = ids.map(() => 0);
      for (const [a, c, w] of edges) {
        for (const [u, v] of [[a, c], [c, a]]) {
          if (!index.has(u)) continue;
          q[index.get(u)][index.get(u)] += w;
          if (index.has(v)) q[index.get(u)][index.get(v)] -= w;
          else b[index.get(u)] += w * nodes[v].x;
        }
      }
      return solveDense(q, b);
    }
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive spring network");
    const scene = el("g", {}, svg);
    svg.addEventListener("pointermove", (evt) => {
      if (held === null) return;
      nodes[held].x = Math.min(Math.max((svgPoint(svg, evt).x - x0) / sx + lo, lo), hi);
    });
    const release = () => { held = null; };
    svg.addEventListener("pointerup", release);
    svg.addEventListener("pointerleave", release);
    function draw() {
      scene.replaceChildren();
      heading(scene, "The worked example as springs", "Drag a cell or a pin. Click a spring to change its stiffness. Red arrows: net force.");
      for (let v = 1; v <= hi; v += 0.5) {
        el("line", {x1: X(v), y1: 72, x2: X(v), y2: 236, stroke: C.grid}, scene);
        label(scene, X(v), 252, String(v), 11, C.muted, "middle");
      }
      edges.forEach((e) => {
        const [a, b, w] = e;
        const p = nodes[a], q = nodes[b];
        el("line", {x1: X(p.x), y1: p.y, x2: X(q.x), y2: q.y, stroke: "#94a3b8", "stroke-width": 1 + w * 1.4}, scene);
        const mx = (X(p.x) + X(q.x)) / 2, my = (p.y + q.y) / 2 + (p.y === q.y ? 20 : -8);
        label(scene, mx, my, String(w), 12, C.amberS, "middle", "bold");
        const hit = el("line", {x1: X(p.x), y1: p.y, x2: X(q.x), y2: q.y, stroke: "transparent", "stroke-width": 18,
          style: "cursor: pointer"}, scene);
        hit.addEventListener("pointerdown", (evt) => { e[2] = (e[2] % 5) + 1; evt.preventDefault(); start(); });
      });
      for (const i of movable()) arrow(scene, X(nodes[i].x), nodes[i].y + 34, force(i) * 40, C.red, 110);
      nodes.forEach((n, i) => {
        const shape = n.fixed ? pin(scene, X(n.x), n.y, 30)
          : el("circle", {cx: X(n.x), cy: n.y, r: 16, fill: C.movF, stroke: C.movS, "stroke-width": 2}, scene);
        shape.style.cursor = "grab";
        shape.style.touchAction = "none";
        shape.addEventListener("pointerdown", (evt) => { held = i; evt.preventDefault(); start(); });
        label(scene, X(n.x), n.y + 5, n.name, 13, n.fixed ? "#374151" : C.movS, "middle", "bold").style.pointerEvents = "none";
      });
      const solution = exact();
      const top = 290;
      const cols = [50, 150, 270, 430, 590];
      ["cell", "position x", "neighbours' average", "net force", "solution of Qx = b"].forEach((h, k) =>
        label(scene, cols[k], top, h, 11, C.muted, "start", "bold"));
      el("line", {x1: 40, y1: top + 10, x2: W - 40, y2: top + 10, stroke: C.grid}, scene);
      movable().forEach((i, k) => {
        const y = top + 34 + k * 26;
        const f = force(i);
        const still = Math.abs(f) < 0.005;
        label(scene, cols[0], y, nodes[i].name, 13, C.movS, "start", "bold");
        label(scene, cols[1], y, nodes[i].x.toFixed(3), 13, C.ink);
        label(scene, cols[2], y, average(i).toFixed(3), 13, C.ink);
        label(scene, cols[3], y, still ? "0" : `${fmt(Math.abs(f))} ${f > 0 ? "→" : "←"}`, 13, still ? C.tealS : C.red, "start", "bold");
        label(scene, cols[4], y, solution[k].toFixed(3), 13, C.tealS);
      });
    }
    function tick() {
      let biggest = 0;
      const next = nodes.map((n, i) => (n.fixed || i === held ? n.x : n.x + 0.12 * (average(i) - n.x)));
      next.forEach((v, i) => { biggest = Math.max(biggest, Math.abs(v - nodes[i].x)); nodes[i].x = v; });
      draw();
      if (biggest > 1e-5 || held !== null) requestAnimationFrame(tick);
      else running = false;
    }
    function start() {
      if (!running) { running = true; requestAnimationFrame(tick); }
    }
    button(host, "Scramble the cells", () => {
      for (const i of movable()) nodes[i].x = lo + 0.2 + Math.random() * (hi - lo - 0.4);
      start();
    }, true);
    button(host, "Reset", () => { ({nodes, edges} = paper()); start(); });
    draw();
  }

  // ------------------------------------------------------------------ Collapse in two dimensions

  function collapseWidget(host) {
    const W = 760, H = 460, n = 40;
    const map = plane(40, 66, 380, 0, 10);
    const pads = [[0, 1.5], [0, 5], [0, 8.5], [10, 1.5], [10, 5], [10, 8.5],
      [1.5, 0], [5, 0], [8.5, 0], [1.5, 10], [5, 10], [8.5, 10]];
    let seed = 7, cells, edges, iteration, timer = null;
    function makeNetlist() {
      const r = random(seed);
      edges = [];
      for (let i = 1; i < n; i++) edges.push([i, Math.floor(r() * i)]);
      for (let k = 0; k < n / 2; k++) {
        const a = Math.floor(r() * n), b = Math.floor(r() * n);
        if (a !== b) edges.push([a, b]);
      }
      edges.push([0, n + Math.floor(r() * pads.length)]);
      for (let k = 0; k < 11; k++) edges.push([Math.floor(r() * n), n + Math.floor(r() * pads.length)]);
      cells = Array.from({length: n}, () => [0.6 + r() * 8.8, 0.6 + r() * 8.8]);
      iteration = 0;
    }
    const at = (id) => (id < n ? cells[id] : pads[id - n]);
    function sweep() {
      const sum = cells.map(() => [0, 0, 0]);
      for (const [a, b] of edges) {
        for (const [u, v] of [[a, b], [b, a]]) {
          if (u >= n) continue;
          const p = at(v);
          sum[u][0] += p[0]; sum[u][1] += p[1]; sum[u][2] += 1;
        }
      }
      let moved = 0;
      cells = cells.map((c, i) => {
        const next = [sum[i][0] / sum[i][2], sum[i][1] / sum[i][2]];
        moved = Math.max(moved, Math.abs(next[0] - c[0]) + Math.abs(next[1] - c[1]));
        return next;
      });
      iteration += 1;
      return moved;
    }
    const wirelength = () => edges.reduce((s, [a, b]) => {
      const p = at(a), q = at(b);
      return s + (p[0] - q[0]) ** 2 + (p[1] - q[1]) ** 2;
    }, 0);
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive collapse");
    const scene = el("g", {}, svg);
    function draw() {
      scene.replaceChildren();
      heading(scene, "Forty cells, twelve pads, nothing but springs", "Each step moves every cell to the average of its neighbours.");
      el("rect", {x: map.px, y: map.py, width: map.size, height: map.size, fill: "#fbfcff", stroke: "#cbd5e1"}, scene);
      for (const [a, b] of edges) {
        const p = at(a), q = at(b);
        el("line", {x1: map.x(p[0]), y1: map.y(p[1]), x2: map.x(q[0]), y2: map.y(q[1]),
          stroke: b >= n ? C.amberS : "#cbd5e1", "stroke-width": 1, opacity: b >= n ? 0.6 : 1}, scene);
      }
      const xs = cells.map((c) => c[0]), ys = cells.map((c) => c[1]);
      const box = [Math.min(...xs), Math.min(...ys), Math.max(...xs), Math.max(...ys)];
      el("rect", {x: map.x(box[0]) - 8, y: map.y(box[3]) - 8, width: map.x(box[2]) - map.x(box[0]) + 16,
        height: map.y(box[1]) - map.y(box[3]) + 16, rx: 6, fill: "none", stroke: C.tealS, "stroke-dasharray": "5 4"}, scene);
      for (const [x, y] of pads) pin(scene, map.x(x), map.y(y), 16);
      for (const [x, y] of cells) {
        el("rect", {x: map.x(x) - 5, y: map.y(y) - 5, width: 10, height: 10, rx: 2, fill: C.movF, stroke: C.movS,
          "stroke-width": 1.5}, scene);
      }
      const tx = 480;
      let y = 96;
      y = stat(scene, tx, y, "step", String(iteration));
      y = stat(scene, tx, y, "squared wirelength", String(Math.round(wirelength())), C.hiS);
      stat(scene, tx, y, "cells' bounding box (dashed)", `${Math.round((box[2] - box[0]) * (box[3] - box[1]))}% of the chip`, C.tealS);
    }
    function stop() {
      if (timer !== null) clearInterval(timer);
      timer = null;
      play.textContent = "Play";
    }
    const play = button(host, "Play", () => {
      if (timer !== null) { stop(); return; }
      play.textContent = "Pause";
      timer = setInterval(() => {
        const moved = sweep();
        draw();
        if (moved < 1e-3) stop();
      }, 120);
    }, true);
    button(host, "Step", () => { stop(); sweep(); draw(); });
    button(host, "Jump to the solution", () => {
      stop();
      for (let k = 0; k < 5000 && sweep() > 1e-9; k++);
      draw();
    });
    spacer(host);
    button(host, "New netlist", () => { stop(); seed += 1; makeNetlist(); draw(); });
    makeNetlist();
    draw();
  }

  // ------------------------------------------------------------------ SimPL anchor weight

  function anchorWidget(host) {
    const W = 760, H = 380, anchorAt = 10;
    const X = (v) => 60 + v * 45;
    let logAlpha = -1;
    let timer = null;
    const position = (a) => (anchorAt * a) / (1 + a);
    const plotX = (la) => 60 + ((la + 2) / 3) * 450;
    const plotY = (x) => 340 - (x / 10) * 130;
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive anchor weight");
    const scene = el("g", {}, svg);
    function draw() {
      scene.replaceChildren();
      const a = 10 ** logAlpha, x = position(a);
      heading(scene, "One cell between its wire and its anchor", "The wire pulls toward the pin at 0, the anchor spring toward 10.");
      const rowY = 104;
      el("line", {x1: X(0), y1: rowY, x2: X(x), y2: rowY, stroke: C.arrow, "stroke-width": 3}, scene);
      el("line", {x1: X(x), y1: rowY, x2: X(anchorAt), y2: rowY, stroke: C.hiS, "stroke-width": 1 + Math.min(a, 10) * 0.6,
        "stroke-dasharray": "7 4"}, scene);
      pin(scene, X(0), rowY);
      el("rect", {x: X(anchorAt) - 11, y: rowY - 11, width: 22, height: 22, rx: 3, fill: C.hiF, stroke: C.hiS, "stroke-width": 2,
        "stroke-dasharray": "4 2"}, scene);
      label(scene, X(0), rowY + 30, "pin", 11, C.muted, "middle");
      label(scene, X(anchorAt), rowY + 30, "anchor", 11, C.hiS, "middle", "bold");
      arrow(scene, X(x), rowY - 30, -x * 40, C.arrow, 230);
      arrow(scene, X(x), rowY - 30, a * (anchorAt - x) * 40, C.hiS, 230);
      el("circle", {cx: X(x), cy: rowY, r: 13, fill: C.movF, stroke: C.movS, "stroke-width": 2}, scene);
      label(scene, X(x), rowY + 4, "x", 13, C.movS, "middle", "bold");
      el("line", {x1: plotX(-2), y1: plotY(0), x2: plotX(1), y2: plotY(0), stroke: "#cbd5e1"}, scene);
      el("line", {x1: plotX(-2), y1: plotY(0), x2: plotX(-2), y2: plotY(10), stroke: "#cbd5e1"}, scene);
      const pts = [];
      for (let la = -2; la <= 1.0001; la += 0.02) pts.push(`${plotX(la).toFixed(1)},${plotY(position(10 ** la)).toFixed(1)}`);
      el("polyline", {points: pts.join(" "), fill: "none", stroke: C.movS, "stroke-width": 2.5}, scene);
      for (const [la, text] of [[-2, "0.01"], [-1, "0.1"], [0, "1"], [1, "10"]]) {
        label(scene, plotX(la), plotY(0) + 16, text, 11, C.muted, "middle");
      }
      label(scene, plotX(1) + 10, plotY(0) + 4, "α", 13, C.ink, "start", "bold");
      label(scene, plotX(-2) - 8, plotY(10) + 4, "10", 11, C.muted, "end");
      label(scene, plotX(-2) - 8, plotY(0) + 4, "0", 11, C.muted, "end");
      label(scene, plotX(-2) + 8, plotY(10) - 4, "cell position", 11, C.movS, "start", "bold");
      el("circle", {cx: plotX(logAlpha), cy: plotY(x), r: 6, fill: C.hiS}, scene);
      const tx = 570;
      let y = 84;
      y = stat(scene, tx, y, "anchor weight α", a < 0.1 ? a.toFixed(3) : a.toFixed(2));
      y = stat(scene, tx, y, "cell position 10α / (1 + α)", x.toFixed(2), C.movS);
      y = stat(scene, tx, y, "each pull (they are equal)", x.toFixed(2), C.hiS);
      stat(scene, tx, y, "SimPL iteration with this α", String(Math.max(0, Math.round(a / 0.01 - 1))));
    }
    function stop() {
      if (timer !== null) clearInterval(timer);
      timer = null;
      play.textContent = "Play";
    }
    const play = button(host, "Play", () => {
      if (timer !== null) { stop(); return; }
      play.textContent = "Pause";
      if (logAlpha >= 0.999) logAlpha = -2;
      timer = setInterval(() => {
        logAlpha = Math.min(1, Math.log10(10 ** logAlpha + Math.max(0.01, 10 ** logAlpha * 0.04)));
        control.input.value = String(logAlpha);
        control.shown.textContent = (10 ** logAlpha).toFixed(2);
        draw();
        if (logAlpha >= 0.999) stop();
      }, 50);
    }, true);
    const control = slider(host, "α", -2, 1, 0.01, logAlpha, (v, out) => {
      stop();
      logAlpha = v;
      out.textContent = (10 ** v).toFixed(2);
      draw();
    });
    control.shown.textContent = (10 ** logAlpha).toFixed(2);
    draw();
  }

  // ------------------------------------------------------------------ HPWL

  function hpwlWidget(host) {
    const W = 760, H = 400;
    const X = (v) => 56 + v * 46, Y = (v) => 360 - v * 46;
    const start = () => [[1, 1], [4, 2], [2, 5]];
    let pins = start();
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive HPWL");
    const scene = el("g", {}, svg);
    const grab = draggable(svg, (i, p) => {
      pins[i] = [Math.min(10, Math.max(0, Math.round(((p.x - 56) / 46) * 2) / 2)),
        Math.min(6, Math.max(0, Math.round(((360 - p.y) / 46) * 2) / 2))];
      draw();
    });
    function draw() {
      scene.replaceChildren();
      heading(scene, "Drag the pins of one net", "Pink pins are on the box edge and set HPWL. A blue pin inside changes nothing.");
      for (let v = 0; v <= 10; v++) {
        el("line", {x1: X(v), y1: Y(0), x2: X(v), y2: Y(6), stroke: C.grid}, scene);
        label(scene, X(v), Y(0) + 18, String(v), 10, C.muted, "middle");
      }
      for (let v = 0; v <= 6; v++) {
        el("line", {x1: X(0), y1: Y(v), x2: X(10), y2: Y(v), stroke: C.grid}, scene);
        label(scene, X(0) - 10, Y(v) + 4, String(v), 10, C.muted, "end");
      }
      const xs = pins.map((p) => p[0]), ys = pins.map((p) => p[1]);
      const x0 = Math.min(...xs), x1 = Math.max(...xs), y0 = Math.min(...ys), y1 = Math.max(...ys);
      el("rect", {x: X(x0), y: Y(y1), width: X(x1) - X(x0), height: Y(y0) - Y(y1), fill: "rgba(219,39,119,0.06)",
        stroke: C.hiS, "stroke-width": 2, "stroke-dasharray": "6 4"}, scene);
      if (pins.length === 2) {
        const [[ax, ay], [bx, by]] = pins;
        el("polyline", {points: `${X(ax)},${Y(ay)} ${X(bx)},${Y(ay)} ${X(bx)},${Y(by)}`, fill: "none", stroke: C.tealS,
          "stroke-width": 3}, scene);
      }
      pins.forEach(([x, y], i) => {
        const onEdge = x === x0 || x === x1 || y === y0 || y === y1;
        const dot = el("circle", {cx: X(x), cy: Y(y), r: 10, fill: onEdge ? C.hiF : C.movF, stroke: onEdge ? C.hiS : C.movS,
          "stroke-width": 2.5}, scene);
        grab(dot, i);
      });
      const tx = 590;
      let y = 96;
      y = stat(scene, tx, y, "width (max x − min x)", fmt(x1 - x0));
      y = stat(scene, tx, y, "height (max y − min y)", fmt(y1 - y0));
      y = stat(scene, tx, y, "HPWL = width + height", fmt(x1 - x0 + y1 - y0), C.hiS);
      if (pins.length === 2) label(scene, tx, y, "two pins: the grid walk (teal)", 11, C.tealS, "start", "bold");
    }
    button(host, "Add a pin", () => {
      if (pins.length < 7) pins.push([Math.round(Math.random() * 20) / 2, Math.round(Math.random() * 12) / 2]);
      draw();
    }, true);
    button(host, "Remove a pin", () => { if (pins.length > 2) pins.pop(); draw(); });
    button(host, "Reset", () => { pins = start(); draw(); });
    draw();
  }

  // ------------------------------------------------------------------ Partial derivatives and the gradient

  function gradientWidget(host) {
    const W = 760, H = 464;
    const map = plane(60, 86, 340, -1, 3);
    let withPin = true;
    let point = [2.6, -0.4];
    let trail = [];
    let held = false, timer = null;
    const f = (a, b) => (withPin ? (a - 1) ** 2 : 0) + (a - b) ** 2;
    const grad = (a, b) => [(withPin ? 2 * (a - 1) : 0) + 2 * (a - b), 2 * (b - a)];
    const hessian = () => (withPin ? [[4, -2], [-2, 2]] : [[2, -2], [-2, 2]]);
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive gradient");
    const terrain = el("g", {}, svg);
    const scene = el("g", {}, svg);
    function drawTerrain() {
      terrain.replaceChildren();
      contours(terrain, f, map);
      for (let v = -1; v <= 3; v++) {
        label(terrain, map.x(v), map.y(-1) + 18, String(v), 10, C.muted, "middle");
        label(terrain, map.x(-1) - 8, map.y(v) + 4, String(v), 10, C.muted, "end");
      }
      label(terrain, map.x(3) + 8, map.y(-1) + 4, "x₁", 13, C.ink, "start", "bold");
      label(terrain, map.x(-1) - 8, map.y(3) - 10, "x₂", 13, C.ink, "end", "bold");
    }
    function draw() {
      scene.replaceChildren();
      heading(scene, withPin ? "f = (x₁ − 1)² + (x₁ − x₂)²: a bowl" : "f = (x₁ − x₂)²: the pin removed, a trough",
        "Drag the white dot. Dashed: the slope east and the slope north. Red: straight downhill.");
      if (withPin) el("circle", {cx: map.x(1), cy: map.y(1), r: 5, fill: C.tealS}, scene);
      else el("line", {x1: map.x(-1), y1: map.y(-1), x2: map.x(3), y2: map.y(3), stroke: C.tealS, "stroke-width": 2.5,
        "stroke-dasharray": "6 4"}, scene);
      if (trail.length > 1) {
        el("polyline", {points: trail.map(([a, b]) => `${map.x(a)},${map.y(b)}`).join(" "), fill: "none", stroke: C.hiS,
          "stroke-width": 2}, scene);
      }
      const [a, b] = point;
      const [g1, g2] = grad(a, b);
      const k = 20;
      const sx = map.x(a), sy = map.y(b);
      const ex = sx - g1 * k, ey = sy + g2 * k;
      el("line", {x1: sx, y1: sy, x2: ex, y2: sy, stroke: C.amberS, "stroke-width": 2.5, "stroke-dasharray": "5 3"}, scene);
      el("line", {x1: ex, y1: sy, x2: ex, y2: ey, stroke: C.tealS, "stroke-width": 2.5, "stroke-dasharray": "5 3"}, scene);
      const len = Math.hypot(ex - sx, ey - sy);
      if (len > 1) {
        const ux = (ex - sx) / len, uy = (ey - sy) / len;
        el("line", {x1: sx, y1: sy, x2: ex - ux * 8, y2: ey - uy * 8, stroke: C.red, "stroke-width": 3.5}, scene);
        el("polygon", {points: `${ex},${ey} ${ex - ux * 11 - uy * 6},${ey - uy * 11 + ux * 6} ${ex - ux * 11 + uy * 6},${ey - uy * 11 - ux * 6}`,
          fill: C.red}, scene);
      }
      const dot = el("circle", {cx: sx, cy: sy, r: 9, fill: "#ffffff", stroke: C.ink, "stroke-width": 2.5,
        style: "cursor: grab; touch-action: none"}, scene);
      dot.addEventListener("pointerdown", (evt) => { held = true; evt.preventDefault(); });
      const tx = 450;
      let y = 96;
      y = stat(scene, tx, y, withPin ? "slope east  ∂f/∂x₁ = 2(x₁ − 1) + 2(x₁ − x₂)" : "slope east  ∂f/∂x₁ = 2(x₁ − x₂)", fmt(g1), C.amberS);
      y = stat(scene, tx, y, "slope north  ∂f/∂x₂ = 2(x₂ − x₁)", fmt(g2), C.tealS);
      y = stat(scene, tx, y, "height f", fmt(f(a, b)));
      const flat = Math.hypot(g1, g2) < 1e-3;
      if (flat) label(scene, tx, y + 4, withPin ? "Both slopes are zero: the bottom." : "Both slopes zero, as on all of x₁ = x₂.",
        13, C.tealS, "start", "bold");
    }
    function step() {
      const [a, b] = point;
      const g = grad(a, b);
      const H2 = hessian();
      const hg = [H2[0][0] * g[0] + H2[0][1] * g[1], H2[1][0] * g[0] + H2[1][1] * g[1]];
      const gg = g[0] * g[0] + g[1] * g[1];
      if (gg < 1e-12) return false;
      const t = gg / (g[0] * hg[0] + g[1] * hg[1]);
      if (!trail.length) trail.push([a, b]);
      point = [a - t * g[0], b - t * g[1]];
      trail.push(point);
      return true;
    }
    function stop() {
      if (timer !== null) clearInterval(timer);
      timer = null;
    }
    function moveTo(evt) {
      const p = svgPoint(svg, evt);
      point = [Math.min(3, Math.max(-1, map.fromX(p.x))), Math.min(3, Math.max(-1, map.fromY(p.y)))];
      trail = [];
      draw();
    }
    svg.addEventListener("pointerdown", (evt) => {
      if (!map.contains(svgPoint(svg, evt))) return;
      stop();
      held = true;
      moveTo(evt);
    });
    svg.addEventListener("pointermove", (evt) => { if (held) moveTo(evt); });
    const release = () => { held = false; };
    svg.addEventListener("pointerup", release);
    svg.addEventListener("pointerleave", release);
    button(host, "Step downhill", () => { stop(); step(); draw(); }, true);
    button(host, "Roll to the bottom", () => {
      stop();
      let count = 0;
      timer = setInterval(() => {
        if (!step() || ++count > 60) stop();
        draw();
      }, 250);
    });
    spacer(host);
    const toggle = button(host, "Remove the fixed pin", () => {
      stop();
      withPin = !withPin;
      toggle.textContent = withPin ? "Remove the fixed pin" : "Put the pin back";
      trail = [];
      drawTerrain();
      draw();
    });
    drawTerrain();
    draw();
  }

  // ------------------------------------------------------------------ Bowl, trough, saddle

  function shapeWidget(host) {
    const W = 760, H = 444;
    const map = plane(60, 86, 330, -2, 2);
    let q = {a: 2, b: 1, c: 3};
    let probe = [1, 2];
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive quadratic form");
    const surface = el("g", {}, svg);
    const scene = el("g", {}, svg);
    const value = (x, y) => q.a * x * x + 2 * q.b * x * y + q.c * y * y;
    function kind() {
      const det = q.a * q.c - q.b * q.b;
      if (Math.abs(det) < 1e-6) {
        if (q.a + q.c > 0 && q.a >= 0 && q.c >= 0) return ["a trough", "one flat direction: no single bottom", C.amberS];
        if (q.a + q.c < 0) return ["an upside-down trough", "no bottom", C.red];
        return ["flat", "every point is a bottom", C.amberS];
      }
      if (det < 0) return ["a saddle", "downhill forever in some directions", C.red];
      if (q.a > 0) return ["a bowl", "symmetric positive definite: one bottom", C.tealS];
      return ["a dome", "no bottom at all", C.red];
    }
    function drawSurface() {
      surface.replaceChildren();
      contours(surface, value, map);
      el("line", {x1: map.x(-2), y1: map.y(0), x2: map.x(2), y2: map.y(0), stroke: "#e2e8f0"}, surface);
      el("line", {x1: map.x(0), y1: map.y(-2), x2: map.x(0), y2: map.y(2), stroke: "#e2e8f0"}, surface);
      label(surface, map.x(2) + 8, map.y(0) + 4, "x₁", 13, C.ink, "start", "bold");
      label(surface, map.x(0), map.y(2) - 8, "x₂", 13, C.ink, "middle", "bold");
    }
    function draw() {
      scene.replaceChildren();
      const [name, meaning, color] = kind();
      heading(scene, `Q = [[${fmt(q.a)}, ${fmt(q.b)}], [${fmt(q.b)}, ${fmt(q.c)}]]`, "Blue lines: xᵀQx > 0. Orange: xᵀQx < 0. Click to test a vector.");
      const [x, y] = probe;
      const v = value(x, y);
      el("line", {x1: map.x(0), y1: map.y(0), x2: map.x(x), y2: map.y(y), stroke: C.ink, "stroke-width": 2}, scene);
      el("circle", {cx: map.x(x), cy: map.y(y), r: 7, fill: "#ffffff", stroke: C.ink, "stroke-width": 2.5}, scene);
      const tx = 440;
      label(scene, tx, 104, name, 24, color, "start", "bold");
      label(scene, tx, 126, meaning, 12, color);
      const [l1, l2] = eigen2(q.a, q.b, q.c);
      let yy = 166;
      yy = stat(scene, tx, yy, "curvatures (eigenvalues)", `${fmt(Math.round(l1 * 100) / 100)} and ${fmt(Math.round(l2 * 100) / 100)}`);
      stat(scene, tx, yy, `xᵀQx at x = (${fmt(x)}, ${fmt(y)})`, fmt(Math.round(v * 100) / 100),
        v > 1e-9 ? C.tealS : v < -1e-9 ? C.red : C.amberS);
    }
    svg.addEventListener("pointerdown", (evt) => {
      const p = svgPoint(svg, evt);
      if (!map.contains(p)) return;
      probe = [Math.round(map.fromX(p.x) * 10) / 10, Math.round(map.fromY(p.y) * 10) / 10];
      draw();
    });
    const sliders = {};
    for (const key of ["a", "b", "c"]) {
      sliders[key] = slider(host, key, -2, 4, 0.1, q[key], (v, out) => {
        q[key] = v;
        out.textContent = fmt(v);
        select(null);
        drawSurface();
        draw();
      });
      sliders[key].shown.textContent = fmt(q[key]);
    }
    const presets = {bowl: {a: 2, b: -1, c: 2}, trough: {a: 1, b: -1, c: 1}, saddle: {a: 1, b: 2, c: 1}};
    controlRow(host);
    caption(host, "examples");
    const select = choice(host, [["bowl", "Bowl"], ["trough", "Trough"], ["saddle", "Saddle"]], null, (key) => {
      q = {...presets[key]};
      for (const k of ["a", "b", "c"]) {
        sliders[k].input.value = String(q[k]);
        sliders[k].shown.textContent = fmt(q[k]);
      }
      drawSurface();
      draw();
    });
    drawSurface();
    draw();
  }

  // ------------------------------------------------------------------ Big-O

  function bigOWidget(host) {
    const W = 760, H = 310;
    let exponent = 6;
    const rows = [
      ["O(n)", (n) => n],
      ["O(n log n)", (n) => n * Math.log2(n)],
      ["O(n log² n)", (n) => n * Math.log2(n) ** 2],
      ["O(n²)", (n) => n * n],
      ["O(n³)", (n) => n ** 3],
    ];
    const lo = -9, hi = 12, x0 = 150, width = 440;
    const X = (seconds) => x0 + ((Math.log10(Math.max(seconds, 1e-9)) - lo) / (hi - lo)) * width;
    function human(s) {
      const units = [[3.15e7, "years"], [86400, "days"], [3600, "hours"], [60, "minutes"], [1, "seconds"], [1e-3, "ms"], [1e-6, "µs"], [1e-9, "ns"]];
      for (const [size, name] of units) {
        if (s >= size) return `${s / size < 10 ? (s / size).toFixed(1) : Math.round(s / size).toLocaleString("en")} ${name}`;
      }
      return "< 1 ns";
    }
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive growth rates");
    const scene = el("g", {}, svg);
    function draw() {
      scene.replaceChildren();
      heading(scene, `n = ${Math.round(10 ** exponent).toLocaleString("en")} cells`, "Time at 10⁹ simple steps per second, on a log scale.");
      for (const [seconds, name] of [[1e-6, "1 µs"], [1, "1 second"], [86400, "1 day"], [3.15e9, "100 years"]]) {
        el("line", {x1: X(seconds), y1: 70, x2: X(seconds), y2: 280, stroke: C.grid, "stroke-width": 1.5}, scene);
        label(scene, X(seconds), 296, name, 11, C.muted, "middle");
      }
      rows.forEach(([name, f], k) => {
        const seconds = f(10 ** exponent) / 1e9;
        const y = 82 + k * 40;
        const color = seconds < 1 ? C.tealS : seconds < 86400 ? C.amberS : C.red;
        label(scene, x0 - 14, y + 17, name, 13, C.ink, "end", "bold");
        el("rect", {x: x0, y, width: Math.max(2, X(seconds) - x0), height: 24, rx: 4, fill: color, opacity: 0.85}, scene);
        label(scene, Math.min(X(seconds), x0 + width) + 10, y + 17, human(seconds), 13, color, "start", "bold");
      });
    }
    const control = slider(host, "cells n", 2, 7, 0.1, exponent, (v, out) => {
      exponent = v;
      out.textContent = `10^${v.toFixed(1)}`;
      draw();
    });
    control.shown.textContent = "10^6.0";
    draw();
  }

  // ------------------------------------------------------------------ Stamps into Q and b

  function stampWidget(host) {
    const W = 760, H = 450;
    const names = ["x₁", "x₂", "x₃", "x₄"];
    const graph = [
      {name: "f₁", x: 80, y: 150, fixed: 1.0}, {name: "x₁", x: 210, y: 150}, {name: "x₂", x: 310, y: 90},
      {name: "x₃", x: 410, y: 150}, {name: "x₄", x: 540, y: 150}, {name: "f₂", x: 670, y: 150, fixed: 3.5},
    ];
    const all = () => [[0, 1, 3], [1, 2, 1], [1, 3, 1], [2, 3, 1], [3, 4, 2], [4, 5, 4]].map(([a, b, w]) => ({a, b, w, on: true}));
    const edges = all();
    let hover = null;
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive stamps");
    const top = el("g", {}, svg);
    const bottom = el("g", {}, svg);
    const movableIndex = (node) => (graph[node].fixed === undefined ? node - 1 : -1);
    function stamp(edge) {
      const touched = [];
      const i = movableIndex(edge.a), j = movableIndex(edge.b);
      if (i >= 0) touched.push(`q${i}${i}`);
      if (j >= 0) touched.push(`q${j}${j}`);
      if (i >= 0 && j >= 0) touched.push(`q${i}${j}`, `q${j}${i}`);
      if (i >= 0 && j < 0) touched.push(`b${i}`);
      if (j >= 0 && i < 0) touched.push(`b${j}`);
      return touched;
    }
    function build() {
      const q = names.map(() => names.map(() => 0));
      const b = names.map(() => 0);
      for (const e of edges.filter((x) => x.on)) {
        const i = movableIndex(e.a), j = movableIndex(e.b);
        if (i >= 0) q[i][i] += e.w;
        if (j >= 0) q[j][j] += e.w;
        if (i >= 0 && j >= 0) { q[i][j] -= e.w; q[j][i] -= e.w; }
        if (i >= 0 && j < 0) b[i] += e.w * graph[e.b].fixed;
        if (j >= 0 && i < 0) b[j] += e.w * graph[e.a].fixed;
      }
      return {q, b};
    }
    const visible = [];
    const edgeColor = (e, k) => (hover === k ? C.amberS : e.on ? "#64748b" : "#cbd5e1");
    function highlight() {
      visible.forEach((line, k) => {
        line.setAttribute("stroke", edgeColor(edges[k], k));
        line.setAttribute("stroke-width", edges[k].on ? 1.5 + edges[k].w : 1.5);
        line.setAttribute("stroke-dasharray", edges[k].on ? "none" : "5 4");
      });
      drawMatrix();
    }
    function drawGraph() {
      top.replaceChildren();
      visible.length = 0;
      heading(top, "Each edge stamps its weight into Q and b", "Point at an edge to see its stamp. Click it to remove it or put it back.");
      edges.forEach((e, k) => {
        const p = graph[e.a], q = graph[e.b];
        visible.push(el("line", {x1: p.x, y1: p.y, x2: q.x, y2: q.y, stroke: edgeColor(e, k), "stroke-width": 1.5 + e.w}, top));
        const mx = (p.x + q.x) / 2, my = (p.y + q.y) / 2 + (p.y === q.y ? 20 : -8);
        label(top, mx, my, String(e.w), 12, C.amberS, "middle", "bold");
        const hit = el("line", {x1: p.x, y1: p.y, x2: q.x, y2: q.y, stroke: "transparent", "stroke-width": 20, style: "cursor: pointer"}, top);
        hit.addEventListener("pointerenter", () => { hover = k; highlight(); });
        hit.addEventListener("pointerleave", () => { if (hover === k) { hover = null; highlight(); } });
        hit.addEventListener("click", () => { e.on = !e.on; highlight(); });
      });
      graph.forEach((n) => {
        if (n.fixed !== undefined) {
          pin(top, n.x, n.y, 30);
          label(top, n.x, n.y + 36, `at ${n.fixed}`, 11, C.muted, "middle");
        } else {
          el("circle", {cx: n.x, cy: n.y, r: 16, fill: C.movF, stroke: C.movS, "stroke-width": 2}, top);
        }
        label(top, n.x, n.y + 5, n.name, 13, n.fixed !== undefined ? "#374151" : C.movS, "middle", "bold").style.pointerEvents = "none";
      });
    }
    function drawMatrix() {
      bottom.replaceChildren();
      const {q, b} = build();
      const marks = hover === null ? [] : stamp(edges[hover]);
      const cell = 44, qx = 110, qy = 236;
      const box = (x, y, text, marked, tint) => {
        el("rect", {x, y, width: cell - 4, height: cell - 4, rx: 5, fill: marked ? C.amberF : tint,
          stroke: marked ? C.amberS : "#e2e8f0", "stroke-width": marked ? 2 : 1}, bottom);
        label(bottom, x + (cell - 4) / 2, y + 27, text, 15, text === "0" ? "#cbd5e1" : C.ink, "middle", "bold");
      };
      label(bottom, qx + 2 * cell - 2, qy - 14, "Q", 14, C.ink, "middle", "bold");
      label(bottom, qx + 4 * cell + 46, qy - 14, "b", 14, C.ink, "middle", "bold");
      names.forEach((name, i) => {
        label(bottom, qx - 12, qy + i * cell + 27, name, 13, C.movS, "end", "bold");
        names.forEach((_, j) => box(qx + j * cell, qy + i * cell, fmt(q[i][j]), marks.includes(`q${i}${j}`), i === j ? "#eef2ff" : "#f8fafc"));
        box(qx + 4 * cell + 26, qy + i * cell, fmt(b[i]), marks.includes(`b${i}`), "#f8fafc");
      });
      const tx = 400;
      if (hover === null) {
        label(bottom, tx, qy + 20, "Point at an edge above.", 13, C.muted);
        return;
      }
      const e = edges[hover];
      const fixedEnd = [graph[e.a], graph[e.b]].find((g) => g.fixed !== undefined);
      label(bottom, tx, qy + 20, `${graph[e.a].name}–${graph[e.b].name}, weight ${e.w}`, 15, C.amberS, "start", "bold");
      const lines = fixedEnd
        ? [`movable to fixed: +${e.w} on the diagonal,`, `and ${e.w} × ${fixedEnd.fixed} = ${fmt(e.w * fixedEnd.fixed)} into b`]
        : [`movable to movable: +${e.w} on both diagonals,`, `−${e.w} on the two entries that link them`];
      lines.forEach((text, k) => label(bottom, tx, qy + 46 + k * 20, text, 13, C.ink));
      label(bottom, tx, qy + 100, e.on ? "In Q now. Click to take it out." : "Not in Q. Click to put it back.", 12, C.muted);
    }
    button(host, "All edges", () => { edges.forEach((e) => { e.on = true; }); highlight(); });
    button(host, "No edges", () => { edges.forEach((e) => { e.on = false; }); highlight(); });
    drawGraph();
    drawMatrix();
  }

  // ------------------------------------------------------------------ Jacobi on a chain

  function jacobiWidget(host) {
    const W = 760, H = 450, x0 = 70, width = 420, top = 86, bottomY = 400;
    let count = 12, cells, steps, history, timer = null;
    const X = (v) => x0 + (v / 10) * width;
    const exact = (k) => (10 * (k + 1)) / (count + 1);
    const row = (k) => top + ((k + 1) / (count + 1)) * (bottomY - top);
    const error = () => Math.max(...cells.map((v, k) => Math.abs(v - exact(k))));
    function reset() {
      cells = Array.from({length: count}, () => 0);
      steps = 0;
      history = [error()];
    }
    function sweep() {
      cells = cells.map((_, k) => ((k === 0 ? 0 : cells[k - 1]) + (k === count - 1 ? 10 : cells[k + 1])) / 2);
      steps += 1;
      history.push(error());
    }
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive Jacobi iteration");
    const scene = el("g", {}, svg);
    function draw() {
      scene.replaceChildren();
      heading(scene, `Jacobi on a chain of ${count} cells`, "Every cell starts at 0. The dashed line is the exact answer.");
      for (let v = 0; v <= 10; v += 2) {
        el("line", {x1: X(v), y1: top, x2: X(v), y2: bottomY, stroke: C.grid}, scene);
        label(scene, X(v), bottomY + 28, String(v), 11, C.muted, "middle");
      }
      el("line", {x1: X(0), y1: top, x2: X(10), y2: bottomY, stroke: C.tealS, "stroke-dasharray": "4 4", opacity: 0.6}, scene);
      const pts = [`${X(0)},${top}`, ...cells.map((v, k) => `${X(v)},${row(k)}`), `${X(10)},${bottomY}`];
      el("polyline", {points: pts.join(" "), fill: "none", stroke: "#94a3b8", "stroke-width": 1.5}, scene);
      pin(scene, X(0), top, 18);
      pin(scene, X(10), bottomY, 18);
      cells.forEach((v, k) => {
        el("circle", {cx: X(v), cy: row(k), r: 5.5, fill: v > 1e-9 ? C.movS : "#ffffff", stroke: C.movS, "stroke-width": 1.5}, scene);
      });
      const tx = 540;
      let y = 96;
      y = stat(scene, tx, y, "step", String(steps));
      y = stat(scene, tx, y, "cells that have moved", `${cells.filter((v) => v > 1e-9).length} of ${count}`, C.movS);
      y = stat(scene, tx, y, "largest distance from the answer", history[history.length - 1].toFixed(3), C.red);
      const gx = tx, gy = y + 4, gw = 180, gh = 70;
      el("rect", {x: gx, y: gy, width: gw, height: gh, rx: 4, fill: "#fbfcff", stroke: "#e2e8f0"}, scene);
      const span = Math.max(50, history.length);
      const ly = (e) => gy + gh - ((Math.log10(Math.max(e, 1e-4)) + 4) / 5) * gh;
      el("polyline", {points: history.map((e, k) => `${gx + (k / span) * gw},${ly(e)}`).join(" "), fill: "none",
        stroke: C.red, "stroke-width": 2}, scene);
      label(scene, gx, gy + gh + 16, "error per step (log scale)", 11, C.muted);
    }
    function stop() {
      if (timer !== null) clearInterval(timer);
      timer = null;
      play.textContent = "Play";
    }
    const play = button(host, "Play", () => {
      if (timer !== null) { stop(); return; }
      play.textContent = "Pause";
      timer = setInterval(() => {
        for (let k = steps < 40 ? 1 : 5; k > 0; k--) sweep();
        draw();
        if (error() < 1e-3 || steps > 3000) stop();
      }, 60);
    }, true);
    button(host, "Step", () => { stop(); sweep(); draw(); });
    button(host, "Reset", () => { stop(); reset(); draw(); });
    const control = slider(host, "cells", 3, 40, 1, count, (v, out) => {
      stop();
      count = v;
      out.textContent = String(v);
      reset();
      draw();
    });
    control.shown.textContent = String(count);
    reset();
    draw();
  }

  // ------------------------------------------------------------------ Steepest descent, CG and preconditioning

  function descentWidget(host) {
    const W = 760, H = 450;
    const map = plane(60, 80, 340, -3, 3);
    let kappa = 20, angle = 30, precondition = false;
    let start = [-1.3, 2.5];
    let held = false;
    function matrix() {
      const c = Math.cos((angle * Math.PI) / 180), s = Math.sin((angle * Math.PI) / 180);
      const a = kappa * c * c + s * s, b = (kappa - 1) * c * s, d = kappa * s * s + c * c;
      return precondition ? [1, b / Math.sqrt(a * d), 1] : [a, b, d];
    }
    const times = ([a, b, d], [x, y]) => [a * x + b * y, b * x + d * y];
    const dot = (u, v) => u[0] * v[0] + u[1] * v[1];
    function steepest(m, from) {
      const path = [from];
      let x = from;
      const scale = Math.hypot(...from);
      for (let k = 0; k < 500 && Math.hypot(...x) > 1e-3 * scale; k++) {
        const r = times(m, x).map((v) => -v);
        const t = dot(r, r) / dot(r, times(m, r));
        x = [x[0] + t * r[0], x[1] + t * r[1]];
        path.push(x);
      }
      return path;
    }
    function conjugate(m, from) {
      const path = [from];
      let x = from;
      let r = times(m, x).map((v) => -v);
      let p = r;
      for (let k = 0; k < 2 && dot(r, r) > 1e-14; k++) {
        const qp = times(m, p);
        const t = dot(r, r) / dot(p, qp);
        x = [x[0] + t * p[0], x[1] + t * p[1]];
        const next = [r[0] - t * qp[0], r[1] - t * qp[1]];
        const beta = dot(next, next) / dot(r, r);
        p = [next[0] + beta * p[0], next[1] + beta * p[1]];
        r = next;
        path.push(x);
      }
      return path;
    }
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive descent");
    const bowl = el("g", {}, svg);
    const scene = el("g", {}, svg);
    function drawBowl() {
      bowl.replaceChildren();
      const [a, b, d] = matrix();
      contours(bowl, (x, y) => a * x * x + 2 * b * x * y + d * y * y, map);
      el("circle", {cx: map.x(0), cy: map.y(0), r: 4, fill: C.ink}, bowl);
    }
    function draw() {
      scene.replaceChildren();
      const m = matrix();
      const sd = steepest(m, start), cg = conjugate(m, start);
      heading(scene, precondition ? "The same bowl after Jacobi preconditioning" : "Steepest descent against conjugate gradient",
        "Drag the white dot. Red: steepest descent. Teal: conjugate gradient.");
      el("polyline", {points: sd.map(([x, y]) => `${map.x(x)},${map.y(y)}`).join(" "), fill: "none", stroke: C.red,
        "stroke-width": 1.8}, scene);
      el("polyline", {points: cg.map(([x, y]) => `${map.x(x)},${map.y(y)}`).join(" "), fill: "none", stroke: C.tealS,
        "stroke-width": 3, opacity: 0.85}, scene);
      cg.slice(1).forEach(([x, y]) => el("circle", {cx: map.x(x), cy: map.y(y), r: 4, fill: C.tealS}, scene));
      const handle = el("circle", {cx: map.x(start[0]), cy: map.y(start[1]), r: 9, fill: "#ffffff", stroke: C.ink,
        "stroke-width": 2.5, style: "cursor: grab; touch-action: none"}, scene);
      handle.addEventListener("pointerdown", (evt) => { held = true; evt.preventDefault(); });
      const [l1, l2] = eigen2(...m);
      const tx = 450;
      let y = 96;
      y = stat(scene, tx, y, "condition number κ (steepest / flattest curvature)", (l1 / l2).toFixed(1));
      y = stat(scene, tx, y, "steepest descent: steps to get 1000× closer", String(sd.length - 1), C.red);
      stat(scene, tx, y, "conjugate gradient: steps (2 unknowns, so ≤ 2)", String(cg.length - 1), C.tealS);
    }
    svg.addEventListener("pointermove", (evt) => {
      if (!held) return;
      const p = svgPoint(svg, evt);
      start = [Math.min(2.9, Math.max(-2.9, map.fromX(p.x))), Math.min(2.9, Math.max(-2.9, map.fromY(p.y)))];
      if (Math.hypot(...start) < 0.2) start = [0.2, 0.2];
      draw();
    });
    const release = () => { held = false; };
    svg.addEventListener("pointerup", release);
    svg.addEventListener("pointerleave", release);
    const k = slider(host, "κ", 0, 2, 0.01, Math.log10(kappa), (v, out) => {
      kappa = 10 ** v;
      out.textContent = kappa.toFixed(1);
      drawBowl();
      draw();
    });
    k.shown.textContent = kappa.toFixed(1);
    const r = slider(host, "tilt", 0, 90, 1, angle, (v, out) => {
      angle = v;
      out.textContent = `${v}°`;
      drawBowl();
      draw();
    });
    r.shown.textContent = `${angle}°`;
    const toggle = button(host, "Precondition", () => {
      precondition = !precondition;
      toggle.className = precondition ? "kt-btn kt-btn-primary" : "kt-btn";
      drawBowl();
      draw();
    });
    drawBowl();
    draw();
  }

  // ------------------------------------------------------------------ Net models

  function netModelWidget(host) {
    const W = 760, H = 400, x0 = 60, width = 640, lineY = 250;
    const X = (v) => x0 + (v / 10) * width;
    let pins = [0, 2, 5, 10];
    let model = "b2b";
    let frozen = null;
    let held = null;
    const hpwl = () => Math.max(...pins) - Math.min(...pins);
    function rebuild() {
      const order = pins.map((v, i) => i).sort((a, b) => pins[a] - pins[b]);
      const left = order[0], right = order[order.length - 1];
      const pairs = [[left, right]];
      for (const k of order.slice(1, -1)) pairs.push([k, left], [k, right]);
      frozen = pairs.map(([i, j]) => ({i, j, w: 1 / ((pins.length - 1) * Math.max(Math.abs(pins[i] - pins[j]), 0.1))}));
    }
    function edgesFor() {
      if (model === "clique") {
        const list = [];
        for (let i = 0; i < pins.length; i++) for (let j = i + 1; j < pins.length; j++) list.push({i, j, w: 1});
        return list;
      }
      if (model === "star") return pins.map((_, i) => ({i, j: "star", w: 1}));
      return frozen;
    }
    const star = () => pins.reduce((s, v) => s + v, 0) / pins.length;
    const at = (j) => (j === "star" ? star() : pins[j]);
    const cost = () => edgesFor().reduce((s, {i, j, w}) => s + w * (pins[i] - at(j)) ** 2, 0);
    host.innerHTML = "";
    const svg = card(host, W, H, "Interactive net models");
    const scene = el("g", {}, svg);
    const grab = draggable(svg, (i, p) => {
      pins[i] = Math.min(10, Math.max(0, Math.round(((p.x - x0) / width) * 40) / 4));
      if (model !== "b2b") rebuild();
      draw();
    });
    function draw() {
      scene.replaceChildren();
      const titles = {clique: "Clique: an edge between every pair, weight 1", star: "Star: every pin to one extra node, weight 1",
        b2b: "Bound2Bound: every pin to the two outer pins"};
      heading(scene, titles[model], model === "b2b" ? "Weight 1 / ((p − 1) · length). Thicker arcs are stiffer. Drag the pins."
        : "Drag the pins.");
      for (let v = 0; v <= 10; v++) label(scene, X(v), lineY + 30, String(v), 11, C.muted, "middle");
      el("line", {x1: X(0), y1: lineY, x2: X(10), y2: lineY, stroke: "#cbd5e1"}, scene);
      const list = edgesFor();
      list.forEach(({i, j, w}) => {
        const a = X(pins[i]), b = X(at(j));
        if (j === "star") {
          el("line", {x1: a, y1: lineY, x2: b, y2: lineY + 64, stroke: C.amberS, "stroke-width": 2}, scene);
          return;
        }
        const h = 16 + Math.abs(a - b) * 0.22;
        el("path", {d: `M ${a} ${lineY} Q ${(a + b) / 2} ${lineY - 2 * h} ${b} ${lineY}`, fill: "none", stroke: C.amberS,
          "stroke-width": model === "b2b" ? 1.2 + Math.min(4, w * 8) : 1.6, opacity: 0.9}, scene);
      });
      if (model === "star") {
        const sx = X(star());
        el("rect", {x: sx - 8, y: lineY + 56, width: 16, height: 16, transform: `rotate(45 ${sx} ${lineY + 64})`,
          fill: C.amberF, stroke: C.amberS, "stroke-width": 2}, scene);
        label(scene, sx + 16, lineY + 69, "extra node", 11, C.amberS, "start", "bold");
      }
      pins.forEach((v, i) => {
        grab(el("circle", {cx: X(v), cy: lineY, r: 10, fill: C.movF, stroke: C.movS, "stroke-width": 2.5}, scene), i);
      });
      const c = cost();
      const ty = 316;
      stat(scene, 60, ty, "HPWL in x", fmt(hpwl()), C.hiS);
      stat(scene, 230, ty, "squared cost Σ w · length²", fmt(Math.round(c * 1000) / 1000), C.amberS);
      stat(scene, 450, ty, "edges", model === "star" ? `${pins.length}, plus a new unknown` : String(list.length));
      let status = "Here the squared cost has nothing to do with HPWL.", color = C.red;
      if (model === "b2b") {
        const exact = Math.abs(c - hpwl()) < 1e-6;
        status = exact ? "The weights fit this placement: the squared cost equals HPWL."
          : "A pin moved but the weights did not: press Rebuild.";
        color = exact ? C.tealS : C.red;
      }
      label(scene, 60, ty + 70, status, 13, color, "start", "bold");
    }
    choice(host, [["clique", "Clique"], ["star", "Star"], ["b2b", "Bound2Bound"]], model, (key) => {
      model = key;
      rebuild();
      draw();
    });
    button(host, "Rebuild", () => { rebuild(); draw(); }, true);
    spacer(host);
    button(host, "+ pin", () => {
      if (pins.length < 8) pins.push(Math.round(Math.random() * 40) / 4);
      rebuild();
      draw();
    });
    button(host, "− pin", () => { if (pins.length > 2) pins.pop(); rebuild(); draw(); });
    rebuild();
    draw();
  }

  const WIDGETS = {placerow: placeRowWidget, "optimal-region": optimalRegionWidget, spring: springWidget,
    "spring-network": springNetworkWidget, collapse: collapseWidget, anchor: anchorWidget, hpwl: hpwlWidget,
    gradient: gradientWidget, shape: shapeWidget, "big-o": bigOWidget, stamp: stampWidget, jacobi: jacobiWidget,
    descent: descentWidget, "net-model": netModelWidget};
  function init() {
    document.querySelectorAll(".kt-widget[data-widget]").forEach((host) => {
      const make = WIDGETS[host.dataset.widget];
      if (make) make(host);
    });
  }
  if (typeof document$ !== "undefined") {
    document$.subscribe(init);
  } else {
    document.addEventListener("DOMContentLoaded", init);
  }
})();
