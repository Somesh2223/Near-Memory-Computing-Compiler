/* ============================================================================
 * web/app.js  --  the NEMO-C workbench in the browser
 *
 * Every button runs the real compiler: the editor's text is sent to
 * web/server.py, which runs  ./nemoc <file> <flags>  and returns what it
 * printed plus any files it wrote (Graphviz graphs, DPU assembly, host C).
 * Nothing here re-implements the compiler; this file only shows its output.
 * ==========================================================================*/
'use strict';

const PHASES = [
  { id: 'A', name: 'Front end',
    blurb: 'Lexer (flex DFA, maximal munch), LALR(1) parser (bison, 0 conflicts, panic-mode error recovery), scoped symbol table and semantic checks. Every error is reported with line and column; click one to jump to it.',
    tryFiles: ['tests/bad_syntax.nmc', 'tests/bad_semantics.nmc', 'tests/bad_token.nmc'],
    ops: [['tokens', 'Token stream'], ['ast', 'Parse tree (AST)'], ['check', 'Semantic check']] },
  { id: 'B', name: 'IR + CFG',
    blurb: 'Three-address code with one operation per line and temporaries %t0, %t1, ... Loops stay nested for the loop analyses; a flat control-flow graph (basic blocks, back edges, merge blocks) is built on demand.',
    tryFiles: ['benchmarks/saxpy.nmc'],
    ops: [['ir', 'Three-address IR'], ['cfg', 'Control-flow graph']] },
  { id: 'C', name: 'Optimise',
    blurb: 'Constant folding (forward dataflow, lattice UNDEF / CONST / NAC), common-subexpression elimination, dead-code elimination (backward liveness), induction variables and strength reduction. Each pass prints the IR before, its analysis, and the IR after.',
    tryFiles: ['tests/opt_demo.nmc'],
    ops: [['opt', 'All passes'], ['constfold', 'Constant folding'], ['cse', 'CSE'], ['dce', 'Dead code'], ['indvar', 'Induction variables'], ['strength', 'Strength reduction']] },
  { id: 'D', name: 'Dependence',
    blurb: 'Access matrices F and offsets c for every array reference, the GCD and Banerjee tests (each verdict reported on its own), direction vectors, and which loops are parallel.',
    tryFiles: ['tests/dep_banerjee.nmc', 'benchmarks/stencil2d.nmc'],
    ops: [['affine', 'Access matrices'], ['deps', 'Dependence tests'], ['depsdot', 'Dependence graph']] },
  { id: 'E', name: 'Decide',
    blurb: 'The offload decision. A nest goes to PIM only if all six checks pass: arithmetic intensity below 0.5, no significant reuse, a parallel outer loop, analysable accesses, a partition with no all-to-all shuffle, and enough work to pay the 80,000-cycle launch.',
    tryFiles: ['benchmarks/matmul.nmc', 'benchmarks/saxpy.nmc', 'benchmarks/transpose.nmc', 'tests/tiny_size.nmc'],
    ops: [['decisions', 'Decisions'], ['explain', 'Explain']] },
  { id: 'F', name: 'Transform',
    blurb: 'Loop fission (Allen-Kennedy with Tarjan strongly connected components), loop interchange (legal while every direction vector stays lexicographically non-negative) and loop fusion. Each result is decided again.',
    tryFiles: ['benchmarks/mixed_loop.nmc', 'benchmarks/colmajor.nmc', 'benchmarks/fusion_chain.nmc'],
    ops: [['transformations', 'Transformations']] },
  { id: 'G', name: 'Partition',
    blurb: 'Owner-computes partitioning across 64 DPUs with halo rows; block, cyclic and block-cyclic are costed. Flush and invalidate operations are placed by a forward dataflow analysis over the host CFG (meet = union).',
    tryFiles: ['benchmarks/stencil1d.nmc', 'tests/coherence_demo.nmc'],
    ops: [['partition', 'Partitioning'], ['coherence', 'Coherence']] },
  { id: 'H', name: 'Codegen',
    blurb: 'PIM assembly with scratchpad tiling, Chaitin-Briggs graph-colouring register allocation (24 registers; set fewer below to force spills), and the host C that flushes, copies data in, launches and copies results out.',
    tryFiles: ['benchmarks/stencil1d.nmc', 'benchmarks/stencil2d.nmc'],
    ops: [['codegen', 'Generate code'], ['regalloc', 'Register allocation'], ['interference', 'Interference graph']] },
  { id: 'I', name: 'Simulate',
    blurb: 'Runs HOST_ONLY, NAIVE_OFFLOAD and NEMO_C on the simulator, checks all three are bit-identical, and also executes the generated DPU code. Takes 5 to 20 seconds.',
    tryFiles: ['benchmarks/matmul.nmc', 'benchmarks/saxpy.nmc', 'benchmarks/colmajor.nmc'],
    ops: [['simulate', 'Simulate']] }
];

