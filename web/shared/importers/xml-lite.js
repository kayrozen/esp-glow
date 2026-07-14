// xml-lite.js -- dependency-free XML parser shared by the QLC+ and GDTF
// importers.
//
// Why not the browser's DOMParser (as the original importer spec
// suggested)? Because this module also has to run unmodified under Node
// for the importer test suite (test-importers.mjs) -- there's no DOMParser
// there, and this repo doesn't do npm/node_modules (see every other
// hand-rolled parser: osc_parser.cpp, djlink_parser.cpp, midi_realtime.cpp).
// One parser, one behavior, testable outside a browser -- same reasoning,
// applied to JS instead of C++.
//
// Deliberately not a general-purpose XML parser: no namespace resolution
// (tag/attribute names are kept as literal strings, which is fine --
// QLC+ and GDTF files don't use namespace prefixes on elements), no DTD
// processing, no external entities. Comments, CDATA, the XML prolog and
// DOCTYPE are skipped. That covers every real fixture file this importer
// has been pointed at.

const CLOSED_ENTITIES = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'" };

function decodeEntities(s) {
  if (s.indexOf("&") === -1) return s;
  return s.replace(/&(#x[0-9a-fA-F]+|#[0-9]+|[a-zA-Z]+);/g, (m, body) => {
    if (body[0] === "#") {
      const code = body[1] === "x" || body[1] === "X"
        ? parseInt(body.slice(2), 16)
        : parseInt(body.slice(1), 10);
      return Number.isFinite(code) ? String.fromCodePoint(code) : m;
    }
    return Object.prototype.hasOwnProperty.call(CLOSED_ENTITIES, body) ? CLOSED_ENTITIES[body] : m;
  });
}

export class XmlElement {
  constructor(tag, attrs) {
    this.tag = tag;
    this.attrs = attrs || {};
    this.kids = []; // XmlElement[] (text nodes are not kept as nodes; see `.text`)
    this._text = ""; // concatenated direct text content
  }

  attr(name, fallback = undefined) {
    return Object.prototype.hasOwnProperty.call(this.attrs, name) ? this.attrs[name] : fallback;
  }

  // Direct children, optionally filtered by tag name.
  children(tag = null) {
    return tag == null ? this.kids.slice() : this.kids.filter((k) => k.tag === tag);
  }

  // First direct child matching tag, or null.
  child(tag) {
    return this.kids.find((k) => k.tag === tag) || null;
  }

  // First descendant (depth-first, any depth) matching tag, or null.
  find(tag) {
    for (const k of this.kids) {
      if (k.tag === tag) return k;
      const deep = k.find(tag);
      if (deep) return deep;
    }
    return null;
  }

  // All descendants (depth-first, any depth) matching tag.
  findAll(tag, out = []) {
    for (const k of this.kids) {
      if (k.tag === tag) out.push(k);
      k.findAll(tag, out);
    }
    return out;
  }

  // Direct text content (trimmed). Does not descend into child elements.
  get text() {
    return this._text.trim();
  }
}

// Parses `str` and returns the document (root) element. Throws on
// structurally malformed XML (mismatched tags, unclosed input) -- callers
// should catch and surface as an import error, not let it propagate as an
// uncaught exception.
export function parseXML(str) {
  let i = 0;
  const n = str.length;
  const stack = [];
  let root = null;

  function skipWs() {
    while (i < n && /\s/.test(str[i])) i++;
  }

  function parseAttrs(tagStr) {
    const attrs = {};
    const re = /([^\s=/]+)\s*=\s*("([^"]*)"|'([^']*)')/g;
    let m;
    while ((m = re.exec(tagStr))) {
      const val = m[3] !== undefined ? m[3] : m[4];
      attrs[m[1]] = decodeEntities(val);
    }
    return attrs;
  }

  while (i < n) {
    const lt = str.indexOf("<", i);
    if (lt === -1) break;

    if (lt > i && stack.length) {
      stack[stack.length - 1]._text += decodeEntities(str.slice(i, lt));
    }

    if (str.startsWith("<!--", lt)) {
      const end = str.indexOf("-->", lt + 4);
      i = end === -1 ? n : end + 3;
      continue;
    }
    if (str.startsWith("<![CDATA[", lt)) {
      const end = str.indexOf("]]>", lt + 9);
      const content = end === -1 ? str.slice(lt + 9) : str.slice(lt + 9, end);
      if (stack.length) stack[stack.length - 1]._text += content;
      i = end === -1 ? n : end + 3;
      continue;
    }
    if (str.startsWith("<!", lt) || str.startsWith("<?", lt)) {
      // DOCTYPE or XML/processing-instruction declaration. Both can
      // contain nested '>' inside brackets (DOCTYPE's internal subset);
      // a plain indexOf(">") is good enough for every file this importer
      // has actually been pointed at (none use an internal DTD subset).
      const end = str.indexOf(">", lt + 2);
      i = end === -1 ? n : end + 1;
      continue;
    }

    // Closing tag.
    if (str[lt + 1] === "/") {
      const end = str.indexOf(">", lt);
      if (end === -1) throw new Error("xml-lite: unterminated closing tag");
      const name = str.slice(lt + 2, end).trim();
      if (!stack.length || stack[stack.length - 1].tag !== name) {
        throw new Error(`xml-lite: mismatched closing tag </${name}>`);
      }
      stack.pop();
      i = end + 1;
      continue;
    }

    // Opening (or self-closing) tag. Find the matching '>' that isn't
    // inside a quoted attribute value.
    let end = lt + 1;
    let inQuote = null;
    while (end < n) {
      const c = str[end];
      if (inQuote) {
        if (c === inQuote) inQuote = null;
      } else if (c === '"' || c === "'") {
        inQuote = c;
      } else if (c === ">") {
        break;
      }
      end++;
    }
    if (end >= n) throw new Error("xml-lite: unterminated opening tag");

    const selfClosing = str[end - 1] === "/";
    const inner = str.slice(lt + 1, selfClosing ? end - 1 : end);
    const nameMatch = /^[^\s/>]+/.exec(inner);
    if (!nameMatch) throw new Error("xml-lite: malformed tag");
    const tagName = nameMatch[0];
    const attrs = parseAttrs(inner.slice(tagName.length));

    const elem = new XmlElement(tagName, attrs);
    if (stack.length) stack[stack.length - 1].kids.push(elem);
    else if (root) throw new Error("xml-lite: multiple root elements");
    else root = elem;

    if (!selfClosing) stack.push(elem);
    i = end + 1;
  }

  if (stack.length) throw new Error(`xml-lite: unclosed tag <${stack[stack.length - 1].tag}>`);
  if (!root) throw new Error("xml-lite: no root element");
  return root;
}
