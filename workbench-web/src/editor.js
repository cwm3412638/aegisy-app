import * as monaco from "monaco-editor/esm/vs/editor/editor.api.js";
import "monaco-editor/esm/vs/basic-languages/cpp/cpp.contribution.js";
import "monaco-editor/esm/vs/basic-languages/csharp/csharp.contribution.js";
import "monaco-editor/esm/vs/basic-languages/go/go.contribution.js";
import "monaco-editor/esm/vs/basic-languages/java/java.contribution.js";
import "monaco-editor/esm/vs/basic-languages/markdown/markdown.contribution.js";
import "monaco-editor/esm/vs/basic-languages/python/python.contribution.js";
import "monaco-editor/esm/vs/basic-languages/rust/rust.contribution.js";
import "monaco-editor/esm/vs/basic-languages/shell/shell.contribution.js";
import "monaco-editor/esm/vs/basic-languages/sql/sql.contribution.js";
import "monaco-editor/esm/vs/basic-languages/xml/xml.contribution.js";
import "monaco-editor/esm/vs/basic-languages/yaml/yaml.contribution.js";
import "monaco-editor/esm/vs/language/css/monaco.contribution.js";
import "monaco-editor/esm/vs/language/html/monaco.contribution.js";
import "monaco-editor/esm/vs/language/json/monaco.contribution.js";
import "monaco-editor/esm/vs/language/typescript/monaco.contribution.js";
import "./style.css";

const workerForLabel = (label) => {
  if (label === "json") return "json.worker.js";
  if (label === "css" || label === "scss" || label === "less") return "css.worker.js";
  if (label === "html" || label === "handlebars" || label === "razor") return "html.worker.js";
  if (label === "typescript" || label === "javascript") return "ts.worker.js";
  return "editor.worker.js";
};

self.MonacoEnvironment = {
  getWorker(_moduleId, label) {
    return new Worker(`./${workerForLabel(label)}`);
  }
};

const languageForPath = (path) => {
  const extension = path.includes(".") ? path.slice(path.lastIndexOf(".") + 1).toLowerCase() : "";
  const languages = {
    c: "c", h: "cpp", cc: "cpp", cpp: "cpp", cxx: "cpp", hpp: "cpp",
    cs: "csharp", css: "css", go: "go", html: "html", htm: "html",
    java: "java", js: "javascript", jsx: "javascript", json: "json",
    md: "markdown", py: "python", rs: "rust", sh: "shell", sql: "sql",
    ts: "typescript", tsx: "typescript", xml: "xml", yaml: "yaml", yml: "yaml"
  };
  return languages[extension] || "plaintext";
};

const models = new Map();
const groupPaths = ["", ""];
let bridge = null;
let activeGroup = 0;
let splitEnabled = false;
let applyingHostState = false;
const contentTimers = [0, 0];
const viewTimers = [0, 0];

monaco.editor.defineTheme("aegisy-light", {
  base: "vs",
  inherit: true,
  rules: [
    { token: "keyword", foreground: "174EA6", fontStyle: "bold" },
    { token: "string", foreground: "067647" },
    { token: "number", foreground: "B54708" },
    { token: "comment", foreground: "7A8699", fontStyle: "italic" },
    { token: "type", foreground: "7A3E9D" }
  ],
  colors: {
    "editor.background": "#FFFFFF",
    "editor.foreground": "#182230",
    "editorLineNumber.foreground": "#98A2B3",
    "editorLineNumber.activeForeground": "#475467",
    "editor.selectionBackground": "#C8D8FF",
    "editor.inactiveSelectionBackground": "#E8EFFF",
    "editor.lineHighlightBackground": "#F8FAFC",
    "editorCursor.foreground": "#165DFF",
    "editorIndentGuide.background1": "#EEF2F6",
    "editorIndentGuide.activeBackground1": "#C8D8FF",
    "focusBorder": "#84A8FF"
  }
});

const createEditor = (group) => monaco.editor.create(document.getElementById(`editor-${group}`), {
  automaticLayout: true,
  fontFamily: "Menlo, Monaco, Consolas, monospace",
  fontSize: 12,
  lineHeight: 20,
  minimap: { enabled: false },
  padding: { top: 10, bottom: 10 },
  renderWhitespace: "selection",
  scrollBeyondLastLine: false,
  smoothScrolling: false,
  tabSize: 4,
  theme: "aegisy-light",
  wordWrap: "off"
});

const editors = [createEditor(0), createEditor(1)];
const validGroup = (group) => group === 1 ? 1 : 0;

const layoutEditors = () => {
  editors.forEach((editor, group) => {
    const host = document.getElementById(`editor-${group}`);
    if (host.clientWidth > 0 && host.clientHeight > 0) {
      editor.layout({ width: host.clientWidth, height: host.clientHeight });
      editor.render(true);
    }
  });
};

const markActiveGroup = (group, notify = true) => {
  const next = validGroup(group);
  if (next === 1 && !splitEnabled) return;
  activeGroup = next;
  document.getElementById("editor-group-0").classList.toggle("is-active", next === 0);
  document.getElementById("editor-group-1").classList.toggle("is-active", next === 1);
  if (notify && bridge && groupPaths[next]) bridge.groupActivated(next, groupPaths[next]);
};

const emitContent = (group) => {
  const index = validGroup(group);
  const editor = editors[index];
  const path = groupPaths[index];
  if (!bridge || applyingHostState || !path || !editor.getModel()) return;
  const selection = editor.getSelection();
  const model = editor.getModel();
  bridge.contentChanged(
    path,
    model.getValue(),
    model.getOffsetAt(selection.getPosition()),
    model.getOffsetAt(selection.getSelectionStart())
  );
};

