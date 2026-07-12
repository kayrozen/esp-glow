//
// fennel-editor.js — the one CodeMirror 6 component shared by web/console
// (device REPL/editor) and web/provisioner-static (offline authoring). See
// the vendored bundle at web/vendor/editor-bundle.mjs (built by
// scripts/vendor_editor_bundle.sh) for what's included.
//
// Non-negotiable for a Lisp editor (README_LUA_FENNEL.md / the Fennel
// scripting UI plan):
//   - bracket matching + auto-close
//   - Parinfer (indent mode: reformats parens to match indentation after
//     every edit -- the "structural editing" that makes s-expressions
//     tolerable). Runs best-effort: a parse failure just leaves the buffer
//     alone rather than corrupting it.
//   - Fennel/Lisp syntax highlighting via the Clojure legacy-mode (close
//     enough for Fennel; a dedicated Fennel mode is a nice-to-have, not a
//     blocker per the plan).
//
// No build step for regular development: this file is loaded as a plain
// ESM module by both index.html shells, importing the pre-bundled vendor
// file directly.
//

import {
  EditorState, EditorView, keymap, lineNumbers, highlightActiveLine,
  drawSelection, dropCursor, rectangularSelection, highlightSpecialChars,
  history, defaultKeymap, historyKeymap, indentWithTab,
  bracketMatching, indentOnInput, syntaxHighlighting, defaultHighlightStyle,
  StreamLanguage, LanguageSupport, closeBrackets, closeBracketsKeymap,
  clojure, parinfer,
} from "../vendor/editor-bundle.mjs";

const fennelLanguage = new LanguageSupport(StreamLanguage.define(clojure));

// Venue-ready dark theme, matching web/console/styles.css's palette.
const darkTheme = EditorView.theme({
  "&": {
    color: "#e8ecf4",
    backgroundColor: "#12141c",
    fontSize: "14px",
    height: "100%",
  },
  ".cm-content": {
    fontFamily: "'SF Mono', 'Menlo', 'Consolas', monospace",
    caretColor: "#7fd0ff",
  },
  ".cm-gutters": {
    backgroundColor: "#0e1017",
    color: "#4a5468",
    border: "none",
  },
  ".cm-activeLine": { backgroundColor: "#1a1e2a" },
  ".cm-activeLineGutter": { backgroundColor: "#1a1e2a" },
  ".cm-selectionBackground, &.cm-focused .cm-selectionBackground": {
    backgroundColor: "#2a3f5f !important",
  },
  ".cm-matchingBracket": {
    backgroundColor: "#2f6a3f",
    outline: "1px solid #5fd07f",
  },
  ".cm-cursor": { borderLeftColor: "#7fd0ff" },
}, { dark: true });

// Locate the 0-based character offset for a 0-based (line, col) pair,
// clamped to the document's bounds. Used to remap the cursor after
// Parinfer reformats the buffer, since it reports position as line/col,
// not a flat offset.
function offsetForLineCol(text, line, col) {
  const lines = text.split("\n");
  const clampedLine = Math.max(0, Math.min(line, lines.length - 1));
  let offset = 0;
  for (let i = 0; i < clampedLine; i++) offset += lines[i].length + 1;
  const clampedCol = Math.max(0, Math.min(col, lines[clampedLine].length));
  return offset + clampedCol;
}

// Runs Parinfer's indent-mode over the whole buffer and, if it changed
// anything, replaces the document in place while trying to keep the
// cursor at the equivalent position. Best-effort: any exception or a
// reported failure (unbalanced string/comment, etc.) leaves the buffer
// untouched rather than risking corruption.
function runParinfer(view) {
  if (view.__parinferBusy) return;
  const state = view.state;
  const text = state.doc.toString();
  const head = state.selection.main.head;
  const cursorLineObj = state.doc.lineAt(head);
  const cursorLine = cursorLineObj.number - 1;
  const cursorX = head - cursorLineObj.from;

  let result;
  try {
    result = parinfer.indentMode(text, { cursorLine, cursorX });
  } catch {
    return;
  }
  if (!result || !result.success || result.text === text) return;

  const newHead = offsetForLineCol(
    result.text,
    result.cursorLine ?? cursorLine,
    result.cursorX ?? cursorX,
  );

  view.__parinferBusy = true;
  try {
    view.dispatch({
      changes: { from: 0, to: state.doc.length, insert: result.text },
      selection: { anchor: Math.max(0, Math.min(newHead, result.text.length)) },
    });
  } finally {
    view.__parinferBusy = false;
  }
}

const parinferPlugin = EditorView.updateListener.of((update) => {
  if (update.docChanged && !update.view.__parinferBusy) {
    runParinfer(update.view);
  }
});

// Lints for the two documented Fennel footguns (README_LUA_FENNEL.md's
// real-time-safety section): a constructed string in an effect body
// (allocates every frame) and an unbounded `while` loop (eats the frame
// instruction budget). This is a cheap regex pass, not a real parse --
// deliberately a hint, not a gate; false negatives are fine, false
// positives should stay rare given how narrow the patterns are.
export function lintFootguns(text) {
  const warnings = [];
  const lines = text.split("\n");
  lines.forEach((line, i) => {
    if (/\(\.\.\s/.test(line)) {
      warnings.push({ line: i + 1, message: "string concatenation (..) allocates every frame in an effect body" });
    }
    if (/\(while\b/.test(line)) {
      warnings.push({ line: i + 1, message: "while loop: make sure it can't run unbounded inside the frame instruction budget" });
    }
  });
  return warnings;
}

// Creates a CodeMirror EditorView mounted into `parent`.
//   doc          initial text
//   onChange(text)  called after every doc-changing transaction (already
//                    Parinfer-corrected, since the listener order below
//                    runs Parinfer first)
//   readOnly     defaults to false
//   extraKeymap  extra key bindings, highest precedence (e.g. Enter to
//                evaluate in the REPL input)
export function createFennelEditor({ parent, doc = "", onChange, readOnly = false, extraKeymap = [] }) {
  const extensions = [
    lineNumbers(),
    highlightActiveLine(),
    highlightSpecialChars(),
    drawSelection(),
    dropCursor(),
    rectangularSelection(),
    history(),
    bracketMatching(),
    closeBrackets(),
    indentOnInput(),
    syntaxHighlighting(defaultHighlightStyle, { fallback: true }),
    fennelLanguage,
    darkTheme,
    parinferPlugin,
    keymap.of([...closeBracketsKeymap, ...extraKeymap, ...defaultKeymap, ...historyKeymap, indentWithTab]),
    EditorView.editable.of(!readOnly),
    EditorView.lineWrapping,
  ];
  if (onChange) {
    extensions.push(EditorView.updateListener.of((update) => {
      if (update.docChanged) onChange(update.state.doc.toString());
    }));
  }

  const state = EditorState.create({ doc, extensions });
  return new EditorView({ state, parent });
}

// Replaces the whole document (e.g. loading a different script) and moves
// the cursor to the start.
export function setEditorDoc(view, text) {
  view.dispatch({
    changes: { from: 0, to: view.state.doc.length, insert: text },
    selection: { anchor: 0 },
  });
}

export function getEditorDoc(view) {
  return view.state.doc.toString();
}
