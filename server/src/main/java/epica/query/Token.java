// Token.java -- one lexical unit of a query.
//
// Kinds:
//   WORD    a bare identifier/keyword/value: PUT, user:42, 100
//   STRING  a double-quoted string with escapes resolved: "hello world"
//   NUMBER  digits only (used after LIMIT)
//   SEMI    ';' separates statements inside a BATCH
//   EOF     end of input
//
// Keywords are just WORDs compared case-insensitively by the parser, so a
// key literally named "get" is still usable as a quoted string.
package epica.query;

public record Token(Kind kind, String text, int position) {
  public enum Kind { WORD, STRING, NUMBER, SEMI, EOF }

  public boolean isWord(String keyword) {
    return kind == Kind.WORD && text.equalsIgnoreCase(keyword);
  }
}
