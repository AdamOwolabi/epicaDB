// LexerTest.java -- tokenisation, quoting and escapes.
package epica.query;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.List;
import org.junit.jupiter.api.Test;

class LexerTest {
  @Test
  void wordsNumbersAndSemicolons() {
    List<Token> t = Lexer.tokenize("PUT user:1 42; DEL x");
    assertEquals(Token.Kind.WORD, t.get(0).kind());
    assertEquals("PUT", t.get(0).text());
    assertEquals("user:1", t.get(1).text());
    assertEquals(Token.Kind.NUMBER, t.get(2).kind());
    assertEquals(Token.Kind.SEMI, t.get(3).kind());
    assertEquals("DEL", t.get(4).text());
    assertEquals(Token.Kind.EOF, t.get(6).kind());
  }

  @Test
  void quotedStringsWithEscapes() {
    List<Token> t = Lexer.tokenize("PUT k \"hello \\\"world\\\"\\n\"");
    assertEquals(Token.Kind.STRING, t.get(2).kind());
    assertEquals("hello \"world\"\n", t.get(2).text());
  }

  @Test
  void keywordsAreCaseInsensitiveButValuesKeepCase() {
    List<Token> t = Lexer.tokenize("get MixedCase");
    assertEquals(true, t.get(0).isWord("GET"));
    assertEquals("MixedCase", t.get(1).text());
  }

  @Test
  void unterminatedStringIsAnError() {
    assertThrows(QueryException.class, () -> Lexer.tokenize("PUT k \"oops"));
  }
}
