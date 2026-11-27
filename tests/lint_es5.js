'use strict';
/* The Wine / Gecko build runs page scripts in Wine's JScript, which is ES5. This keeps every script that ships to such
   a page (and the UI, which runs there too) inside that language:
     - the file must parse as ES5 (no arrow functions, let/const, classes, template strings, spread, async, ...);
     - reserved words are never used as unquoted property names (old engines reject  x.catch  and  {default: 1});
     - library calls from ES2015+ are only used where a polyfill for them ships (src/ui/compat.js for the UI;
       nothing for scripts that run inside arbitrary web pages). */
const acorn = require('acorn'), fs = require('fs'), path = require('path');
const ROOT = path.resolve(__dirname, '..');
let fails = 0, total = 0;
const check = (c, m) => { total++; if (!c) { fails++; console.log('  FAIL: ' + m); } };

const RESERVED = new Set('break case catch class const continue debugger default delete do else enum export extends false finally for function if import in instanceof new null return super switch this throw true try typeof var void while with let static yield await implements interface package private protected public'.split(' '));

/* what the UI may call because compat.js adds it */
const UI_OK_METHODS = new Set(['find', 'findIndex', 'includes', 'fill', 'startsWith', 'endsWith', 'repeat', 'padStart', 'padEnd', 'closest', 'matches', 'remove', 'finally', 'assign', 'values', 'entries', 'from', 'isInteger', 'isFinite', 'isNaN', 'trunc', 'sign', 'resolve', 'reject', 'all', 'race']);
const UI_OK_NEW = new Set(['Promise', 'WeakMap']);
/* never available in the engine, with or without compat.js */
const DENY_METHODS = new Set(['flat', 'flatMap', 'at', 'replaceAll', 'matchAll', 'trimStart', 'trimEnd', 'fromEntries', 'getOwnPropertyDescriptors', 'allSettled', 'any', 'toSorted', 'toReversed', 'structuredClone', 'randomUUID', 'append', 'prepend', 'before', 'after', 'replaceWith', 'toggleAttribute', 'getAttributeNames']);
const DENY_NEW = new Set(['Map', 'Set', 'WeakSet', 'Proxy', 'URL', 'URLSearchParams', 'MutationObserver', 'ResizeObserver', 'IntersectionObserver', 'TextEncoder', 'TextDecoder', 'AbortController', 'CustomEvent', 'Intl']);
const DENY_IDENT = new Set(['Symbol', 'Reflect', 'fetch', 'queueMicrotask', 'globalThis', 'BigInt']);

function scan(file, mode) {
  const src = fs.readFileSync(file, 'utf8'), rel = path.relative(ROOT, file);
  let ast;
  try { ast = acorn.parse(src, { ecmaVersion: 5 }); } catch (e) { check(false, rel + ' is not ES5: ' + e.message); return; }
  check(true, rel);
  /* reserved words as property names */
  (function walk(n) {
    if (!n || typeof n.type !== 'string') return;
    if (n.type === 'MemberExpression' && !n.computed && RESERVED.has(n.property.name)) check(false, rel + ':' + src.slice(0, n.property.start).split('\n').length + ' unquoted reserved property .' + n.property.name);
    if (n.type === 'Property' && !n.computed && n.key.type === 'Identifier' && RESERVED.has(n.key.name)) check(false, rel + ':' + src.slice(0, n.key.start).split('\n').length + ' unquoted reserved key ' + n.key.name);
    for (const k of Object.keys(n)) { const v = n[k]; if (Array.isArray(v)) v.forEach(walk); else if (v && typeof v.type === 'string') walk(v); }
  })(ast);
  /* library use, from tokens so strings and comments are ignored */
  const toks = [...acorn.tokenizer(src, { ecmaVersion: 5, locations: true })];
  for (let i = 0; i < toks.length; i++) {
    const t = toks[i], line = t.loc.start.line;
    if (src.split('\n')[line - 1].indexOf('fb-lint-ok') >= 0) continue;      // a line that is guarded by feature detection
    if (t.type.label === 'name') {
      const prev = toks[i - 1], next = toks[i + 1];
      const isMember = prev && prev.type.label === '.';
      if (isMember && next && next.type.label === '(') {
        if (DENY_METHODS.has(t.value)) check(false, `${rel}:${line} .${t.value}() is not available in the engine`);
        else if (mode === 'page' && UI_OK_METHODS.has(t.value) && !['remove', 'matches', 'closest', 'find', 'all', 'race', 'resolve', 'reject', 'from', 'values', 'entries', 'assign'].includes(t.value) && prev) {
          if (['includes', 'startsWith', 'endsWith', 'repeat', 'padStart', 'padEnd', 'fill', 'findIndex', 'finally', 'isInteger', 'trunc', 'sign'].includes(t.value)) check(false, `${rel}:${line} .${t.value}() needs a polyfill, none ships for page scripts`);
        }
      }
      if (prev && prev.type.keyword === 'new') {
        if (DENY_NEW.has(t.value)) check(false, `${rel}:${line} new ${t.value} is not available in the engine`);
        if (mode === 'page' && UI_OK_NEW.has(t.value)) check(false, `${rel}:${line} new ${t.value} needs a polyfill, none ships for page scripts`);
      }
      if (!isMember && DENY_IDENT.has(t.value) && next && !(next.type.label === ':' )) check(false, `${rel}:${line} ${t.value} is not available in the engine`);
      /* Object.assign / Array.from / Number.is* / Promise.* in page scripts */
      if (mode === 'page' && !isMember && ['Promise'].includes(t.value)) check(false, `${rel}:${line} Promise is not available to page scripts`);
      if (mode === 'page' && !isMember && ['Object', 'Array', 'Number', 'Math'].includes(t.value) && next && next.type.label === '.' && toks[i + 2]) {
        const m = toks[i + 2].value;
        if (['assign', 'values', 'entries', 'from', 'of', 'isInteger', 'isFinite', 'isNaN', 'trunc', 'sign'].includes(m) && !(t.value === 'Number' && m === 'isFinite' && false)) check(false, `${rel}:${line} ${t.value}.${m} needs a polyfill, none ships for page scripts`);
      }
    }
  }
}

const page = ['src/js/fb_runtime.js', 'src/js/fb_tools.js', 'src/js/fb_legacy.js'];
page.forEach(f => scan(path.join(ROOT, f), 'page'));
fs.readdirSync(path.join(ROOT, 'src/ui')).filter(f => f.endsWith('.js') && f !== 'compat.js').forEach(f => scan(path.join(ROOT, 'src/ui', f), 'ui'));
scan(path.join(ROOT, 'src/ui/compat.js'), 'compat');

/* inline <script> blocks of the HTML pages */
for (const f of fs.readdirSync(path.join(ROOT, 'src/ui')).filter(f => f.endsWith('.html'))) {
  const html = fs.readFileSync(path.join(ROOT, 'src/ui', f), 'utf8');
  const re = /<script>([\s\S]*?)<\/script>/g; let m, n = 0;
  while ((m = re.exec(html))) { n++; try { acorn.parse(m[1], { ecmaVersion: 5 }); check(true, f + ' inline script ' + n); } catch (e) { check(false, f + ' inline script ' + n + ' is not ES5: ' + e.message); } }
  /* every UI page that runs scripts loads compat.js first */
  if (/<script src=/.test(html)) check(/<script src="compat\.js"><\/script>[\s\S]*<script src="common\.js">/.test(html), f + ' loads compat.js before common.js');
}
console.log(`${total} checks, ${fails} failed`);
process.exit(fails ? 1 : 0);