const GRAPH_OPS = { cfg: 1, depsdot: 1, interference: 1 };
/* single-pass optimisation dumps: shown with the lines each pass changed highlighted */
const PASS_OPS = { constfold: 1, cse: 1, dce: 1, indvar: 1, strength: 1 };
const LINE_PX = 20;   /* editor line height, matches style.css */

const state = {
  files: [], path: null, original: '', phase: 'A', lastOp: null,
  nests: [], kernel: '', busy: false, result: null, errorLines: {}, view: 'compiler'
};

const $ = id => document.getElementById(id);

function store(k, v) { try { localStorage.setItem('nemoc.' + k, v); } catch (e) { /* storage unavailable */ } }
function recall(k) { try { return localStorage.getItem('nemoc.' + k); } catch (e) { return null; } }

function esc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

async function api(path, body) {
  const opt = body
    ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }
    : {};
  let r;
  try {
    r = await fetch(path, opt);
  } catch (e) {
    throw new Error('cannot reach the server; is  make web  still running?');
  }
  const j = await r.json().catch(() => ({ error: 'unexpected response from the server' }));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}

function setBusy(text) {
  state.busy = !!text;
  $('busy').hidden = !text;
  $('busy-text').textContent = text || '';
  document.querySelectorAll('.op, #build-btn, #runall-btn, #test-btn').forEach(b => { b.disabled = !!text; });
}

/* ---- status and build ------------------------------------------------------- */
async function refreshStatus() {
  const pill = $('build-pill');
  try {
    const s = await api('/api/status');
    pill.textContent = s.built ? 'nemoc built' : 'nemoc not built';
    pill.className = 'pill ' + (s.built ? 'ok' : 'bad');
  } catch (e) {
    pill.textContent = 'server offline';
    pill.className = 'pill bad';
  }
}

async function runMake(target) {
  const labels = { all: 'make', 'run-all': 'make run-all', test: 'make test' };
  const waits = { all: 'Building nemoc', 'run-all': 'Running all 15 benchmarks (a few minutes)', test: 'Running the unit tests (about 8 minutes)' };
  if (target === 'test' && !confirm('make test runs 8 test programs and takes about 8 minutes. Start it?')) return;
  setBusy(waits[target] + '...');
  try {
    const r = await api('/api/make', { target: target });
    const ok = r.exit === 0;
    const text = r.output || '(no output)';
    if (target === 'all') {
      showText('$ ' + r.cmd, text, ok, r.ms);
    } else {
      $('make-block').hidden = false;
      $('make-title').innerHTML = esc(labels[target]) + ' &middot; ' +
        (ok ? '<span class="b-ok">passed</span>' : '<span class="b-bad">failed</span>') +
        ' <span class="muted">in ' + fmtMs(r.ms) + '</span>';
      $('make-out').innerHTML = highlight(text, 'text');
      if (target === 'run-all') await loadResults();
      $('make-block').scrollIntoView({ behavior: 'smooth', block: 'start' });
    }
  } catch (e) {
    alert(e.message);
  } finally {
    setBusy(null);
    refreshStatus();
  }
}

/* ---- the program picker and editor --------------------------------------------- */
function baseName(path) {
  const b = (path || 'program').split('/').pop();
  return b.replace(/\.nmc$/, '');
}

