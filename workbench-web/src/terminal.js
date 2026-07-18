import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import "./terminal.css";

const host = document.getElementById("terminal");
const loading = document.getElementById("loading");
const fitAddon = new FitAddon();
const terminal = new Terminal({
  allowProposedApi: false,
  convertEol: false,
  cursorBlink: true,
  cursorStyle: "block",
  fontFamily: "Menlo, Monaco, Consolas, monospace",
  fontSize: 12,
  lineHeight: 1.2,
  scrollback: 10000,
  theme: {
    background: "#101828",
    foreground: "#d0d5dd",
    cursor: "#84a8ff",
    cursorAccent: "#101828",
    selectionBackground: "#2e5aac99",
    black: "#101828",
    red: "#f97066",
    green: "#32d583",
    yellow: "#fdb022",
    blue: "#84a8ff",
    magenta: "#c59cff",
    cyan: "#5fe9d0",
    white: "#f2f4f7",
    brightBlack: "#667085",
    brightRed: "#fda29b",
    brightGreen: "#6ce9a6",
    brightYellow: "#fec84b",
    brightBlue: "#b2ccff",
    brightMagenta: "#d6bbfb",
    brightCyan: "#99f6e4",
    brightWhite: "#ffffff"
  }
});
terminal.loadAddon(fitAddon);
terminal.open(host);

let bridge = null;
let inputEnabled = false;
let resizeTimer = 0;
let lastRows = 0;
let lastCols = 0;

const fit = () => {
  if (!host.clientWidth || !host.clientHeight) return;
  try {
    fitAddon.fit();
  } catch (_error) {
    return;
  }
  if (!bridge || (terminal.rows === lastRows && terminal.cols === lastCols)) return;
  lastRows = terminal.rows;
  lastCols = terminal.cols;
  bridge.resized(lastRows, lastCols);
};

const scheduleFit = () => {
  window.clearTimeout(resizeTimer);
  resizeTimer = window.setTimeout(fit, 40);
};

const decodeBase64 = (base64) => {
  const binary = window.atob(base64);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) {
    bytes[index] = binary.charCodeAt(index);
  }
  return bytes;
};

terminal.onData((data) => {
  if (bridge && inputEnabled) bridge.input(data);
});
terminal.onSelectionChange(() => {
  if (bridge) bridge.selectionChanged(terminal.getSelection());
});
terminal.attachCustomKeyEventHandler((event) => {
  if (event.type !== "keydown" || !bridge) return true;
  const modifier = navigator.platform.toLowerCase().includes("mac")
    ? event.metaKey
    : event.ctrlKey;
  if (!modifier) return true;
  const key = event.key.toLowerCase();
  if (key === "c" && terminal.hasSelection()) {
    bridge.copy(terminal.getSelection());
    return false;
  }
  if (key === "v" && inputEnabled) {
    bridge.paste();
    return false;
  }
  return true;
});
new ResizeObserver(scheduleFit).observe(host);
window.addEventListener("resize", scheduleFit);

new QWebChannel(qt.webChannelTransport, (channel) => {
  bridge = channel.objects.aegisyTerminal;
  bridge.resetRequested.connect((_generation) => {
    terminal.reset();
    terminal.clear();
    terminal.focus();
  });
  bridge.outputReceived.connect((base64) => {
    if (base64) terminal.write(decodeBase64(base64));
  });
  bridge.inputEnabledChanged.connect((enabled) => {
    inputEnabled = enabled;
    terminal.options.cursorBlink = enabled;
  });
  bridge.focusRequested.connect(() => terminal.focus());
  bridge.pasteReceived.connect((text) => {
    if (text && inputEnabled) bridge.input(text);
  });
  loading.hidden = true;
  scheduleFit();
  bridge.ready();
});

window.aegisyTerminalTest = {
  isReady: () => bridge !== null && loading.hidden,
  getDimensions: () => ({
    width: host.clientWidth,
    height: host.clientHeight,
    rows: terminal.rows,
    cols: terminal.cols
  }),
  getText: () => {
    const lines = [];
    const buffer = terminal.buffer.active;
    for (let index = 0; index < buffer.length; index += 1) {
      lines.push(buffer.getLine(index)?.translateToString(true) || "");
    }
    return lines.join("\n");
  },
  selectAllAndCopy: () => {
    terminal.selectAll();
    if (bridge && terminal.hasSelection()) bridge.copy(terminal.getSelection());
  },
  requestPaste: () => {
    if (bridge && inputEnabled) bridge.paste();
  }
};
