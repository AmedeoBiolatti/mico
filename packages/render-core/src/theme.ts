export type AnsiPalette = {
  black: string;
  red: string;
  green: string;
  yellow: string;
  blue: string;
  magenta: string;
  cyan: string;
  white: string;
  brightBlack: string;
  brightRed: string;
  brightGreen: string;
  brightYellow: string;
  brightBlue: string;
  brightMagenta: string;
  brightCyan: string;
  brightWhite: string;
};

export type Theme = {
  id: string;
  background: string;
  foreground: string;
  muted: string;
  border: string;
  panel: string;
  panelActive: string;
  accent: string;
  danger: string;
  warning: string;
  success: string;
  selection: string;
  focus: string;
  ansi: AnsiPalette;
};

export const draculaTheme: Theme = {
  id: "dracula",
  background: "#282a36",
  foreground: "#f8f8f2",
  muted: "#6272a4",
  border: "#44475a",
  panel: "#21222c",
  panelActive: "#343746",
  accent: "#bd93f9",
  danger: "#ff5555",
  warning: "#ffb86c",
  success: "#50fa7b",
  selection: "#44475a",
  focus: "#8be9fd",
  ansi: {
    black: "#21222c",
    red: "#ff5555",
    green: "#50fa7b",
    yellow: "#f1fa8c",
    blue: "#bd93f9",
    magenta: "#ff79c6",
    cyan: "#8be9fd",
    white: "#f8f8f2",
    brightBlack: "#6272a4",
    brightRed: "#ff6e6e",
    brightGreen: "#69ff94",
    brightYellow: "#ffffa5",
    brightBlue: "#d6acff",
    brightMagenta: "#ff92df",
    brightCyan: "#a4ffff",
    brightWhite: "#ffffff"
  }
};

export const micoDarkTheme: Theme = {
  ...draculaTheme,
  id: "mico-dark",
  background: "#15161e",
  panel: "#15161e",
  panelActive: "#1f2130",
  border: "#3b3f54",
  muted: "#6d739e",
  selection: "#2c2f42"
};