async function loadFileList() {
  const r = await api('/api/files');
  state.files = r.files;
  const sel = $('file');
  sel.innerHTML = '';
  [['benchmarks/', 'Benchmarks'], ['tests/', 'Demo inputs'], ['lab/', 'Lab exercises']].forEach(([prefix, label]) => {
    const g = document.createElement('optgroup');
    g.label = label;
    r.files.filter(f => f.startsWith(prefix)).forEach(f => {
      const o = document.createElement('option');
      o.value = f;
      o.textContent = f.slice(prefix.length);
      g.appendChild(o);
    });
    sel.appendChild(g);
  });
  const want = recall('file');
  return state.files.includes(want) ? want : 'benchmarks/saxpy.nmc';
}

async function openFile(path, askFirst) {
  if (askFirst && isDirty() && !confirm('Discard your edits to ' + baseName(state.path) + '?')) {
    $('file').value = state.path;
    return;
  }
  const r = await api('/api/file?path=' + encodeURIComponent(path));
  state.path = r.path;
  state.original = r.source;
  state.errorLines = {};
  $('file').value = r.path;
  $('src').value = r.source;
  $('src').scrollTop = 0;
  store('file', r.path);
  onEdit();
  state.nests = [];
  renderKernelPick();
  clearOutput('Loaded ' + r.path + '. Pick an operation.');
}

function isDirty() { return $('src').value !== state.original; }

function onEdit() {
  $('dirty').hidden = !isDirty();
  renderGutter();
  renderKernelPick();
}

function renderGutter() {
  const n = $('src').value.split('\n').length;
  const parts = [];
  for (let i = 1; i <= n; i++)
    parts.push(state.errorLines[i] ? '<span class="hit">' + i + '</span>' : String(i));
  $('gutter').innerHTML = parts.join('\n') + '\n\n';
  $('gutter').scrollTop = $('src').scrollTop;
}

function jumpTo(line) {
  const src = $('src');
  const lines = src.value.split('\n');
  if (line < 1 || line > lines.length) return;
  let off = 0;
  for (let i = 0; i < line - 1; i++) off += lines[i].length + 1;
  src.focus();
  src.setSelectionRange(off, off + lines[line - 1].length);
  src.scrollTop = Math.max(0, (line - 4) * LINE_PX);
  renderGutter();
}

function kernelNames() {
  const names = [];
  const re = /\bkernel\s+([A-Za-z_]\w*)/g;
  let m;
  while ((m = re.exec($('src').value)) !== null) names.push(m[1]);
  state.nests.forEach(n => { if (!names.includes(n)) names.push(n); });
  return names;
}

function renderKernelPick() {
  const sel = $('kernel');
  if (!sel) return;
  const names = kernelNames();
  sel.innerHTML = names.map(n => '<option>' + esc(n) + '</option>').join('');
  if (names.includes(state.kernel)) sel.value = state.kernel;
  else state.kernel = sel.value;
}

/* ---- phases and operations ------------------------------------------------------- */
function renderPhases() {
  $('phases').innerHTML = PHASES.map(p =>
    '<button class="phase' + (p.id === state.phase ? ' active' : '') + '" data-phase="' + p.id + '">' +
    '<b>' + p.id + '</b>' + esc(p.name) + '</button>').join('');
  const p = PHASES.find(x => x.id === state.phase);
  $('phase-title').textContent = 'Phase ' + p.id + ' · ' + p.name;
  const tries = p.tryFiles.filter(f => state.files.includes(f))
    .map(f => '<a href="#" class="try" data-file="' + esc(f) + '">' + esc(baseName(f)) + '</a>').join(', ');
  $('phase-blurb').innerHTML = esc(p.blurb) + (tries ? ' <span class="muted">Good examples: ' + tries + '.</span>' : '');
  let html = p.ops.map(([id, label]) =>
    '<button class="op' + (state.lastOp === id ? ' active' : '') + '" data-op="' + id + '">' + esc(label) + '</button>').join('');
  if (p.id === 'E')
    html += '<span class="kernel-pick">kernel <select id="kernel" aria-label="Kernel or nest to explain"></select></span>';
  $('ops').innerHTML = html;
  renderKernelPick();
  if (state.busy) setBusy($('busy-text').textContent);
}

function options() {
  return {
    noOpt: $('opt-noopt').checked,
    dist: $('opt-dist').value,
    regs: $('opt-regs').value
  };
}

