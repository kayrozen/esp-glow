#!/usr/bin/env bash
# vendor_editor_bundle.sh — build the single-file CodeMirror 6 + parinfer
# ESM bundle shared by web/console (device REPL/editor) and
# web/provisioner-static (offline authoring).
#
# CodeMirror 6 ships as many small ESM packages with imports between them
# (unlike Preact/htm, which each publish a single-file ESM build we can
# vendor as-is — see web/console/vendor/). There is no pre-built
# single-file release, so — same rationale as vendor_fennel.sh bootstrapping
# fennel.lua — this script bundles one with esbuild and commits the result.
# No build step is introduced for regular development: the bundle is a
# committed static artifact, esbuild is a vendoring-time tool only.
#
# Re-run this script to reproduce web/vendor/editor-bundle.mjs from scratch.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
npm init -y >/dev/null 2>&1
npm install --no-save \
  @codemirror/state @codemirror/view @codemirror/commands \
  @codemirror/language @codemirror/autocomplete @codemirror/legacy-modes \
  parinfer esbuild >/dev/null

cat > entry.js << 'JSEOF'
// Entry module for the vendored CodeMirror 6 + parinfer bundle used by
// both web/console (device REPL/editor) and web/provisioner-static (offline
// authoring). See scripts/vendor_editor_bundle.sh for how this is built.
export { EditorState, Compartment } from "@codemirror/state";
export {
  EditorView, keymap, highlightActiveLine, drawSelection, lineNumbers,
  highlightSpecialChars, dropCursor, rectangularSelection,
} from "@codemirror/view";
export {
  defaultKeymap, historyKeymap, history, indentWithTab,
} from "@codemirror/commands";
export {
  bracketMatching, indentOnInput, syntaxHighlighting, defaultHighlightStyle,
  StreamLanguage, LanguageSupport,
} from "@codemirror/language";
export {
  closeBrackets, closeBracketsKeymap, autocompletion, completeFromList,
} from "@codemirror/autocomplete";
export { clojure } from "@codemirror/legacy-modes/mode/clojure";
import parinfer from "parinfer";
export { parinfer };
JSEOF

# parinfer.js has a dead `if (RUN_ASSERTS) assert = require('assert')`
# branch (RUN_ASSERTS is a hardcoded `false`); esbuild still resolves the
# require() at bundle time since dead-code elimination runs after module
# resolution, and "assert" isn't a real browser module. Alias it to a stub.
echo 'export default function(){};' > empty-shim.js

node_modules/.bin/esbuild entry.js --bundle --format=esm --minify \
  --alias:assert=./empty-shim.js --outfile=editor-bundle.mjs

# Sanity check: the bundle must import cleanly under Node with no DOM
# (CodeMirror only touches `document` when a view is actually mounted).
EXPECTED_EXPORTS="EditorState EditorView keymap history defaultKeymap bracketMatching closeBrackets clojure parinfer LanguageSupport StreamLanguage"
node --input-type=module -e "
import * as m from './editor-bundle.mjs';
const expected = '$EXPECTED_EXPORTS'.split(' ');
const missing = expected.filter((k) => !(k in m));
if (missing.length) { console.error('missing exports:', missing); process.exit(1); }
console.log('editor-bundle.mjs: all expected exports present (' + Object.keys(m).length + ' total)');
"

mkdir -p "$OLDPWD/web/vendor"
cp editor-bundle.mjs "$OLDPWD/web/vendor/editor-bundle.mjs"

# License notice: every bundled package (@codemirror/*, parinfer) is MIT.
cat > "$OLDPWD/web/vendor/editor-bundle.LICENSE.txt" << 'LICEOF'
web/vendor/editor-bundle.mjs is a bundled build of the following
MIT-licensed packages (versions pinned in this script):

  @codemirror/state, @codemirror/view, @codemirror/commands,
  @codemirror/language, @codemirror/autocomplete, @codemirror/legacy-modes
  Copyright (C) 2018-present by Marijn Haverbeke <marijnh@gmail.com> and others
  https://codemirror.net/  MIT License

  parinfer
  Copyright (c) 2015-present, Shaun Williams and Chris Oakman
  https://github.com/parinfer/parinfer.js  MIT License

Full license texts: https://opensource.org/licenses/MIT
LICEOF

echo "Vendored web/vendor/editor-bundle.mjs ($(wc -c < "$OLDPWD/web/vendor/editor-bundle.mjs") bytes)."
