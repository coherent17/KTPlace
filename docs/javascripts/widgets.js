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
    el("rect", {x: 0.5, y: 0.5, width: w - 1, height: h - 1, rx: 12, fill: "#ffffff", stroke: "#c7d2fe"}, svg);
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
      const btn = document.createElement("button");
      btn.className = "md-button kt-widget-button";
      btn.textContent = "New random nets";
      btn.addEventListener("click", () => {
        const count = 2 + Math.floor(Math.random() * 3);
        nets = Array.from({length: count}, () => {
          const a = Math.round(Math.random() * 9 * 2) / 2;
          return [a, Math.min(12, a + 0.5 + Math.round(Math.random() * 5 * 2) / 2)];
        });
        draw();
      });
      host.appendChild(btn);
    }
    draw();
  }

  const WIDGETS = {placerow: placeRowWidget, "optimal-region": optimalRegionWidget};
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