async function runOp(op) {
  if (state.busy) return;
  state.lastOp = op;
  renderPhases();
  const req = { action: op, source: $('src').value, name: baseName(state.path), options: options() };
  if (op === 'explain') {
    req.arg = state.kernel;
    if (!req.arg) { alert('This program has no kernel to explain.'); return; }
  }
  $('cmd').textContent = 'running ' + op + '...';
  setBusy(op === 'simulate' ? 'Simulating three configurations...' : 'Running nemoc...');
  try {
    const r = await api('/api/run', req);
    showResult(op, r);
  } catch (e) {
    showText('error', e.message, false, 0);
  } finally {
    setBusy(null);
  }
}

/* ---- output ------------------------------------------------------------------------- */
function fmtMs(ms) { return ms >= 1000 ? (ms / 1000).toFixed(1) + ' s' : ms + ' ms'; }

function clearOutput(msg) {
  state.result = null;
  $('cmd').textContent = msg;
  $('cmd').className = '';
  $('meta').innerHTML = '';
  $('out-tabs').hidden = true;
  $('graph').hidden = true;
  $('out').hidden = false;
  $('out').innerHTML = '';
}

function showText(cmd, text, ok, ms) {
  state.result = null;
  $('cmd').textContent = cmd;
  $('cmd').className = 'live';
  $('meta').innerHTML = (ok ? '<span class="ok">ok</span>' : '<span class="bad">failed</span>') + (ms ? ' · ' + fmtMs(ms) : '');
  $('out-tabs').hidden = true;
  $('graph').hidden = true;
  $('out').hidden = false;
  $('out').innerHTML = highlight(text, 'text');
}

function showResult(op, r) {
  state.result = r;
  const text = (r.stdout || '') + (r.stderr ? (r.stdout ? '\n' : '') + r.stderr : '');
  r.text = text || '(no output)';
  r.op = op;

  /* error positions -> gutter marks */
  state.errorLines = {};
  const re = /\.nmc:(\d+):(\d+)/g;
  let m;
  while ((m = re.exec(text)) !== null) state.errorLines[+m[1]] = true;
  renderGutter();

  /* nests listed by --decisions feed the Explain picker */
  if (op === 'decisions') {
    const found = [];
    r.stdout.split('\n').forEach(l => {
      const d = /^(\S+)\s+(OFFLOAD|REJECT)\b/.exec(l);
      if (d) found.push(d[1]);
    });
    state.nests = found;
    renderKernelPick();
  }

  $('cmd').textContent = '$ ' + r.cmd;
  $('cmd').className = 'live';
  $('meta').innerHTML = (r.exit === 0 ? '<span class="ok">exit 0</span>' : '<span class="bad">exit ' + r.exit + '</span>') +
    ' · ' + fmtMs(r.ms);

  const tabs = [{ key: 'out', label: 'output' }].concat(r.files.map((f, i) => ({ key: 'f' + i, label: f.name.split('/').pop() })));
  $('out-tabs').hidden = tabs.length < 2;
  $('out-tabs').innerHTML = tabs.map(t => '<button class="out-tab" data-tab="' + t.key + '">' + esc(t.label) + '</button>').join('');
  let first = 'out';
  if (r.exit === 0 && r.files.length > 0 && (GRAPH_OPS[op] || op === 'codegen')) first = 'f0';
  showTab(first);
}

function showTab(key) {
  const r = state.result;
  if (!r) return;
  document.querySelectorAll('.out-tab').forEach(b => b.classList.toggle('active', b.dataset.tab === key));
  if (key === 'out') {
    $('graph').hidden = true;
    $('out').hidden = false;
    $('out').innerHTML = (PASS_OPS[r.op] && r.exit === 0) ? passDiff(r.text) : highlight(r.text, 'text');
    $('out').scrollTop = 0;
    return;
  }
  const f = r.files[+key.slice(1)];
  if (f.kind === 'dot') {
    showGraph(f);
  } else {
    $('graph').hidden = true;
    $('out').hidden = false;
    $('out').innerHTML = highlight(f.text, f.kind);
    $('out').scrollTop = 0;
  }
}