const emitView = (group) => {
  const index = validGroup(group);
  const editor = editors[index];
  const path = groupPaths[index];
  if (!bridge || applyingHostState || !path || !editor.getModel()) return;
  const selection = editor.getSelection();
  const model = editor.getModel();
  bridge.viewChanged(
    index,
    path,
    model.getOffsetAt(selection.getPosition()),
    model.getOffsetAt(selection.getSelectionStart()),
    Math.round(editor.getScrollTop()),
    Math.round(editor.getScrollLeft())
  );
};

editors.forEach((editor, group) => {
  editor.onDidChangeModelContent(() => {
    clearTimeout(contentTimers[group]);
    contentTimers[group] = setTimeout(() => emitContent(group), 90);
  });
  editor.onDidChangeCursorSelection(() => {
    clearTimeout(viewTimers[group]);
    viewTimers[group] = setTimeout(() => emitView(group), 50);
  });
  editor.onDidScrollChange(() => {
    clearTimeout(viewTimers[group]);
    viewTimers[group] = setTimeout(() => emitView(group), 50);
  });
  editor.onDidFocusEditorWidget(() => markActiveGroup(group));
  editor.addCommand(monaco.KeyMod.CtrlCmd | monaco.KeyCode.KeyS, () => {
    if (!bridge || !groupPaths[group]) return;
    markActiveGroup(group);
    clearTimeout(contentTimers[group]);
    emitContent(group);
    bridge.saveRequested(group, groupPaths[group]);
  });
});

const applyModel = (group, path, content, readOnly,
                    cursorOffset, anchorOffset, scrollTop, scrollLeft) => {
  const index = validGroup(group);
  const editor = editors[index];
  clearTimeout(contentTimers[index]);
  clearTimeout(viewTimers[index]);
  emitContent(index);
  emitView(index);
  applyingHostState = true;
  try {
    groupPaths[index] = path;
    let model = models.get(path);
    if (!model) {
      const uri = monaco.Uri.parse(`aegisy-workspace://model/${encodeURIComponent(path)}`);
      model = monaco.editor.createModel(content, languageForPath(path), uri);
      models.set(path, model);
    } else if (model.getValue() !== content) {
      model.setValue(content);
    }
    editor.setModel(model);
    editor.updateOptions({ readOnly });
    const cursor = model.getPositionAt(Math.max(0, Math.min(cursorOffset, content.length)));
    const anchor = model.getPositionAt(Math.max(0, Math.min(anchorOffset, content.length)));
    editor.setSelection(new monaco.Selection(
      anchor.lineNumber, anchor.column, cursor.lineNumber, cursor.column));
    editor.setScrollPosition({ scrollTop, scrollLeft });
    requestAnimationFrame(layoutEditors);
  } finally {
    applyingHostState = false;
  }
};

const setSplitEnabled = (enabled) => {
  splitEnabled = Boolean(enabled);
  const workspace = document.getElementById("editor-workspace");
  const secondary = document.getElementById("editor-group-1");
  workspace.classList.toggle("is-split", splitEnabled);
  secondary.hidden = !splitEnabled;
  if (!splitEnabled && activeGroup === 1) markActiveGroup(0);
  layoutEditors();
  requestAnimationFrame(layoutEditors);
  setTimeout(layoutEditors, 50);
};

const closeModel = (path) => {
  const model = models.get(path);
  if (!model) return;
  editors.forEach((editor, group) => {
    if (groupPaths[group] !== path) return;
    editor.setModel(null);
    groupPaths[group] = "";
  });
  model.dispose();
  models.delete(path);
};

self.aegisyEditorTest = {
  getValue: (group = activeGroup) => editors[validGroup(group)].getModel()?.getValue() || "",
  setValue: (value, group = activeGroup) => editors[validGroup(group)].getModel()?.setValue(value),
  getLanguage: (group = activeGroup) => editors[validGroup(group)].getModel()?.getLanguageId() || "",
  getPath: (group = activeGroup) => groupPaths[validGroup(group)],
  getActiveGroup: () => activeGroup,
  isSplit: () => splitEnabled,
  getDimensions: (group) => {
    const host = document.getElementById(`editor-${validGroup(group)}`);
    return { width: host.clientWidth, height: host.clientHeight };
  },
  focusGroup: (group) => {
    const index = validGroup(group);
    markActiveGroup(index);
    editors[index].focus();
  }
};

new QWebChannel(qt.webChannelTransport, (channel) => {
  bridge = channel.objects.aegisyEditor;
  bridge.modelActivated.connect(applyModel);
  bridge.splitEnabledChanged.connect(setSplitEnabled);
  bridge.editorGroupFocusRequested.connect((group) => {
    const index = validGroup(group);
    markActiveGroup(index, false);
    editors[index].focus();
  });
  bridge.modelReadOnlyChanged.connect((path, readOnly) => {
    editors.forEach((editor, group) => {
      if (path === groupPaths[group]) editor.updateOptions({ readOnly });
    });
  });
  bridge.modelClosed.connect(closeModel);
  bridge.allModelsClosed.connect(() => {
    for (const path of [...models.keys()]) closeModel(path);
  });
  bridge.contentRequested.connect((group) => {
    const index = validGroup(group);
    clearTimeout(contentTimers[index]);
    emitContent(index);
    if (groupPaths[index]) bridge.saveRequested(index, groupPaths[index]);
  });
  document.getElementById("loading").hidden = true;
  bridge.ready();
});
