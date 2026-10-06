// Lexer.java -- turns a query line into tokens.
//
// Example:   PUT user:1 "Ada Lovelace"; DEL user:2
//   -> WORD(PUT) WORD(user:1) STRING(Ada Lovelace) SEMI WORD(DEL) WORD(user:2) EOF
//
// Quoted strings support \" \\ \n \t \0 escapes so binary-ish values can be
// typed. Anything else is a WORD delimited by whitespace or ';'. A run of
// digits is a NUMBER (the parser also accepts a NUMBER where a key/value is
// expected, so "PUT 42 100" works).
package epica.query;

import java.util.ArrayList;
import java.util.List;

public final class Lexer {
  private final String src;
  private int pos = 0;

  public Lexer(String src) { this.src = src; }

  public static List<Token> tokenize(String src) {
    Lexer lx = new Lexer(src);
    List<Token> out = new ArrayList<>();
    Token t;
    do {
      t = lx.next();
      out.add(t);
    } while (t.kind() != Token.Kind.EOF);
    return out;
  }

  public Token next() {
    while (pos < src.length() && Character.isWhitespace(src.charAt(pos))) pos++;
    if (pos >= src.length()) return new Token(Token.Kind.EOF, "", pos);
    int start = pos;
    char c = src.charAt(pos);
    if (c == ';') {
      pos++;
      return new Token(Token.Kind.SEMI, ";", start);
    }
    if (c == '"') return quoted(start);
    StringBuilder sb = new StringBuilder();
    boolean allDigits = true;
    while (pos < src.length()) {
      c = src.charAt(pos);
      if (Character.isWhitespace(c) || c == ';') break;
      if (c == '"') throw new QueryException("unexpected quote inside word at " + pos);
      if (!Character.isDigit(c)) allDigits = false;
      sb.append(c);
      pos++;
    }
    return new Token(allDigits ? Token.Kind.NUMBER : Token.Kind.WORD, sb.toString(), start);
  }

  private Token quoted(int start) {
    pos++;  // opening quote
    StringBuilder sb = new StringBuilder();
    while (true) {
      if (pos >= src.length()) throw new QueryException("unterminated string starting at " + start);
      char c = src.charAt(pos++);
      if (c == '"') break;
      if (c == '\\') {
        if (pos >= src.length()) throw new QueryException("dangling backslash at " + (pos - 1));
        char e = src.charAt(pos++);
        switch (e) {
          case 'n' -> sb.append('\n');
          case 't' -> sb.append('\t');
          case '0' -> sb.append('\0');
          case '"' -> sb.append('"');
          case '\\' -> sb.append('\\');
          default -> throw new QueryException("unknown escape \\" + e + " at " + (pos - 2));
        }
      } else {
        sb.append(c);
      }
    }
    return new Token(Token.Kind.STRING, sb.toString(), start);
  }
}