/* Graphviz: drawn by the server when `dot` is installed, else by viz.js in the browser */
let vizPromise = null;
function loadViz() {
  if (!vizPromise) {
    vizPromise = new Promise((resolve, reject) => {
      const s = document.createElement('script');
      s.src = 'https://cdn.jsdelivr.net/npm/@viz-js/viz@3/lib/viz-standalone.js';
      s.onload = () => (window.Viz ? window.Viz.instance().then(resolve, reject) : reject(new Error('viz.js did not load')));
      s.onerror = () => { vizPromise = null; reject(new Error('no internet connection for viz.js')); };
      document.head.appendChild(s);
    });
  }
  return vizPromise;
}

async function showGraph(f) {
  const g = $('graph');
  $('out').hidden = true;
  g.hidden = false;
  if (f.svg) { g.innerHTML = f.svg; return; }
  g.innerHTML = '<p class="note">Drawing ' + esc(f.name) + '...</p>';
  try {
    const viz = await loadViz();
    const svg = viz.renderSVGElement(f.text);
    g.innerHTML = '<p class="note">' + esc(f.name) + '</p>';
    g.appendChild(svg);
  } catch (e) {
    g.innerHTML = '<p class="note">Could not draw the graph (' + esc(e.message) +
      '). Drawing needs internet access, or Graphviz in WSL: <code>sudo apt install graphviz</code>. The Graphviz source:</p><pre>' +
      esc(f.text) + '</pre>';
  }
}

/* One optimisation pass prints "IR BEFORE", its analysis, then "IR AFTER".
 * Compare the two IR listings line by line (longest common subsequence) and
 * mark what the pass removed (red, in BEFORE) and what it added or changed
 * (green, in AFTER).  Source positions @line:col are ignored when comparing. */
function passDiff(text) {
  const lines = text.split('\n');
  const find = re => lines.findIndex(l => re.test(l));
  const iB = find(/^==== \w+: IR BEFORE/), iA = find(/^==== \w+: analysis/), iF = find(/^==== \w+: IR AFTER/);
  if (iB < 0 || iA < 0 || iF < 0) return highlight(text, 'text');

  const key = l => l.replace(/@\d+:\d+\s*$/, '').replace(/\s+/g, ' ').trim();
  const before = lines.slice(iB + 1, iA), after = lines.slice(iF + 1);
  const n = before.length, m = after.length;
  /* lcs[i][j] = length of the longest common subsequence of before[i..] and after[j..] */
  const lcs = [];
  for (let i = 0; i <= n; i++) lcs.push(new Array(m + 1).fill(0));
  for (let i = n - 1; i >= 0; i--)
    for (let j = m - 1; j >= 0; j--)
      lcs[i][j] = key(before[i]) === key(after[j]) ? lcs[i + 1][j + 1] + 1 : Math.max(lcs[i + 1][j], lcs[i][j + 1]);
  const removed = new Array(n).fill(false), added = new Array(m).fill(false);
  let i = 0, j = 0;
  while (i < n && j < m) {
    if (key(before[i]) === key(after[j])) { i++; j++; }
    else if (lcs[i + 1][j] >= lcs[i][j + 1]) removed[i++] = true;
    else added[j++] = true;
  }
  while (i < n) removed[i++] = true;
  while (j < m) added[j++] = true;

  const row = (l, cls) => '<span class="ln' + (cls ? ' ' + cls : '') + '">' + (highlight(l, 'text') || '​') + '</span>';
  const out = [];
  out.push(row('Highlighted: red = removed by this pass, green = added or changed by it.', 't-dim'));
  for (let k = 0; k < lines.length; k++) {
    if (k > iB && k < iA) out.push(row(lines[k], removed[k - iB - 1] && lines[k].trim() ? 'd-del' : ''));
    else if (k > iF) out.push(row(lines[k], added[k - iF - 1] && lines[k].trim() ? 'd-add' : ''));
    else out.push(row(lines[k], ''));
  }
  return out.join('');
}

