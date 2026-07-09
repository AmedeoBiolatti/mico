export type BorderStyle = {
  horizontal: string;
  vertical: string;
  topLeft: string;
  topRight: string;
  bottomLeft: string;
  bottomRight: string;
  teeLeft: string;
  teeRight: string;
  teeTop: string;
  teeBottom: string;
  cross: string;
};

export const singleBorder: BorderStyle = {
  horizontal: "\u2500",
  vertical: "\u2502",
  topLeft: "\u250c",
  topRight: "\u2510",
  bottomLeft: "\u2514",
  bottomRight: "\u2518",
  teeLeft: "\u251c",
  teeRight: "\u2524",
  teeTop: "\u252c",
  teeBottom: "\u2534",
  cross: "\u253c"
};

export const asciiBorder: BorderStyle = {
  horizontal: "-",
  vertical: "|",
  topLeft: "+",
  topRight: "+",
  bottomLeft: "+",
  bottomRight: "+",
  teeLeft: "+",
  teeRight: "+",
  teeTop: "+",
  teeBottom: "+",
  cross: "+"
};