/* syntax colouring of compiler output, DPU assembly and C */
function highlight(text, kind) {
  return text.split('\n').map(line => {
    let h = esc(line);
    if (kind === 'pim') {
      if (/^\s*;/.test(line)) return '<span class="t-dim">' + h + '</span>';
      return h.replace(/(;.*)$/, '<span class="t-dim">$1</span>');
    }
    if (kind === 'c') {
      if (/^\s*(\/\*|\*|\/\/)/.test(line)) return '<span class="t-dim">' + h + '</span>';
      return h.replace(/(\/\*.*\*\/|\/\/.*)$/, '<span class="t-dim">$1</span>');
    }
    h = h.replace(/([\w\/.-]+\.nmc):(\d+):(\d+)/g, '<a class="loc" data-line="$2">$1:$2:$3</a>');
    if (/\berror\b|cannot |NOT VERIFIED|SOME TESTS FAILED|MISMATCH/i.test(line))
      return '<span class="t-err">' + h + '</span>';
    h = h.replace(/\[pass\]/g, '<span class="b-ok">[pass]</span>')
      .replace(/\b(OFFLOAD(?:ED)?|INDEPENDENT|PARALLEL|bit-identical|OK|PASSED)\b/g, '<span class="b-ok">$1</span>')
      .replace(/\b(REJECT(?:ED)?|SERIAL|FAIL(?:ED)?)\b/g, '<span class="b-bad">$1</span>')
      .replace(/: (parallel(?: \(reduction\))?)$/, ': <span class="b-ok">$1</span>')
      .replace(/: serial$/, ': <span class="b-bad">serial</span>');
    if (/^(====|---- |Step \d+\.|Verdict:|  round \d+|IR$|=== )/.test(line)) return '<span class="t-head">' + h + '</span>';
    if (/^nemoc: /.test(line)) return '<span class="t-dim">' + h + '</span>';
    return h;
  }).join('\n');
}

/* ---- results view --------------------------------------------------------------------- */
function parseCsv(text) {
  const data = {};
  const order = [];
  text.trim().split('\n').slice(1).forEach(l => {
    const c = l.split(',');
    if (c.length < 4) return;
    const [b, cfg, metric, val] = c;
    if (!data[b]) { data[b] = {}; order.push(b); }
    if (!data[b][cfg]) data[b][cfg] = {};
    data[b][cfg][metric] = +val;
  });
  return { data, order };
}

function parseMdTable(md) {
  return md.split('\n').filter(l => /^\|/.test(l) && !/^\|\s*-/.test(l) && !/^\|\s*benchmark\s*\|/.test(l))
    .map(l => l.replace(/^\||\|$/g, '').split('|').map(s => s.trim()));
}

function speedChart(rows) {
  /* one row per benchmark: NAIVE_OFFLOAD (grey) and NEMO_C (accent) speedup over the host, log scale */
  const lo = Math.log(0.4), hi = Math.log(8);
  const X = v => 170 + (Math.log(Math.min(Math.max(v, 0.4), 8)) - lo) / (hi - lo) * 520;
  const y0 = 78, step = 24;
  const H = y0 + rows.length * step + 36;
  const yb = y0 + rows.length * step;
  let s = '<svg viewBox="0 0 760 ' + H + '" role="img" aria-label="Speedup over the host for every benchmark" font-family="inherit" font-size="12">';
  s += '<text x="24" y="26" font-size="15" font-weight="600" fill="var(--text)">Speedup over HOST_ONLY (log scale; right of the dashed line = faster)</text>';
  s += '<circle cx="30" cy="48" r="5" fill="var(--grey-dot)"/><text x="42" y="52" fill="var(--muted)">NAIVE_OFFLOAD: every parallel loop</text>';
  s += '<circle cx="300" cy="48" r="5" fill="var(--accent)"/><text x="312" y="52" fill="var(--text)">NEMO_C: our cost model</text>';
  [0.5, 1, 2, 4, 8].forEach(t => {
    s += '<line x1="' + X(t) + '" x2="' + X(t) + '" y1="' + (y0 - 12) + '" y2="' + yb + '" stroke="var(--line)"/>';
    s += '<text x="' + X(t) + '" y="' + (yb + 18) + '" text-anchor="middle" fill="var(--muted)">' + t + 'x</text>';
  });
  s += '<line x1="' + X(1) + '" x2="' + X(1) + '" y1="' + (y0 - 12) + '" y2="' + yb + '" stroke="var(--muted)" stroke-dasharray="4 3"/>';
  rows.forEach((r, i) => {
    const y = y0 + i * step + 6;
    const a = X(r.naive), b = X(r.nemo);
    s += '<text x="24" y="' + (y + 4) + '" fill="var(--text)">' + esc(r.bench) + '</text>';
    s += '<line x1="' + Math.min(a, b) + '" x2="' + Math.max(a, b) + '" y1="' + y + '" y2="' + y + '" stroke="var(--line)" stroke-width="3"/>';
    s += '<circle cx="' + a + '" cy="' + y + '" r="5" fill="var(--grey-dot)"><title>' + esc(r.bench) + ' NAIVE_OFFLOAD ' + r.naive.toFixed(2) + 'x</title></circle>';
    s += '<circle cx="' + b + '" cy="' + y + '" r="5" fill="var(--accent)"><title>' + esc(r.bench) + ' NEMO_C ' + r.nemo.toFixed(2) + 'x</title></circle>';
    if (r.naive < 0.99)
      s += '<text x="' + (a - 10) + '" y="' + (y + 4) + '" text-anchor="end" fill="var(--muted)">' + r.naive.toFixed(2) + 'x</text>';
    s += '<text x="' + (Math.max(a, b) + 10) + '" y="' + (y + 4) + '" font-weight="600" fill="var(--text)">' + r.nemo.toFixed(2) + 'x</text>';
  });
  return s + '</svg>';
}

async function loadResults() {
  let r;
  try {
    r = await api('/api/results');
  } catch (e) {
    $('cards').innerHTML = '<p class="muted">' + esc(e.message) + '</p>';
    return;
  }
  if (!r.csv.trim()) {
    $('cards').innerHTML = '<p class="muted">No results yet: press <b>Run all benchmarks</b>.</p>';
    $('speedup-chart').innerHTML = '';
    return;
  }
  const { data, order } = parseCsv(r.csv);
  const rows = order.filter(b => data[b].HOST_ONLY && data[b].NAIVE_OFFLOAD && data[b].NEMO_C).map(b => {
    const d = data[b];
    return {
      bench: b,
      host: d.HOST_ONLY.cycles,
      naive: d.HOST_ONLY.cycles / d.NAIVE_OFFLOAD.cycles,
      nemo: d.HOST_ONLY.cycles / d.NEMO_C.cycles,
      nests: d.NEMO_C.pim_nests,
      busHost: d.HOST_ONLY.bus_bytes,
      busNemo: d.NEMO_C.bus_bytes,
      verified: d.HOST_ONLY.verified && d.NAIVE_OFFLOAD.verified && d.NEMO_C.verified && d.NEMO_C.asm_verified !== 0
    };
  });

  const declined = rows.filter(x => x.nests === 0);
  const best = rows.reduce((a, x) => (x.nemo > a.nemo ? x : a), rows[0]);
  const worstNemo = rows.reduce((a, x) => Math.min(a, x.nemo), Infinity);
  const nVerified = rows.filter(x => x.verified).length;
  const card = (num, lbl) => '<div class="card"><div class="num">' + num + '</div><div class="lbl">' + lbl + '</div></div>';
  $('cards').innerHTML =
    card(rows.length, 'benchmarks simulated') +
    card(nVerified + ' / ' + rows.length, 'bit-identical in every configuration') +
    card(declined.length, 'declined by the cost model (' + declined.map(x => esc(x.bench)).join(', ') + ')') +
    card(best.nemo.toFixed(2) + 'x', 'best speedup (' + esc(best.bench) + ')') +
    card(worstNemo.toFixed(2) + 'x', 'worst NEMO_C speedup: never below the host');

  const sorted = rows.slice().sort((a, b) => b.nemo - a.nemo || b.naive - a.naive);
  $('speedup-chart').innerHTML = speedChart(sorted);

  const sp = v => '<span class="' + (v < 0.995 ? 'b-bad' : 'b-ok') + '">' + v.toFixed(2) + 'x</span>';
  $('bench-table').innerHTML =
    '<thead><tr><th>benchmark</th><th class="num">nests on PIM</th><th class="num">host cycles</th>' +
    '<th class="num">NAIVE_OFFLOAD</th><th class="num">NEMO_C</th><th class="num">bus bytes host / NEMO_C</th><th>checks</th></tr></thead><tbody>' +
    rows.map(x => '<tr><td><a href="#" class="try" data-file="benchmarks/' + esc(x.bench) + '.nmc">' + esc(x.bench) + '</a></td>' +
      '<td class="num">' + x.nests + '</td><td class="num">' + x.host.toLocaleString('en-US') + '</td>' +
      '<td class="num">' + sp(x.naive) + '</td><td class="num">' + sp(x.nemo) + '</td>' +
      '<td class="num">' + x.busHost.toLocaleString('en-US') + ' / ' + x.busNemo.toLocaleString('en-US') + '</td>' +
      '<td>' + (x.verified ? '<span class="b-ok">bit-identical</span>' : '<span class="b-bad">mismatch</span>') + '</td></tr>').join('') +
    '</tbody>';

  const dec = parseMdTable(r.decisions);
  $('dec-table').innerHTML =
    '<thead><tr><th>benchmark</th><th>nest</th><th class="num">AI</th><th>class</th><th>verdict</th><th>reason</th></tr></thead><tbody>' +
    dec.map(c => '<tr><td>' + esc(c[0]) + '</td><td><code>' + esc(c[1]) + '</code></td><td class="num">' + esc(c[2]) + '</td>' +
      '<td>' + esc(c[4]) + '</td><td><span class="' + (c[5] === 'OFFLOAD' ? 'b-ok' : 'b-bad') + '">' + esc(c[5]) + '</span></td>' +
      '<td class="reason">' + esc(c[6] || '') + '</td></tr>').join('') + '</tbody>';

  $('png-block').hidden = r.charts.length === 0;
  $('pngs').innerHTML = r.charts.map(c => '<img alt="' + c + ' chart" src="/results/' + c + '.png?t=' + Date.now() + '">').join('');
}

/* ---- wiring ------------------------------------------------------------------------- */
function showView(v) {
  state.view = v;
  document.querySelectorAll('.view-tab').forEach(b => b.classList.toggle('active', b.dataset.view === v));
  $('view-compiler').hidden = v !== 'compiler';
  $('view-results').hidden = v !== 'results';
  store('view', v);
  if (v === 'results') loadResults();
}

function wire() {
  document.querySelectorAll('.view-tab').forEach(b => b.addEventListener('click', () => showView(b.dataset.view)));
  $('build-btn').addEventListener('click', () => { showView('compiler'); runMake('all'); });
  $('runall-btn').addEventListener('click', () => runMake('run-all'));
  $('test-btn').addEventListener('click', () => runMake('test'));
  $('file').addEventListener('change', e => openFile(e.target.value, true));
  $('reset-btn').addEventListener('click', () => openFile(state.path, true));

  $('phases').addEventListener('click', e => {
    const b = e.target.closest('.phase');
    if (!b) return;
    state.phase = b.dataset.phase;
    store('phase', state.phase);
    renderPhases();
  });
  $('ops').addEventListener('click', e => {
    const b = e.target.closest('.op');
    if (b) runOp(b.dataset.op);
  });
  $('ops').addEventListener('change', e => {
    if (e.target.id === 'kernel') state.kernel = e.target.value;
  });
  document.addEventListener('click', e => {
    const t = e.target.closest('a.try');
    if (t) {
      e.preventDefault();
      showView('compiler');
      openFile(t.dataset.file, true);
      return;
    }
    const loc = e.target.closest('a.loc');
    if (loc) jumpTo(+loc.dataset.line);
  });
  $('out-tabs').addEventListener('click', e => {
    const b = e.target.closest('.out-tab');
    if (b) showTab(b.dataset.tab);
  });

  const src = $('src');
  src.addEventListener('input', onEdit);
  src.addEventListener('scroll', () => { $('gutter').scrollTop = src.scrollTop; });
  src.addEventListener('keydown', e => {
    if (e.key === 'Tab' && !e.shiftKey) {
      e.preventDefault();
      const s = src.selectionStart;
      src.setRangeText('    ', s, src.selectionEnd, 'end');
      onEdit();
    } else if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) {
      e.preventDefault();
      if (state.lastOp) runOp(state.lastOp);
    }
  });
}

async function init() {
  wire();
  const p = recall('phase');
  if (PHASES.some(x => x.id === p)) state.phase = p;
  refreshStatus();
  try {
    const first = await loadFileList();
    renderPhases();
    await openFile(first, false);
  } catch (e) {
    renderPhases();
    clearOutput(e.message);
  }
  if (recall('view') === 'results') showView('results');
}

init();
